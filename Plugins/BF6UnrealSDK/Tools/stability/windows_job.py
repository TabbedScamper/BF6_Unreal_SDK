"""Windows process-tree resource controls. No third-party dependencies."""
import ctypes as C
from ctypes import wintypes as W
import os
import re
import subprocess

K = C.WinDLL("kernel32", use_last_error=True)
SIZE = C.c_size_t


def api(name, result, *args):
    fn = getattr(K, name)
    fn.restype, fn.argtypes = result, args
    return fn


close = api("CloseHandle", W.BOOL, W.HANDLE)


def checked(value):
    if not value:
        raise C.WinError(C.get_last_error())
    return value


class Basic(C.Structure):
    _fields_ = [("process_time", C.c_int64), ("job_time", C.c_int64),
                ("flags", W.DWORD), ("min_ws", SIZE), ("max_ws", SIZE),
                ("active_limit", W.DWORD), ("affinity", SIZE),
                ("priority", W.DWORD), ("scheduling", W.DWORD)]


class Limits(C.Structure):
    _fields_ = [("basic", Basic), ("io", C.c_uint64 * 6),
                ("process_memory", SIZE), ("job_memory", SIZE),
                ("peak_process", SIZE), ("peak_job", SIZE)]


class Startup(C.Structure):
    _fields_ = [("cb", W.DWORD), ("reserved", W.LPWSTR),
                ("desktop", W.LPWSTR), ("title", W.LPWSTR),
                ("x", W.DWORD), ("y", W.DWORD), ("w", W.DWORD),
                ("h", W.DWORD), ("xc", W.DWORD), ("yc", W.DWORD),
                ("fill", W.DWORD), ("flags", W.DWORD), ("show", W.WORD),
                ("reserved2_size", W.WORD), ("reserved2", C.c_void_p),
                ("stdin", W.HANDLE), ("stdout", W.HANDLE), ("stderr", W.HANDLE)]


class ProcessInfo(C.Structure):
    _fields_ = [("process", W.HANDLE), ("thread", W.HANDLE),
                ("pid", W.DWORD), ("tid", W.DWORD)]


class Memory(C.Structure):
    _fields_ = [("cb", W.DWORD), ("faults", W.DWORD)] + [
        (n, SIZE) for n in ("peak_ws", "ws", "peak_paged", "paged",
                           "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile", "private")]


def physical_core_mask(count):
    """One logical processor per physical core; fail rather than guess topology."""
    if (os.cpu_count() or 1) > 64:
        raise ValueError("Processor-group systems over 64 logical CPUs are not supported yet")
    fn = api("GetLogicalProcessorInformationEx", W.BOOL, C.c_int, C.c_void_p, C.POINTER(W.DWORD))
    size = W.DWORD()
    fn(0, None, C.byref(size))  # RelationProcessorCore
    buffer = C.create_string_buffer(size.value)
    checked(fn(0, buffer, C.byref(size)))
    offset, cores = 0, []
    while offset < size.value:
        row = buffer.raw[offset:]
        length = int.from_bytes(row[4:8], "little")
        if length < 48:
            raise ValueError("Invalid processor topology record")
        groups = int.from_bytes(row[30:32], "little")
        if groups != 1 or int.from_bytes(row[40:42], "little") != 0:
            raise ValueError("Multi-group CPU topology is not supported")
        mask = int.from_bytes(row[32:40], "little")
        cores.append(mask & -mask)
        offset += length
    if count > len(cores):
        raise ValueError(f"Requested {count} physical cores, only {len(cores)} available")
    return sum(cores[:count]) if count else 0


