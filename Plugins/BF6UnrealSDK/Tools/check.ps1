<#
  check.ps1 - compile every plugin file on its own, then lint the conventions.

  WHY A SINGLE-FILE COMPILE.
  Live Coding means a normal build needs the editor CLOSED, which turns every
  typo into a round trip through a human. -SingleFile compiles one .cpp and does
  not link, so it runs in ~35 seconds with the editor open and in front of you.

  It is also STRICTER than the real build. A unity build concatenates .cpp files,
  so a file can lean on an include, or a type declaration, that some neighbour
  happened to pull in first. Alone, it cannot. Everything this catches is real;
  it is just not fatal YET.

  Usage:
    Tools\check.ps1            # everything: compile all + lint
    Tools\check.ps1 -Lint      # lint only, no compiler (about a second)
    Tools\check.ps1 -Files BF6BuildMode.cpp
#>
param(
  [string[]] $Files,
  [switch]   $Lint
)

$ErrorActionPreference = 'Stop'
$Plugin  = Split-Path -Parent $PSScriptRoot
$Private = Join-Path $Plugin 'Source\BF6UnrealSDK\Private'
# Add-ons reach the tool only through Public\BF6SDKExtension.h, but they break
# the same ways, so they are checked with it rather than separately.
$AddOns  = Join-Path (Split-Path -Parent $Plugin) 'Add-Ons'
$Project = 'C:\Users\mwalt\Documents\Unreal Projects\BF6_High_Poly\BF6_Unreal_SDK.uproject'
$Target  = 'BF6_High_PolyEditor'
$UBT     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe'

$Fail = 0
$Warn = 0

function Note($Kind, $File, $Line, $Msg) {
  if ($Kind -eq 'FAIL') { $script:Fail++ } else { $script:Warn++ }
  $where = $File
  if ($Line) { $where = "${File}:${Line}" }
  Write-Host ("  {0,-4} {1,-46} {2}" -f $Kind, $where, $Msg)
}

