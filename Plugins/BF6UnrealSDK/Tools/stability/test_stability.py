"""Real Windows process-tree tests plus verdict regression tests. No Unreal needed."""
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
sys.dont_write_bytecode = True
from windows_job import Job, GPUCounters, api, close
from ctypes import wintypes as W
from report import evaluate, frames_summary
from run import engine_cpu_arguments


class Verdicts(unittest.TestCase):
    def test_affinity_hint_requires_matching_topology(self):
        self.assertEqual(engine_cpu_arguments(6, 0x555, "profile"),
                         ["-corelimit=6", "-processaffinityphysical=6"])
        self.assertEqual(engine_cpu_arguments(6, 0x3f, "profile"), ["-corelimit=6"])
        self.assertEqual(engine_cpu_arguments(6, 0x555, "host"), ["-processaffinityphysical=6"])
        self.assertEqual(engine_cpu_arguments(0, 0, "profile"), [])

    def test_early_crash_cannot_pass_gpu_budget(self):
        samples = [dict(gpu=dict(dedicated_bytes=1024**3), peak_job_commit_bytes=0)]
        r = evaluate(None, samples, 3, None, True, 4.5)
        self.assertEqual(r["gpu_budget"], "NOT_TESTED")
        samples[0]["gpu"]["dedicated_bytes"] = 8 * 1024**3
        r = evaluate(None, samples, 3, None, True, 4.5)
        self.assertEqual(r["gpu_budget"], "FAIL")

    def test_missing_workflow_is_failure(self):
        self.assertEqual(evaluate(None, [], 0, None, True, 4.5)["stability"], "FAIL")

    def test_missing_frames_never_pass(self):
        samples = [dict(monotonic=x, foreground=True, gpu=dict(dedicated_bytes=None), peak_job_commit_bytes=0) for x in (1, 2)]
        r = evaluate(dict(completed=True, flights=[dict(start=0, end=3, frames=frames_summary([]))]), samples, 0, None, True, 4.5)
        self.assertEqual(r["flight_60fps"], "FAIL")
        self.assertEqual(r["gpu_budget"], "NOT_MEASURED")

    def test_hitch_is_kept(self):
        summary = frames_summary([10] * 90 + [300] * 10)
        self.assertEqual(summary["p95_ms"], 300)
        self.assertEqual(summary["over_100ms"], 10)

    def test_crash_after_success_fails(self):
        r = evaluate(dict(completed=True), [], 0xc0000005, None, True, 4.5)
        self.assertEqual(r["stability"], "FAIL")

    def test_hidden_flight_not_graded(self):
        r = evaluate(dict(completed=True, flights=[dict(frames=frames_summary([5] * 100))]), [], 0, None, False, 4.5)
        self.assertEqual(r["flight_60fps"], "NOT_TESTED")

    def test_background_flight_cannot_pass_gpu_performance(self):
        samples = [dict(monotonic=x, foreground=False, gpu=dict(dedicated_bytes=None), peak_job_commit_bytes=0) for x in (1, 2)]
        r = evaluate(dict(completed=True, flights=[dict(start=0, end=3, frames=frames_summary([5] * 100))]), samples, 0, None, True, 4.5)
        self.assertEqual(r["flight_60fps"], "NOT_TESTED")

    def test_good_average_cannot_hide_stalls(self):
        r = evaluate(dict(completed=True, flights=[dict(frames=frames_summary([5] * 999 + [500]))]), [], 0, None, False, 4.5)
        self.assertEqual(r["hitches_100ms"], "FAIL")

    def test_crash_checkpoint_keeps_measured_load_time(self):
        r = evaluate(dict(completed=False, finished=False, opens=[dict(cycle=1, seconds=25)]), [], 3, None, False, 4.5)
        self.assertEqual(r["stability"], "FAIL")
        self.assertEqual(r["open_10s"], "FAIL")
        self.assertEqual(r["workflow"]["opens"][0]["seconds"], 25)


class WindowsLimits(unittest.TestCase):
    def wait_file(self, path):
        deadline = time.perf_counter() + 15
        while not path.exists() and time.perf_counter() < deadline:
            time.sleep(.05)
        self.assertTrue(path.exists(), "Child failed to create " + str(path))

    def test_descendant_membership_affinity_and_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            child = d / "child.py"
            child.write_text("import os,time\nfrom pathlib import Path\nPath(os.environ['TEST_OUTPUT']).write_text(str(os.getpid()))\ntime.sleep(120)\n")
            parent = d / "parent.py"
            parent.write_text("import subprocess,sys,time\nsubprocess.Popen([sys.executable,sys.argv[1]])\ntime.sleep(120)\n")
            job = Job(1, 0, 0)
            descendant_handle = None
            try:
                job.launch([sys.executable, parent, child], d, {**os.environ, "TEST_OUTPUT": str(d / "pid")})
                self.wait_file(d / "pid")
                pid = int((d / "pid").read_text())
                state = job.sample()
                self.assertIn(pid, state["pids"])
                self.assertIn(job.pid, state["pids"])
                self.assertGreater(state["peak_job_commit_bytes"], 0)
                descendant_handle = api("OpenProcess", W.HANDLE, W.DWORD, W.BOOL, W.DWORD)(0x100000 | 0x400, False, pid)
                self.assertTrue(descendant_handle)
                process_mask, system_mask = C.c_size_t(), C.c_size_t()
                self.assertTrue(api("GetProcessAffinityMask", W.BOOL, W.HANDLE, C.POINTER(C.c_size_t), C.POINTER(C.c_size_t))(
                    descendant_handle, C.byref(process_mask), C.byref(system_mask)))
                self.assertEqual(process_mask.value, job.mask)
                job.close()
                self.assertEqual(api("WaitForSingleObject", W.DWORD, W.HANDLE, W.DWORD)(descendant_handle, 5000), 0)
            finally:
                job.close()
                if descendant_handle:
                    close(descendant_handle)

    def test_commit_ceiling_rejects_allocation(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            child = d / "allocate.py"
            child.write_text("from pathlib import Path\nimport os\ntry:\n x=bytearray(256*1024*1024)\n result='unexpected allocation'\nexcept MemoryError:\n result='allocation refused'\nPath(os.environ['TEST_OUTPUT']).write_text(result)\n")
            job = Job(1, 0, .125)
            try:
                job.launch([sys.executable, child], d, {**os.environ, "TEST_OUTPUT": str(d / "result")})
                self.wait_file(d / "result")
                self.assertEqual((d / "result").read_text(), "allocation refused")
            finally:
                job.close()

    def test_cpu_quota_is_installed(self):
        job = Job(1, 1.25, 0)
        try:
            value = (W.DWORD * 2)()
            self.assertTrue(api("QueryInformationJobObject", W.BOOL, W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.c_void_p)(
                job.handle, 15, C.byref(value), C.sizeof(value), None))
            self.assertEqual(list(value), [5, 125])
        finally:
            job.close()

    def test_gpu_missing_pid_not_zero(self):
        gpu = GPUCounters()
        try:
            self.assertIsNone(gpu.sample([0xffffffff])["dedicated_bytes"])
        finally:
            gpu.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