class Job:
    def __init__(self, cores=0, cpu_percent=0, memory_gib=0):
        self.handle = checked(api("CreateJobObjectW", W.HANDLE, C.c_void_p, W.LPCWSTR)(None, None))
        self.process = None
        self.mask = physical_core_mask(cores)
        self.limits = Limits()
        self.limits.basic.flags = 0x2000  # KILL_ON_JOB_CLOSE, includes descendants
        if cores:
            self.limits.basic.flags |= 0x10
            self.limits.basic.affinity = self.mask
        if memory_gib:
            self.limits.basic.flags |= 0x200
            self.limits.job_memory = int(memory_gib * 1024**3)
        self._set(9, self.limits)
        # This is explicitly percent of TOTAL system CPU, independent of affinity.
        # Do not claim that a CPU quota reproduces a particular processor model.
        if cpu_percent:
            rate = (W.DWORD * 2)(5, round(cpu_percent * 100))
            self._set(15, rate)

    def _set(self, kind, value):
        checked(api("SetInformationJobObject", W.BOOL, W.HANDLE, C.c_int, C.c_void_p, W.DWORD)(
            self.handle, kind, C.byref(value), C.sizeof(value)))

    def launch(self, argv, cwd, env, visible=False):
        startup, info = Startup(), ProcessInfo()
        startup.cb = C.sizeof(startup)
        startup.flags, startup.show = 1, 1 if visible else 0
        block = C.create_unicode_buffer("\0".join(f"{k}={v}" for k, v in sorted(env.items(), key=lambda kv: kv[0].upper())) + "\0\0")
        command = C.create_unicode_buffer(subprocess.list2cmdline([str(a) for a in argv]))
        create = api("CreateProcessW", W.BOOL, W.LPCWSTR, W.LPWSTR, C.c_void_p, C.c_void_p,
                     W.BOOL, W.DWORD, C.c_void_p, W.LPCWSTR, C.POINTER(Startup), C.POINTER(ProcessInfo))
        checked(create(str(argv[0]), command, None, None, False, 0x4 | 0x400,
                       block, str(cwd), C.byref(startup), C.byref(info)))
        self.process, self.pid = info.process, info.pid
        try:
            # Assign BEFORE resuming: shader/CEF/decode workers cannot escape the limits.
            checked(api("AssignProcessToJobObject", W.BOOL, W.HANDLE, W.HANDLE)(self.handle, info.process))
            if api("ResumeThread", W.DWORD, W.HANDLE)(info.thread) == 0xffffffff:
                raise C.WinError(C.get_last_error())
        except BaseException:
            api("TerminateProcess", W.BOOL, W.HANDLE, W.UINT)(info.process, 1)
            raise
        finally:
            close(info.thread)

    def poll(self):
        result = api("WaitForSingleObject", W.DWORD, W.HANDLE, W.DWORD)(self.process, 0)
        if result == 258:
            return None
        code = W.DWORD()
        checked(api("GetExitCodeProcess", W.BOOL, W.HANDLE, C.POINTER(W.DWORD))(self.process, C.byref(code)))
        return code.value

    def sample(self):
        query = api("QueryInformationJobObject", W.BOOL, W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.c_void_p)
        data = C.create_string_buffer(8 + 4096 * C.sizeof(SIZE))
        checked(query(self.handle, 3, data, C.sizeof(data), None))
        count = int.from_bytes(data.raw[4:8], "little")
        pids = list((SIZE * count).from_buffer(data, 8))
        private, working = 0, 0
        get_memory = api("K32GetProcessMemoryInfo", W.BOOL, W.HANDLE, C.POINTER(Memory), W.DWORD)
        for pid in pids:
            handle = api("OpenProcess", W.HANDLE, W.DWORD, W.BOOL, W.DWORD)(0x410, False, pid)
            if handle:
                try:
                    mem = Memory()
                    mem.cb = C.sizeof(mem)
                    if get_memory(handle, C.byref(mem), mem.cb):
                        private += mem.private
                        working += mem.ws
                finally:
                    close(handle)
        limits = Limits()
        checked(query(self.handle, 9, C.byref(limits), C.sizeof(limits), None))
        return dict(pids=pids, private_bytes=private, summed_working_set_bytes=working,
                    peak_job_commit_bytes=limits.peak_job)

    def close(self):
        if self.handle:
            close(self.handle)
            self.handle = None
        if self.process:
            close(self.process)
            self.process = None


