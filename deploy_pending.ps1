# Swap in the freshly-built libbf6 DLL and rebuild the Unreal plugin.
# Run this ONLY with the Unreal editor CLOSED (it holds bf6_core.dll open).
#
#   powershell -File deploy_pending.ps1
#
$ErrorActionPreference = "Stop"
$proj   = "C:\Users\mwalt\Documents\Unreal Projects\BF6_High_Poly\BF6_High_Poly.uproject"
$tp     = "C:\Users\mwalt\Documents\Unreal Projects\BF6_High_Poly\Plugins\BF6HighPoly\Source\ThirdParty\libbf6\bin\Win64"
$bin    = "C:\Users\mwalt\Documents\Unreal Projects\BF6_High_Poly\Plugins\BF6HighPoly\Binaries\Win64"
$ue     = "C:\Program Files\Epic Games\UE_5.8"

$editor = Get-Process -Name 'UnrealEditor','BF6_High_Poly' -ErrorAction SilentlyContinue
if ($editor) { Write-Host "Editor is running - close it first." -ForegroundColor Red; exit 1 }

# 1. swap in the pending DLL (both locations the plugin can load from)
if (Test-Path "$tp\bf6_core.dll.pending") {
    Copy-Item "$tp\bf6_core.dll.pending" "$tp\bf6_core.dll" -Force
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    Copy-Item "$tp\bf6_core.dll.pending" "$bin\bf6_core.dll" -Force
    Remove-Item "$tp\bf6_core.dll.pending" -Force
    Write-Host "DLL swapped in (184832 bytes expected)." -ForegroundColor Green
}

# 2. rebuild the editor module (picks up the new BF6HighPoly.cpp + Build.cs)
$ubt = Join-Path $ue "Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe"
& $ubt BF6_High_PolyEditor Win64 Development "-Project=$proj" -WaitMutex
Write-Host "`nDone. Reopen the editor and use Window > Tools > BF6 Objects." -ForegroundColor Green
