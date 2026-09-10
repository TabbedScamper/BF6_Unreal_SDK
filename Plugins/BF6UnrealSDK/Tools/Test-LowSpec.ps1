<# Local, disposable-project stability test. See docs/LOW-SPEC-TESTING.md. #>
param(
    [string]$Project,
    [string]$Editor,
    [ValidateSet('baseline','six-core','pressure12','pressure10','pressure8')][string]$Profile = 'six-core',
    [ValidateSet('smoke','low','high')][string]$Mode = 'low',
    [string]$Level = 'MP_Dumbo',
    [string]$Save = '',
    [int]$MinPlaced = 0,
    [int]$Cycles = 2,
    [double]$FlightSeconds = 30,
    [double]$Timeout = 1200,
    [double]$CpuPercent = 0,
    [double[]]$CameraStart,
    [ValidateSet('fresh-highpoly','copy-existing')][string]$Cache = 'fresh-highpoly',
    [string]$OutputRoot,
    [switch]$Visible,
    [ValidateSet('current','performance','balanced')][string]$RenderProfile = 'current',
    [ValidateSet(0,1)][int]$TextureStreaming = 1,
    [ValidateSet('default','on','off')][string]$Nanite = 'default',
    [ValidateSet(0,32,64,128,256)][int]$BuildBatch = 0,
    [ValidateSet(4,8,16,32,64,128,256)][int]$TerrainBatch = 16,
    [ValidateSet(0,1)][int]$GameLODs = 0,
    [ValidateSet(0,1)][int]$WaterAsync = 1,
    [ValidateSet(0,1)][int]$CompactVertices = 1,
    [ValidateSet('profile','host')][string]$EngineWorkers = 'profile',
    [switch]$MemoryReport,
    [switch]$Trace,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
if (-not $Project) {
    $taskRoot = Split-Path (Split-Path (Split-Path $PSScriptRoot))
    $taskProjects = @(Get-ChildItem -LiteralPath $taskRoot -Filter '*.uproject' -File)
    if ($taskProjects.Count -ne 1) { throw 'Pass -Project with the .uproject path.' }
    $Project = $taskProjects[0].FullName
}
if (-not $Editor) {
    $taskAssociation = (Get-Content -LiteralPath $Project -Raw | ConvertFrom-Json).EngineAssociation
    $taskEngine = Get-ItemPropertyValue -Path "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$taskAssociation" -Name InstalledDirectory -ErrorAction SilentlyContinue
    if (-not $taskEngine) { $taskEngine = "C:\Program Files\Epic Games\UE_$taskAssociation" }
    $Editor = Join-Path $taskEngine 'Engine\Binaries\Win64\UnrealEditor.exe'
}
if (Get-Process -Name UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue) {
    throw 'Close other Unreal editors first. Concurrent runs distort measurements and share plugin binaries.'
}
$taskEngineDir = Split-Path (Split-Path (Split-Path $Editor))
$taskPython = Join-Path $taskEngineDir 'Binaries\ThirdParty\Python3\Win64\python.exe'
if (-not (Test-Path -LiteralPath $taskPython)) { throw "Engine Python missing: $taskPython" }
$taskArgs = @((Join-Path $PSScriptRoot 'stability\run.py'), '--project', $Project, '--editor', $Editor,
    '--profile', $Profile, '--mode', $Mode, '--level', $Level, '--cycles', $Cycles,
    '--flight-seconds', $FlightSeconds, '--timeout', $Timeout, '--cpu-percent', $CpuPercent,
    '--cache', $Cache, '--min-placed', $MinPlaced, '--render-profile', $RenderProfile,
    '--texture-streaming', $TextureStreaming, '--nanite', $Nanite, '--build-batch', $BuildBatch, '--terrain-batch', $TerrainBatch, '--game-lods', $GameLODs,
    '--water-async', $WaterAsync, '--compact-vertices', $CompactVertices, '--engine-workers', $EngineWorkers)
if ($Save) { $taskArgs += @('--save', $Save) }
if ($OutputRoot) { $taskArgs += @('--output-root', $OutputRoot) }
if ($Visible) { $taskArgs += '--visible' }
if ($MemoryReport) { $taskArgs += '--memory-report' }
if ($Trace) { $taskArgs += '--trace' }
if ($CameraStart) {
    if ($CameraStart.Count -ne 6) { throw '-CameraStart needs X,Y,Z,Pitch,Yaw,Roll in centimetres/degrees.' }
    $taskArgs += '--camera-start'
    $taskArgs += $CameraStart
}
if ($PrepareOnly) { $taskArgs += '--prepare-only' }
& $taskPython @taskArgs
exit $LASTEXITCODE