class GPUCounters:
    """WDDM per-process usage, across vendors. Missing data stays null, never zero."""
    def __init__(self):
        self.dll = C.WinDLL("pdh")
        self.query = W.HANDLE()
        self.counters = {}
        self.error = None
        try:
            self._call("PdhOpenQueryW", [W.LPCWSTR, SIZE, C.POINTER(W.HANDLE)], None, 0, C.byref(self.query))
            for name in ("Dedicated", "Shared"):
                counter = W.HANDLE()
                self._call("PdhAddEnglishCounterW", [W.HANDLE, W.LPCWSTR, SIZE, C.POINTER(W.HANDLE)],
                           self.query, f"\\GPU Process Memory(*)\\{name} Usage", 0, C.byref(counter))
                self.counters[name.lower() + "_bytes"] = counter
        except OSError as exc:
            self.error = str(exc)

    def _call(self, name, types, *args):
        fn = getattr(self.dll, name)
        fn.argtypes, fn.restype = types, W.DWORD
        result = fn(*args)
        if result:
            raise OSError(f"{name}: 0x{result:08x}")

    def sample(self, pids):
        result = dict(dedicated_bytes=None, shared_bytes=None)
        if self.error:
            return result
        try:
            self._call("PdhCollectQueryData", [W.HANDLE], self.query)
            class Value(C.Structure):
                _fields_ = [("status", W.DWORD), ("value", C.c_double)]
            class Item(C.Structure):
                _fields_ = [("name", W.LPWSTR), ("value", Value)]
            fn = self.dll.PdhGetFormattedCounterArrayW
            fn.restype, fn.argtypes = W.DWORD, [W.HANDLE, W.DWORD, C.POINTER(W.DWORD), C.POINTER(W.DWORD), C.c_void_p]
            for name, counter in self.counters.items():
                size, count = W.DWORD(), W.DWORD()
                code = fn(counter, 0x200, C.byref(size), C.byref(count), None)
                if code != 0x800007d2 or not size.value:
                    continue
                data = C.create_string_buffer(size.value)
                if fn(counter, 0x200, C.byref(size), C.byref(count), data):
                    continue
                values = []
                for item in (Item * count.value).from_buffer(data):
                    match = re.search(r"pid_(\d+)_", item.name or "")
                    if match and int(match[1]) in pids and item.value.status in (0, 1):
                        values.append(item.value.value)
                if values:
                    result[name] = int(sum(values))
        except OSError:
            pass  # A transient PDH failure is an unavailable sample.
        return result

    def close(self):
        if self.query:
            self._call("PdhCloseQuery", [W.HANDLE], self.query)
            self.query = None


def foreground_pid():
    user = C.WinDLL("user32")
    user.GetForegroundWindow.restype = W.HWND
    user.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
    pid = W.DWORD()
    window = user.GetForegroundWindow()
    if window:
        user.GetWindowThreadProcessId(window, C.byref(pid))
    return pid.value


def editor_windows(pid):
    """Actual visible window/client dimensions; launch ResX/ResY can be overridden by the SDK."""
    user = C.WinDLL("user32")
    callback_type = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    user.EnumWindows.argtypes = [callback_type, W.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
    user.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
    user.IsWindowVisible.argtypes = [W.HWND]
    user.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
    user.GetWindowRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
    result = []
    def visit(window, param):
        owner = W.DWORD()
        user.GetWindowThreadProcessId(window, C.byref(owner))
        if owner.value == pid and user.IsWindowVisible(window):
            title = C.create_unicode_buffer(512)
            user.GetWindowTextW(window, title, len(title))
            if title.value == "BF6 Unreal SDK":
                bounds, client = W.RECT(), W.RECT()
                user.GetWindowRect(window, C.byref(bounds)); user.GetClientRect(window, C.byref(client))
                result.append(dict(title=title.value, width=bounds.right-bounds.left, height=bounds.bottom-bounds.top,
                    client_width=client.right-client.left, client_height=client.bottom-client.top))
        return True
    user.EnumWindows(callback_type(visit), 0)
    return result


def show_editor(pid):
    """One best-effort focus attempt for an explicitly visible benchmark."""
    user = C.WinDLL("user32")
    callback_type = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    user.EnumWindows.argtypes = [callback_type, W.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
    user.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
    user.ShowWindow.argtypes = [W.HWND, C.c_int]
    user.SetForegroundWindow.argtypes = [W.HWND]
    matches = []
    def visit(window, param):
        owner = W.DWORD()
        user.GetWindowThreadProcessId(window, C.byref(owner))
        if owner.value == pid:
            title = C.create_unicode_buffer(512)
            user.GetWindowTextW(window, title, len(title))
            if title.value == "BF6 Unreal SDK":
                matches.append(window)
        return True
    user.EnumWindows(callback_type(visit), 0)
    if matches:
        user.ShowWindow(matches[0], 9)
        user.SetForegroundWindow(matches[0])
