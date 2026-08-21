@echo off
REM ---------------------------------------------------------------------------
REM  BF6 Unreal SDK - Fix Auto Update
REM
REM  Versions 0.5.2 and earlier could not start their own updater, so pressing
REM  Yes closed the editor without changing anything. This installs the newest
REM  plugin by hand, once. After that the in-editor updater works normally.
REM
REM  Just double-click it. Close the Unreal editor first.
REM ---------------------------------------------------------------------------
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command "& {

$ErrorActionPreference = 'Stop'
Write-Host ''
Write-Host '  BF6 Unreal SDK - Fix Auto Update' -ForegroundColor Cyan
Write-Host '  --------------------------------'
Write-Host ''

# The editor holds the plugin DLL open, so it has to be closed first.
if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) {
    Write-Host '  The Unreal editor is still running.' -ForegroundColor Yellow
    Write-Host '  Close it completely, then run this again.'
    Write-Host ''
    pause
    exit 1
}

# Find the plugin: next to this file first, then the usual project locations.
$here = Split-Path -Parent $MyInvocation.MyCommand.Definition
$candidates = @()
foreach ($base in @($here, (Split-Path -Parent $here))) {
    if ($base) {
        $candidates += (Join-Path $base 'Plugins\BF6UnrealSDK')
        $candidates += (Join-Path $base 'BF6_Unreal_SDK\Plugins\BF6UnrealSDK')
    }
}
$roots = @(\"$env:USERPROFILE\Documents\Unreal Projects\", \"$env:USERPROFILE\Documents\\\", 'C:\', 'D:\')
foreach ($r in $roots) {
    if (Test-Path $r) {
        try {
            Get-ChildItem -Path $r -Filter 'BF6UnrealSDK.uplugin' -Recurse -Depth 5 -ErrorAction SilentlyContinue |
                ForEach-Object { $candidates += $_.DirectoryName }
        } catch { }
    }
}

$plugin = $null
foreach ($c in $candidates) {
    if ($c -and (Test-Path (Join-Path $c 'BF6UnrealSDK.uplugin'))) { $plugin = $c; break }
}

if (-not $plugin) {
    Write-Host '  Could not find your BF6UnrealSDK plugin folder automatically.'
    Write-Host '  Paste the full path to it below, for example:'
    Write-Host '    C:\Users\you\Documents\Unreal Projects\BF6_Unreal_SDK\Plugins\BF6UnrealSDK'
    Write-Host ''
    $plugin = (Read-Host '  Plugin folder').Trim('\"')
    if (-not (Test-Path (Join-Path $plugin 'BF6UnrealSDK.uplugin'))) {
        Write-Host '  That folder has no BF6UnrealSDK.uplugin in it. Nothing was changed.' -ForegroundColor Red
        pause
        exit 1
    }
}

$before = 'unknown'
if ((Get-Content (Join-Path $plugin 'BF6UnrealSDK.uplugin') -Raw) -match '\"VersionName\"\s*:\s*\"([^\"]+)\"') { $before = $Matches[1] }
Write-Host \"  Found: $plugin\"
Write-Host \"  Installed version: $before\"
Write-Host ''

# Ask GitHub for the newest release and its plugin package.
Write-Host '  Checking for the newest release...'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/TabbedScamper/BF6_Unreal_SDK/releases/latest' -Headers @{ 'User-Agent' = 'BF6UnrealSDK-Fixer' }
$asset = $rel.assets | Where-Object { $_.name -like '*Plugin*.zip' } | Select-Object -First 1
if (-not $asset) {
    Write-Host '  That release has no plugin package attached. Nothing was changed.' -ForegroundColor Red
    pause
    exit 1
}
Write-Host \"  Newest release: $($rel.tag_name)\"

$tmp = Join-Path $env:TEMP ('bf6sdk_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null
$zip = Join-Path $tmp 'plugin.zip'
Write-Host '  Downloading...'
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -Headers @{ 'User-Agent' = 'BF6UnrealSDK-Fixer' }

Write-Host '  Unpacking...'
$stage = Join-Path $tmp 'stage'
Expand-Archive -Path $zip -DestinationPath $stage -Force
$src = Join-Path $stage 'BF6UnrealSDK'
if (-not (Test-Path $src)) { $src = $stage }

Write-Host '  Installing...'
robocopy $src $plugin /E /R:5 /W:2 /NFL /NDL /NJH /NJS | Out-Null
$code = $LASTEXITCODE
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

$after = 'unknown'
if ((Get-Content (Join-Path $plugin 'BF6UnrealSDK.uplugin') -Raw) -match '\"VersionName\"\s*:\s*\"([^\"]+)\"') { $after = $Matches[1] }

Write-Host ''
if ($code -ge 8) {
    Write-Host \"  The copy reported a problem (robocopy code $code) and the version is still $after.\" -ForegroundColor Red
    Write-Host '  Make sure the editor is closed and try once more.'
} elseif ($after -eq $before) {
    Write-Host \"  Nothing changed - you were already on $after.\" -ForegroundColor Yellow
} else {
    Write-Host \"  Done. Updated from $before to $after.\" -ForegroundColor Green
    Write-Host '  Start the editor as usual. Updates will now install themselves.'
}
Write-Host ''
pause
}"
endlocal