# ---------------------------------------------------------------------------
# LINT. Each rule below is here because it already cost real time once.
# ---------------------------------------------------------------------------
function Invoke-Lint {
  Write-Host "`nLINT" -ForegroundColor Cyan
  $srcs = @(Get-ChildItem $Private -Include *.cpp,*.h -Recurse)
  if (Test-Path $AddOns) {
    $srcs += @(Get-ChildItem $AddOns -Include *.cpp,*.h -Recurse |
               Where-Object { $_.FullName -notlike '*\Intermediate\*' -and $_.FullName -notlike '*\Binaries\*' })
  }

  foreach ($f in $srcs) {
    $name  = $f.Name
    $lines = Get-Content $f.FullName
    $inNamespace = $false
    $topOver     = $false

    for ($i = 0; $i -lt $lines.Count; $i++) {
      $n = $i + 1
      $L = $lines[$i]

      # Rule 1: includes live in ONE block at the top.
      # Three of these sat 4,900 lines down in BF6UnrealSDK.cpp, so every
      # GConfig call above them compiled only by unity accident.
      if ($L -match '^\s*#include') {
        if ($topOver) { Note 'FAIL' $name $n 'include below the top block - hoist it' }
      }
      elseif ($L -match '^\s*(static|void|bool|int32|float|FString|class|struct|namespace)\b' -and -not $topOver) {
        $topOver = $true
      }

      # Rule 2: no elaborated type specifiers inside namespace BF6Api.
      # "class AActor*" written in there declares BF6Api::AActor, a phantom
      # type that only resolves correctly when unity got there first.
      if ($L -match '^\s*namespace\s+BF6Api')     { $inNamespace = $true }
      if ($inNamespace -and $L -match '^\}')      { $inNamespace = $false }
      if ($inNamespace -and $L -match '\b(class|struct)\s+[A-Z]\w+\s*\*') {
        Note 'FAIL' $name $n 'forward declaration inside namespace - move it above'
      }

      # Rule 3: BF6BuildMode.cpp has no LOCTEXT_NAMESPACE, so LOCTEXT there
      # does not compile. It is an easy reflex from every other Slate file.
      if ($name -eq 'BF6BuildMode.cpp' -and $L -match '\bLOCTEXT\s*\(') {
        Note 'FAIL' $name $n 'LOCTEXT in a file with no namespace - use FText::FromString'
      }

      # Rule 4: user-facing text carries no em dashes and no emoji.
      # A standing rule: both read as machine-written in a creator tool.
      if ($L -match 'TEXT\("[^"]*[\u2014\u2013]') {
        Note 'FAIL' $name $n 'em dash in user-facing text - use a hyphen'
      }
      # .NET regex takes EXACTLY four hex digits after \u, so an astral
      # codepoint has to be written as its surrogate pair. Spelling one as
      # \u1F300 silently parses as "\u1F30 then 0" and matches most of Latin.
      if ($L -match 'TEXT\("[^"]*([\u2600-\u27BF\u2B00-\u2BFF\uFE0F]|[\uD83C-\uD83E][\uDC00-\uDFFF])') {
        Note 'FAIL' $name $n 'emoji in user-facing text'
      }

      # A double hyphen standing in for an em dash reads exactly as
      # machine-written as the em dash it replaces. Matched only where it is
      # used as PUNCTUATION - spaced, or joining two words - so that "i--" and
      # command-line flags like "--benchmark-file" stay legal.
      if ($L -match 'TEXT\("[^"]*(\s--\s|\w--\w)') {
        Note 'FAIL' $name $n 'double hyphen used as punctuation - rewrite the sentence'
      }
    }

  }

  # Rule 5: the tree-refresh invariant has ONE point of failure now.
  #
  # Every new actor needs a refresh, because an outliner row whose folder row
  # does not exist yet is dropped and never comes back. That used to be each
  # caller's job and five batch spawners forgot, so objects landed in the world
  # and nowhere in the tree. BF6_FileActor now marks the tree dirty and every
  # path that files an actor goes through it - which means deleting that one
  # line silently brings the whole bug class back. Guard it.
  $core = Join-Path $Private 'BF6UnrealSDK.cpp'
  if (Test-Path $core) {
    $txt = [string]::Join("`n", (Get-Content $core))
    $m = [regex]::Match($txt, '(?s)static void BF6_FileActor\(AActor\* A\)\s*\{.*?\n\}')
    if (-not $m.Success) {
      Note 'FAIL' 'BF6UnrealSDK.cpp' 0 'BF6_FileActor not found - the tree invariant guard cannot run'
    } elseif ($m.Value -notmatch 'MarkSceneTreeDirty') {
      Note 'FAIL' 'BF6UnrealSDK.cpp' 0 'BF6_FileActor no longer marks the tree dirty - spawned objects will go missing from the scene tree'
    }
  }


  # Rule 6: a text box should say what Enter does.
  #
  # "Type a custom map name and press Enter" did nothing for as long as the box
  # existed, because only the Create button was wired. Enter is what anyone
  # typing into a field expects, and a box that silently ignores it gives no
  # sign it heard you at all.
  #
  # A WARNING, not a failure: read-only boxes have nothing to commit, and a form
  # whose buttons are genuinely different choices (Conquest / Breakthrough) has
  # no single obvious Enter. The point is to force the decision, not the wiring.
  foreach ($f in $srcs) {
    if ($f.Extension -ne '.cpp') { continue }
    $ls = @(Get-Content $f.FullName)
    for ($i = 0; $i -lt $ls.Count; $i++) {
      if ($ls[$i] -notmatch 'S(Assign)?New\((\w+, )?S(MultiLine)?EditableTextBox\)') { continue }
      $to  = [Math]::Min($ls.Count - 1, $i + 12)
      $win = [string]::Join("`n", $ls[$i..$to])
      if ($win -match 'OnTextCommitted' -or $win -match 'IsReadOnly') { continue }
      Note 'WARN' $f.Name ($i + 1) 'text box ignores Enter - wire OnTextCommitted, or mark it read-only'
    }
  }

  if ($script:Fail -eq 0 -and $script:Warn -eq 0) { Write-Host "  clean" -ForegroundColor Green }
}

