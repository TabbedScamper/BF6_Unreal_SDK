@echo off
setlocal
set "PS1=%TEMP%\bf6_fix_autoupdate.ps1"
more +8 "%~f0" > "%PS1%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
del "%PS1%" >nul 2>&1
exit /b
REM ---- everything below this line is the PowerShell script ----
# BF6 Unreal SDK - Fix Auto Update
#
# Versions 0.5.2 and earlier could not start their own updater, so pressing Yes
# closed the editor without changing anything. This installs the newest plugin
# by hand, once. After that the in-editor updater works normally.

$ErrorActionPreference = 'Stop'
Write-Host ''
Write-Host '  BF6 Unreal SDK - Fix Auto Update' -ForegroundColor Cyan
Write-Host '  --------------------------------'
Write-Host ''

try {
    # The editor holds the plugin DLL open, so it has to be closed first.
    if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) {
        Write-Host '  The Unreal editor is still running.' -ForegroundColor Yellow
        Write-Host '  Close it completely, then run this again.'
        Write-Host ''
        Read-Host '  Press Enter to close'
        exit 1
    }

    # Find the plugin: beside this file first, then the usual project locations.
    $here = Split-Path -Parent $PSCommandPath
    $candidates = New-Object System.Collections.ArrayList
    foreach ($base in @($here, (Split-Path -Parent $here), (Get-Location).Path)) {
        if ($base) {
            [void]$candidates.Add((Join-Path $base 'Plugins\BF6UnrealSDK'))
            [void]$candidates.Add((Join-Path $base 'BF6_Unreal_SDK\Plugins\BF6UnrealSDK'))
            [void]$candidates.Add($base)
        }
    }
    $plugin = $null
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'BF6UnrealSDK.uplugin'))) { $plugin = $c; break }
    }

    if (-not $plugin) {
        Write-Host '  Looking for your project...'
        $roots = @((Join-Path $env:USERPROFILE 'Documents'), 'C:\', 'D:\')
        foreach ($r in $roots) {
            if ($plugin) { break }
            if (-not (Test-Path $r)) { continue }
            $hit = Get-ChildItem -Path $r -Filter 'BF6UnrealSDK.uplugin' -Recurse -Depth 6 -Force -ErrorAction SilentlyContinue |
                   Select-Object -First 1
            if ($hit) { $plugin = $hit.DirectoryName }
        }
    }

    if (-not $plugin) {
        Write-Host '  Could not find your BF6UnrealSDK plugin folder automatically.'
        Write-Host '  Paste the full path to it below, for example:'
        Write-Host '    C:\Users\you\Documents\Unreal Projects\BF6_Unreal_SDK\Plugins\BF6UnrealSDK'
        Write-Host ''
        $plugin = (Read-Host '  Plugin folder').Trim('"').Trim()
        if (-not (Test-Path (Join-Path $plugin 'BF6UnrealSDK.uplugin'))) {
            Write-Host '  That folder has no BF6UnrealSDK.uplugin in it. Nothing was changed.' -ForegroundColor Red
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
