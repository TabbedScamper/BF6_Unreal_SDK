@echo off
setlocal
set "PS1=%TEMP%\bf6_fix_autoupdate.ps1"
more +9 "%~f0" > "%PS1%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" "%~dp0."
del "%PS1%" >nul 2>&1
exit /b
REM ---------------------------------------------------------------------
REM  Everything below this line is the PowerShell script that gets written
param([string]$BatDir = "")

# BF6 Unreal SDK - Fix Auto Update
#
# Put this file in your project folder - the one holding the .uproject - and
# double-click it with the editor closed.
#
# Versions 0.5.2 and earlier could not start their own updater, so pressing Yes
# closed the editor without changing anything. This installs the newest plugin
# by hand, once. After that the in-editor updater works normally.

$ErrorActionPreference = 'Stop'
Write-Host ''
Write-Host '  BF6 Unreal SDK - Fix Auto Update' -ForegroundColor Cyan
Write-Host '  --------------------------------'
Write-Host ''

function Test-PluginDir([string]$d) {
    if ([string]::IsNullOrWhiteSpace($d)) { return $false }
    return (Test-Path (Join-Path $d 'BF6UnrealSDK.uplugin'))
}

# accepts the project folder, the plugin folder itself, or the Plugins folder
function Resolve-PluginDir([string]$d) {
    if ([string]::IsNullOrWhiteSpace($d)) { return $null }
    try { $d = (Resolve-Path -LiteralPath $d -ErrorAction Stop).Path } catch { return $null }
    foreach ($try in @(
        (Join-Path $d 'Plugins\BF6UnrealSDK'),
        $d,
        (Join-Path $d 'BF6UnrealSDK'),
        (Join-Path $d 'BF6_Unreal_SDK\Plugins\BF6UnrealSDK'))) {
        if (Test-PluginDir $try) { return (Resolve-Path -LiteralPath $try).Path }
    }
    return $null
}

try {
    if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) {
        Write-Host '  The Unreal editor is still running.' -ForegroundColor Yellow
        Write-Host '  Close it completely, then run this again.'
        Write-Host ''
        Read-Host '  Press Enter to close'
        exit 1
    }

    # where this file is sitting - that is the whole rule
    $plugin = Resolve-PluginDir $BatDir
    if (-not $plugin) { $plugin = Resolve-PluginDir (Get-Location).Path }

    if (-not $plugin) {
        Write-Host '  This file needs to be in your project folder.' -ForegroundColor Yellow
        Write-Host ''
        Write-Host '  Move it next to your .uproject file - the folder that also has'
        Write-Host '  Config, Content and Plugins in it - then double-click it again.'
        Write-Host ''
        Write-Host '  Or paste that folder path here and press Enter:'
        $answer = (Read-Host '  Project folder').Trim('"').Trim()
        $plugin = Resolve-PluginDir $answer
        if (-not $plugin) {
            Write-Host '  No Plugins\BF6UnrealSDK in there. Nothing was changed.' -ForegroundColor Red
            Read-Host '  Press Enter to close'
            exit 1
        }
    }

    $upluginPath = Join-Path $plugin 'BF6UnrealSDK.uplugin'
    $before = 'unknown'
    if ((Get-Content $upluginPath -Raw) -match '"VersionName"\s*:\s*"([^"]+)"') { $before = $Matches[1] }
    Write-Host ('  Found: ' + $plugin)
    Write-Host ('  Installed version: ' + $before)
    Write-Host ''

    Write-Host '  Checking for the newest release...'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $headers = @{ 'User-Agent' = 'BF6UnrealSDK-Fixer' }
    $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/TabbedScamper/BF6_Unreal_SDK/releases/latest' -Headers $headers
    $asset = $rel.assets | Where-Object { $_.name -like '*Plugin*.zip' } | Select-Object -First 1
    if (-not $asset) {
        Write-Host '  That release has no plugin package attached. Nothing was changed.' -ForegroundColor Red
        Read-Host '  Press Enter to close'
        exit 1
    }
    Write-Host ('  Newest release: ' + $rel.tag_name)

    $tmp = Join-Path $env:TEMP ('bf6sdk_' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $zip = Join-Path $tmp 'plugin.zip'
    Write-Host '  Downloading...'
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -Headers $headers

    Write-Host '  Unpacking...'
    $stage = Join-Path $tmp 'stage'
    Expand-Archive -Path $zip -DestinationPath $stage -Force
    $src = Join-Path $stage 'BF6UnrealSDK'
    if (-not (Test-Path $src)) { $src = $stage }

    Write-Host '  Installing...'
    $null = robocopy $src $plugin /E /R:5 /W:2 /NFL /NDL /NJH /NJS
    $code = $LASTEXITCODE
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

    $after = 'unknown'
    if ((Get-Content $upluginPath -Raw) -match '"VersionName"\s*:\s*"([^"]+)"') { $after = $Matches[1] }

    Write-Host ''
    if ($code -ge 8) {
        Write-Host ('  The copy reported a problem (robocopy code ' + $code + ') and the version is still ' + $after + '.') -ForegroundColor Red
        Write-Host '  Make sure the editor is closed and try once more.'
    } elseif ($after -eq $before) {
        Write-Host ('  Nothing changed - you were already on ' + $after + '.') -ForegroundColor Yellow
    } else {
        Write-Host ('  Done. Updated from ' + $before + ' to ' + $after + '.') -ForegroundColor Green
        Write-Host '  Start the editor as usual. Updates will now install themselves.'
    }
}
catch {
    Write-Host ''
    Write-Host ('  Something went wrong: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host '  Nothing was changed. You can install by hand instead: download the'
    Write-Host '  plugin zip from the releases page and unzip it over your'
    Write-Host '  Plugins\BF6UnrealSDK folder.'
}

Write-Host ''
Read-Host '  Press Enter to close'