# ---------------------------------------------------------------------------
# COMPILE
# ---------------------------------------------------------------------------
function Invoke-Compile {
  Write-Host "`nCOMPILE (single-file, editor may stay open)" -ForegroundColor Cyan
  $targets = @()
  if ($Files) {
    # -Files takes a BARE NAME, and an add-on file does not live under the
    # tool Private directory. Resolving a name against nothing but $Private
    # handed UBT a path that does not exist, and UBT answers that by compiling
    # nothing and still reporting success.
    foreach ($f in $Files) {
      $leaf = Split-Path $f -Leaf
      $hit = Join-Path $Private $leaf
      if (-not (Test-Path $hit) -and (Test-Path $AddOns)) {
        $found = Get-ChildItem $AddOns -Filter $leaf -Recurse |
                 Where-Object { $_.FullName -notlike '*\Intermediate\*' } |
                 Select-Object -First 1
        if ($found) { $hit = $found.FullName }
      }
      if (-not (Test-Path $hit)) { Note 'FAIL' $leaf 0 'no such source file' ; continue }
      $targets += $hit
    }
  } else {
    $targets = @(Get-ChildItem $Private -Filter *.cpp | ForEach-Object { $_.FullName })
    if (Test-Path $AddOns) {
      $targets += @(Get-ChildItem $AddOns -Filter *.cpp -Recurse |
                    Where-Object { $_.FullName -notlike '*\Intermediate\*' -and $_.FullName -notlike '*\Binaries\*' } |
                    ForEach-Object { $_.FullName })
    }
  }

  foreach ($t in $targets) {
    $nm = Split-Path $t -Leaf
    $log = Join-Path $env:TEMP ("bf6check_" + $nm + ".log")
    & $UBT $Target Win64 Development -Project="$Project" -SingleFile="$t" -NoHotReload -NoHotReloadFromIDE > $log
    $code = $LASTEXITCODE
    $errs = @(Select-String -Path $log -Pattern 'error [A-Z]+\d+' -AllMatches)

    # THE EXIT CODE COUNTS, not just the log text.
    #
    # Grepping for "error C####" alone once reported OK on a file that had not
    # compiled at all: UBT can fail, or decide there is nothing to do, and
    # write a log with no compiler diagnostics in it. A check that passes when
    # nothing ran is worse than no check, because it is trusted.
    $ran = @(Select-String -Path $log -Pattern 'Compile \[x64\]').Count -gt 0
    if ($code -ne 0 -or -not $ran) {
      $script:Fail++
      $why = if ($code -ne 0) { "UBT exit $code" } else { "nothing was compiled" }
      Write-Host ("  FAIL {0}  ({1})" -f $nm, $why) -ForegroundColor Red
      if ($errs.Count -gt 0) { $errs | Select-Object -First 5 | ForEach-Object { Write-Host ("       " + $_.Line.Trim()) } }
      else { Get-Content $log -Tail 6 | ForEach-Object { Write-Host ("       " + $_.Trim()) } }
      Write-Host ("       full log: " + $log)
      continue
    }

    if ($errs.Count -eq 0) {
      Write-Host ("  OK   {0}" -f $nm) -ForegroundColor Green
    } else {
      $script:Fail += $errs.Count
      Write-Host ("  FAIL {0}  ({1} errors)" -f $nm, $errs.Count) -ForegroundColor Red
      $errs | Select-Object -First 5 | ForEach-Object { Write-Host ("       " + $_.Line.Trim()) }
      Write-Host ("       full log: " + $log)
    }
  }
}

Invoke-Lint
if (-not $Lint) { Invoke-Compile }

Write-Host ""
if ($Fail -gt 0) { Write-Host "$Fail failure(s), $Warn warning(s)" -ForegroundColor Red; exit 1 }
Write-Host "passed ($Warn warning(s))" -ForegroundColor Green
exit 0
