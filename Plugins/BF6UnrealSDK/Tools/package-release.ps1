<#
    Assemble the 0.8.1 release pair.

    WHY THIS EXISTS RATHER THAN RunUAT BuildPlugin
    ----------------------------------------------
    BuildPlugin builds each plugin in an isolated host project. That works for
    the SDK and cannot work for the add-on: BF6HighPoly declares a dependency on
    BF6UnrealSDK, the isolated host does not have it, and the build stops with
    "Unable to find plugin 'BF6UnrealSDK'". The pair is only buildable together,
    which is exactly the relationship the paired release is about.

    So the editor target is built normally, in the real project where both
    plugins live, and this assembles the packages from what that produced. It is
    deliberately explicit about what goes in, because the alternative - copy the
    folder and see what happens - is what put 4.6 GB of extracted game meshes and
    two abandoned experimental DLLs into a release candidate.

    WHAT IS DELIBERATELY LEFT OUT
    -----------------------------
    - .pdb files. Three of them came to 240 MB, and a debug symbol database is
      not something a user of a precompiled plugin can act on.
    - Intermediate/, Binaries/*/UnrealGame, and anything not named below.
    - Anything derived from the game install. Website data may ship; game assets
      may not.

    Usage:  ./package-release.ps1 [-OutDir <path>] [-Version 0.8.1]
#>
param(
    [string] $OutDir  = "$env:USERPROFILE\Documents\BF6-Release-0.8.1",
    [string] $Version = "0.8.1"
)

$ErrorActionPreference = 'Stop'
$plugins = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) ''
$sdkSrc  = Join-Path $plugins 'BF6UnrealSDK'
$hpSrc   = Join-Path $plugins 'Add-Ons\BF6HighPoly'
$projectSrc = Split-Path $plugins -Parent

foreach ($descriptor in @((Join-Path $sdkSrc 'BF6UnrealSDK.uplugin'), (Join-Path $hpSrc 'BF6HighPoly.uplugin'))) {
    $declared = (Get-Content -LiteralPath $descriptor -Raw | ConvertFrom-Json).VersionName
    if ($declared -ne $Version) { throw "descriptor version $declared does not match release $Version`: $descriptor" }
}

function Copy-Tree($from, $to, $what) {
    if (-not (Test-Path $from)) { Write-Host "  (no $what)"; return }
    robocopy $from $to /E /R:2 /W:1 /NFL /NDL /NJH /NJS /XD Intermediate __pycache__ /XF *.pyc *.bak *.bak_* | Out-Null
    # Robocopy signals work done, not failure: 0-7 are success codes (1 = files
    # copied). Only 8 and above are errors. Clear it either way, or the last
    # copy's "1" becomes the script's exit code and every run looks failed.
    $code = $LASTEXITCODE
    $global:LASTEXITCODE = 0
    if ($code -ge 8) { throw "copying $what failed (robocopy $code)" }
}

function Copy-Binaries($from, $to) {
    # UnrealEditor.modules is the manifest the editor itself reads to decide
    # which DLL backs each module, so it is the only correct answer to "what
    # belongs in a package". Copying exactly what it names is why this is
    # manifest-driven rather than a wildcard with exclusions: the add-on's
    # Binaries folder holds two abandoned experimental builds, three live-coding
    # .loaded-vN DLLs, and a pile of .patch_N artefacts, and an exclusion list
    # only ever knows about the junk that existed when it was written.
    if (-not (Test-Path $from)) { return }
    $manifest = Join-Path $from 'UnrealEditor.modules'
    if (-not (Test-Path $manifest)) { throw "no UnrealEditor.modules in $from - build the editor target first" }

    New-Item -ItemType Directory -Force -Path $to | Out-Null
    Copy-Item $manifest (Join-Path $to 'UnrealEditor.modules') -Force

    $mods = (Get-Content $manifest -Raw | ConvertFrom-Json).Modules
    foreach ($name in $mods.PSObject.Properties.Name) {
        $dll = $mods.$name
        $src = Join-Path $from $dll
        if (-not (Test-Path $src)) { throw "$manifest names $dll for module $name, but it is not there" }
        Copy-Item $src (Join-Path $to $dll) -Force
        Write-Host ("  module {0} -> {1}" -f $name, $dll)
    }
}

function Copy-Source($from, $to) {
    # Source ships even though the binaries are prebuilt. A Blueprint-only host
    # project would load the DLL happily without it, but a C++ host project makes
    # UBT enumerate every enabled plugin and look for its .Build.cs, and a plugin
    # without one fails the host's build rather than merely failing to load. It
    # is a few megabytes to not break half the projects this drops into.
    #
    # Excluded:
    #   *.bak_*            - three superseded bf6_core builds, 4.7 MB
    #   *_test.exe         - a standalone dispatch harness, not part of the tool
    #   data\maps          - byte-identical to Resources\mapthumbs
    #   data\gameplay      - byte-identical to Resources\gameplaymeshes
    if (-not (Test-Path $from)) { return }
    robocopy $from $to /E /R:2 /W:1 /NFL /NDL /NJH /NJS `
        /XD Intermediate (Join-Path $from 'ThirdParty\libbf6\data\maps') (Join-Path $from 'ThirdParty\libbf6\data\gameplay') `
        /XF *.bak *.bak_* *_test.exe *.pdb *.obj *.pyc | Out-Null
    $code = $LASTEXITCODE
    $global:LASTEXITCODE = 0
    if ($code -ge 8) { throw "copying Source failed (robocopy $code)" }
}

function Size-Of($path) {
    if (-not (Test-Path $path)) { return 0 }
    [math]::Round((Get-ChildItem $path -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)
}

# ---- where the packages go, and what may be deleted to make room -----------
#
# This used to be one line: if the output directory exists, delete it, recursively
# and forcibly. That is a loaded gun pointed at whatever the caller typed. A
# slip in -OutDir - a project folder, a source tree, a home directory - would be
# erased before packaging started, and a package that then failed would have
# destroyed the previous good release for nothing.
#
# So: build in a fresh sibling staging directory, refuse to delete anything this
# script did not create, and only swap the new release into place once it is
# actually built.
$MarkerName = '.bf6-release-staging'

function Assert-SafeOutDir($path) {
    # The raw string is checked BEFORE canonicalizing it. GetFullPath throws on
    # a wildcard, so canonicalizing first replaced this clear refusal with a
    # framework stack trace: the same safe outcome, told badly.
    if ($path -match '[*?]') { throw "output path contains a wildcard, which is never a real folder: $path" }
    $full = [System.IO.Path]::GetFullPath($path).TrimEnd('\')
    # Never a filesystem root, a drive, or the user's profile.
    if ($full.Length -le 3) { throw "refusing to use a drive root as the output: $full" }
    $forbidden = @(
        [Environment]::GetFolderPath('UserProfile'),
        [Environment]::GetFolderPath('MyDocuments'),
        [Environment]::GetFolderPath('Desktop'),
        (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent),   # the Plugins folder
        (Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent)  # the project root
    ) | Where-Object { $_ } | ForEach-Object { [System.IO.Path]::GetFullPath($_).TrimEnd('\') }
    foreach ($bad in $forbidden) {
        if ($full -ieq $bad) { throw "refusing to use $full as the output directory" }
        # Also refuse to write a release INTO the project or plugin tree.
        if ($bad -like '*Plugins' -and $full.StartsWith($bad, 'OrdinalIgnoreCase')) {
            throw "refusing to build a release inside the plugin tree: $full"
        }
    }
    # An existing directory is only reusable if this script made it. Anything
    # else is somebody's data and is never deleted.
    if (Test-Path -LiteralPath $full) {
        if (-not (Test-Path -LiteralPath (Join-Path $full $MarkerName))) {
            throw ("$full already exists and was not created by this script. " +
                   "Delete it yourself if you meant to replace it, or pass a different -OutDir.")
        }
    }
    return $full
}

$OutDir = Assert-SafeOutDir $OutDir
# Build somewhere new, so a failure cannot damage the last good release.
$Staging = "$OutDir.new-$([Guid]::NewGuid().ToString('N').Substring(0,8))"
New-Item -ItemType Directory -Force -Path $Staging | Out-Null
Set-Content -LiteralPath (Join-Path $Staging $MarkerName) -Encoding utf8 `
    -Value "Created by package-release.ps1. Safe to delete."
$Final = $OutDir
$OutDir = $Staging
$OutputParent = [IO.Path]::GetFullPath((Split-Path $Final -Parent)).TrimEnd('\')
function Assert-ReleaseSibling($path) {
    $full = [IO.Path]::GetFullPath($path).TrimEnd('\')
    if ((Split-Path $full -Parent) -ine $OutputParent) { throw "release path escaped its output parent: $full" }
    if (Test-Path -LiteralPath $full) {
        if ((Get-Item -LiteralPath $full).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "release path is a reparse point: $full" }
        if (-not (Test-Path -LiteralPath (Join-Path $full $MarkerName))) { throw "unmarked release path: $full" }
    }
}

# ---- the SDK -------------------------------------------------------------
Write-Host "BF6UnrealSDK $Version"
$sdkOut = Join-Path $OutDir 'BF6UnrealSDK'
New-Item -ItemType Directory -Force -Path $sdkOut | Out-Null
Copy-Item (Join-Path $sdkSrc 'BF6UnrealSDK.uplugin') $sdkOut -Force
foreach ($d in 'Resources', 'Content', 'Config') { Copy-Tree (Join-Path $sdkSrc $d) (Join-Path $sdkOut $d) $d }
Copy-Tree (Join-Path $sdkSrc 'Tools\context') (Join-Path $sdkOut 'Tools\context') 'editor context tools'
Copy-Tree (Join-Path $sdkSrc 'Tools\stability') (Join-Path $sdkOut 'Tools\stability') 'local stability tests'
Copy-Item -LiteralPath (Join-Path $sdkSrc 'Tools\Test-LowSpec.ps1') -Destination (Join-Path $sdkOut 'Tools')
New-Item -ItemType Directory -Force -Path (Join-Path $sdkOut 'docs') | Out-Null
foreach ($doc in 'LOADOUTS.md','PROJECT-CONTRACT.md','PORTAL-EXPORT.md','LOW-SPEC-TESTING.md','ATTACHING-AN-AI.md') {
    Copy-Item -LiteralPath (Join-Path $sdkSrc "docs\$doc") -Destination (Join-Path $sdkOut 'docs')
}
Copy-Binaries (Join-Path $sdkSrc 'Binaries\Win64') (Join-Path $sdkOut 'Binaries\Win64')
# The native core is a runtime dependency, not a build artefact, and the staged
# resolver looks for it here first.
$core = Join-Path $sdkSrc 'Source\ThirdParty\libbf6\bin\Win64\bf6_core.dll'
if (Test-Path $core) { Copy-Item $core (Join-Path $sdkOut 'Binaries\Win64\bf6_core.dll') -Force }
Copy-Source (Join-Path $sdkSrc 'Source') (Join-Path $sdkOut 'Source')
Write-Host ("  {0} MB" -f (Size-Of $sdkOut))

# ---- the add-on ----------------------------------------------------------
Write-Host "BF6HighPoly $Version"
$hpOut = Join-Path $OutDir 'BF6HighPoly'
New-Item -ItemType Directory -Force -Path $hpOut | Out-Null
Copy-Item (Join-Path $hpSrc 'BF6HighPoly.uplugin') $hpOut -Force
Copy-Item (Join-Path $hpSrc 'README.md') $hpOut -Force
New-Item -ItemType Directory -Force -Path (Join-Path $hpOut 'docs') | Out-Null
Copy-Item -LiteralPath (Join-Path $hpSrc 'docs\PERFORMANCE.md') -Destination (Join-Path $hpOut 'docs')
foreach ($d in 'Resources', 'Shaders', 'Config') { Copy-Tree (Join-Path $hpSrc $d) (Join-Path $hpOut $d) $d }
Copy-Binaries (Join-Path $hpSrc 'Binaries\Win64') (Join-Path $hpOut 'Binaries\Win64')
foreach ($module in 'BF6HighPoly','BF6HighPolyShaders') {
    Copy-Source (Join-Path $hpSrc "Source\$module") (Join-Path $hpOut "Source\$module")
}
Write-Host ("  {0} MB" -f (Size-Of $hpOut))

# A complete starter project for creators who do not have an Unreal project.
# The add-on remains optional and is installed through the map selector.
$projectOut = Join-Path $OutDir 'BF6_Unreal_SDK'
New-Item -ItemType Directory -Force -Path $projectOut | Out-Null
Copy-Item -LiteralPath (Join-Path $projectSrc 'BF6_Unreal_SDK.uproject') -Destination $projectOut
foreach ($d in 'Config','Content','Source','Branding') { Copy-Tree (Join-Path $projectSrc $d) (Join-Path $projectOut $d) $d }
Copy-Binaries (Join-Path $projectSrc 'Binaries\Win64') (Join-Path $projectOut 'Binaries\Win64')
$editorReceipts = @(Get-ChildItem -LiteralPath (Join-Path $projectSrc 'Binaries\Win64') -Filter '*Editor.target' -File)
if ($editorReceipts.Count -ne 1) { throw 'expected one built editor target receipt for the starter project' }
Copy-Item -LiteralPath $editorReceipts[0].FullName -Destination (Join-Path $projectOut 'Binaries\Win64')
Copy-Tree $sdkOut (Join-Path $projectOut 'Plugins\BF6UnrealSDK') 'SDK plugin'
foreach ($doc in 'README.md','LICENSE') {
    if (Test-Path -LiteralPath (Join-Path $projectSrc $doc)) { Copy-Item -LiteralPath (Join-Path $projectSrc $doc) -Destination $projectOut }
}

# ---- the manifest --------------------------------------------------------
# Identity and integrity for both halves in one file, so an installer can tell
# whether a pair belongs together before it applies anything.
function Hash-Zip($path) { (Get-FileHash $path -Algorithm SHA256).Hash.ToLower() }

$sdkZip = Join-Path $OutDir "BF6UnrealSDK_Plugin_v$Version.zip"
$hpZip  = Join-Path $OutDir "BF6HighPoly_Plugin_v$Version.zip"
$projectZip = Join-Path $OutDir "BF6UnrealSDK_Project_v$Version.zip"
Compress-Archive -Path $sdkOut -DestinationPath $sdkZip -Force
Compress-Archive -Path $hpOut  -DestinationPath $hpZip  -Force
Compress-Archive -Path $projectOut -DestinationPath $projectZip -Force

$engine = 'UE_5.8'
$manifest = [ordered]@{
    version   = $Version
    engine    = $engine
    builtUtc  = (Get-Date).ToUniversalTime().ToString('s') + 'Z'
    pair      = 'BF6UnrealSDK and BF6HighPoly are released together and are expected to match on version.'
    components = @(
        [ordered]@{ name='BF6UnrealSDK'; asset=(Split-Path $sdkZip -Leaf); bytes=(Get-Item $sdkZip).Length; sha256=(Hash-Zip $sdkZip); requires=@() },
        [ordered]@{ name='BF6HighPoly';  asset=(Split-Path $hpZip  -Leaf); bytes=(Get-Item $hpZip ).Length; sha256=(Hash-Zip $hpZip ); requires=@('BF6UnrealSDK') },
        [ordered]@{ name='BF6UnrealSDK_Project'; asset=(Split-Path $projectZip -Leaf); bytes=(Get-Item $projectZip).Length; sha256=(Hash-Zip $projectZip); requires=@() }
    )
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutDir "release-manifest-v$Version.json") -Encoding utf8

# ---- swap the finished build into place ------------------------------------
# Only now, with both zips and the manifest written, does the previous release
# get replaced. Until this point a failure has cost nothing.
if (-not (Test-Path -LiteralPath $sdkZip) -or -not (Test-Path -LiteralPath $hpZip) -or -not (Test-Path -LiteralPath $projectZip)) {
    throw "packaging did not produce all three archives; the previous release in $Final is untouched"
}
# THE RETIRED COPY IS A SECOND DESTRUCTIVE TARGET, AND IT WAS NOT VALIDATED.
#
# The guard at the top checks the directory the caller named. This block then
# computed a SIBLING of it and deleted that recursively without checking
# anything: whose it was, whether this script made it, or whether it was a
# junction pointing somewhere else entirely. Somebody with their own folder
# called "<release>.previous" would have lost it to a packaging run that never
# mentioned the name.
#
# So nothing pre-existing is ever deleted. The outgoing release moves to a name
# that cannot already be in use, and only copies this script demonstrably made,
# identified by the marker it writes, are ever cleaned up.
if (Test-Path -LiteralPath $Final) {
    $Retired = "$Final.previous-$([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))"
    $n = 0
    while (Test-Path -LiteralPath $Retired) { $n++; $Retired = "$Final.previous-$([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))-$n" }
    Assert-ReleaseSibling $Final
    Assert-ReleaseSibling $Retired
    Assert-ReleaseSibling $Staging
    Move-Item -LiteralPath $Final -Destination $Retired

    # If the new release cannot take its place, put the old one back. Leaving
    # the caller with neither would be the worst outcome available here.
    try {
        Move-Item -LiteralPath $Staging -Destination $Final -ErrorAction Stop
    } catch {
        Move-Item -LiteralPath $Retired -Destination $Final
        throw "could not put the new release in place, so the previous one was restored: $($_.Exception.Message)"
    }

    # Retain earlier packages for rollback; packaging never prunes directories.
} else {
    Assert-ReleaseSibling $Staging
    Assert-ReleaseSibling $Final
    Move-Item -LiteralPath $Staging -Destination $Final
}
$OutDir = $Final

Write-Host ""
Write-Host "packages:"
Get-ChildItem -LiteralPath $OutDir -Filter *.zip | ForEach-Object { Write-Host ("  {0}  {1:N1} MB" -f $_.Name, ($_.Length/1MB)) }
Write-Host ("  release-manifest-v{0}.json" -f $Version)
Write-Host ("  in {0}" -f $OutDir)
