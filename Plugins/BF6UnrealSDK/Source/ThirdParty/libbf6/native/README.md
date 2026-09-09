# Unreal reader source, ABI 5

This is the source snapshot for the reader installed with the September 9 loadout preview work. It combines the UI reconstruction reader with the Unreal reader's existing extensions. Build the DLL and use its matching `include/bf6_core.h`; replacing only one can corrupt native structures.

Preserved Unreal changes include UI sound and placed sound exports, game-mode capture-pair deduplication (`bf6_gm_stats::dropped_twin`), and the material fix that excludes packed scalar parameter `0x686A1072` from base-color tint. All 178 exports of the prior Unreal DLL remain; the combined DLL exports 206 symbols. The ABI is 5 because the UI reader's ABI 4 and Unreal's older game-mode structure were incompatible.

Build from a Visual Studio developer shell with the C++ toolchain and CMake:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --target bf6_core
```

The output is `build/Release/bf6_core.dll`. With the Unreal editor closed, install it in `../bin/Win64/` and the SDK plugin's `Binaries/Win64/`, and copy `include/bf6_core.h` to `../include/`. Rebuild both Unreal plugins against that header. The DLL reads the user's installed game; this source snapshot contains no game assets.

`pose_probe` is a diagnostic target for comparing animation hand targets, actual wrists and weapon transforms. It is not required by the plugin.
