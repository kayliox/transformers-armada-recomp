<#
.SYNOPSIS
    (TODO 425) How much of the game an update will rebuild, said before it starts.

.DESCRIPTION
    Reads only git and the files the last build left (nothing is changed, nothing is built). For the commits between
    -From (the build's commit, .setup.json's SetUpAt) and -To (the new one) it answers with one line:

        <kind>|<minutes>|<why>

    kind  none   nothing the game is built from changed (documents, the setup program, these scripts)
          small  only some of the game's own files: the compile redoes those (minutes)
          translate  the game's code is translated again (the recompiler or what it reads changed): minutes more,
                 and the translated files that come out different are compiled again (when the code the recompiler
                 writes changed, that is most of the game: it cannot be told before the translation)
          most   most of the game is compiled again: a runtime header that every part of the translated game code
                 includes changed, or the PS2Recomp submodule moved
    minutes  the estimate on this PC, from out\build-times.json (the record of earlier builds; 0 for none)
    why      plain words for the player ("" for none)
    -Text: one sentence for the player instead ("This update rebuilds most of the game (...): about 25 min on this PC.").

    The PS2Recomp patch series (patches\ps2recomp): for each patch the update changes, the lines it adds and removes in
    each file, before and after. A file whose lines differ is one the update changes. When that file is
    one of the headers the translated code includes (found by following the includes of a generated file, as build.ps1's
    compile forecast does), the whole game is compiled again: that is real, not a fault, and only said up front.
    Used by update.ps1 (said before the pull) and by tfa-setup.exe's update screen.

.EXAMPLE
    .\tools\update_forecast.ps1 -From 1137f27 -To e1d86d2
#>
param(
    [Parameter(Mandatory)][string]$From,
    [Parameter(Mandatory)][string]$To,
    [string]$Port = (Split-Path $PSScriptRoot -Parent),   # the game's folder (this script's parent)
    [string]$Repo,                                         # the git repository's top (default: the game's folder)
    [switch]$Text                                          # the answer as a sentence for the player
)
$ErrorActionPreference = "Stop"
if (-not $Repo) { $Repo = $Port }
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$Port = (Resolve-Path -LiteralPath $Port).Path

function Invoke-Git {
    $a = $args
    $eap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $out = & git -C $Repo -c core.longpaths=true -c core.quotepath=false @a 2>$null; if ($LASTEXITCODE -ne 0) { return $null }; , @($out) }
    finally { $ErrorActionPreference = $eap }
}
function Answer([string]$Kind, [double]$Minutes, [string]$Why) {
    $min = [int][Math]::Max(1, [Math]::Ceiling($Minutes))
    if (-not $Text) { Write-Output ("{0}|{1}|{2}" -f $Kind, $(if ($Kind -eq "none") { 0 } else { $min }), $Why); exit 0 }
    $about = if ($min -le 1) { "about a minute" } else { "about $min min" }
    switch ($Kind) {
        "none" { Write-Output "This update needs no rebuild of the game." }
        "small" { Write-Output "This update rebuilds only a few of the game's files: $about on this PC." }
        "translate" { Write-Output "This update translates the game's code again: $about on this PC, longer if the translated code itself changed." }
        default { Write-Output "This update rebuilds most of the game, because $($Why): $about on this PC." }
    }
    exit 0
}

# The prefix of the game's folder in the repository ("" in a clone of the game's repository; "port/" in one holding it)
$pre = ""
if ($Port -ne $Repo -and $Port.StartsWith($Repo, [StringComparison]::OrdinalIgnoreCase)) {
    $pre = $Port.Substring($Repo.Length).TrimStart('\', '/').Replace('\', '/') + "/"
}
$changed = Invoke-Git diff --name-only $From $To
if ($null -eq $changed) { exit 1 }   # (a commit git does not have: nothing said)
$changed = @($changed | Where-Object { $_ -and $_.StartsWith($pre) } | ForEach-Object { $_.Substring($pre.Length) })

# What never needs the game rebuilt (as update.ps1's list)
$noBuild = @("docs/*", "*.md", "installer/*", ".github/*", ".githooks/*", "setup.ps1", "update.ps1", "setup.sh", "update.sh",
    "Play.bat", "play.sh", "tests/*", "tools/setup_common.ps1", "tools/step_log.ps1", "tools/update_forecast.ps1",
    "tools/check_no_game_data.py", "tools/check_publishable.py", "LICENSE", ".gitignore", ".gitattributes")
$build = @($changed | Where-Object { $p = $_; -not ($noBuild | Where-Object { $p -like $_ }) })
if (-not $build.Count) { Answer "none" 0 "" }

# The record of earlier builds on this PC: seconds per batch of generated code, per other file, the link, each step
$gentle = $false
try { $gentle = [bool](Get-Content (Join-Path $Port "hd_textures\hd_options.json") -Raw -ErrorAction Stop | ConvertFrom-Json).gentle } catch {}
$times = @{}
foreach ($name in $(if ($gentle) { "build-times-gentle.json", "build-times.json" } else { , "build-times.json" })) {
    try {
        (Get-Content (Join-Path $Port "out\$name") -Raw -ErrorAction Stop | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $times[$_.Name] = [double]$_.Value }
        break
    } catch {}
}
function T([string]$k, [double]$default) { if ($times.ContainsKey($k)) { $times[$k] } else { $default } }
$batchFiles = 0
try { $batchFiles = @([IO.Directory]::EnumerateFiles((Join-Path $Port "work\batches"), "batch_*.cpp")).Count } catch {}
if (-not $batchFiles) { $batchFiles = 330 }   # (a release's count, about)
$fixed = (T "tools" 20) + (T "configure" 20) + (T "link" 30) + 30       # (checks, configure, link: every build)
$fullCompile = $batchFiles * (T "compile_batch" 3.0) + 150 * (T "compile_other" 0.8)
$translate = (T "translate" 120) + (T "sync" 30)

# 1. The PS2Recomp submodule moved: its whole tree is new.
if ($build -contains "PS2Recomp") { Answer "most" (($fixed + $translate + $fullCompile) / 60) "the recompiler (PS2Recomp) moved to a new version" }

# 2. The PS2Recomp patch series: in each patch the update changed (added, changed, removed), the lines it adds and
#    removes in each file, before and after. A file whose lines differ is a file the update changes (a patch only moved
#    to new context lines changes nothing; a patch split in two may name a file that comes out as before: said safely).
function Get-PatchLines([string]$Commit, [string]$Patch) {
    $files = @{}
    $text = Invoke-Git show "${Commit}:$Patch"
    if ($null -eq $text) { return $files }
    $file = $null
    foreach ($line in $text) {
        if ($line.StartsWith("diff --git ")) { $file = ($line -split ' b/', 2)[1]; continue }
        if ($line.StartsWith("+++ ")) { $n = $line.Substring(4); if ($n -ne "/dev/null") { $file = $n -replace '^b/', '' }; continue }
        if ($line.StartsWith("--- ") -or $line.StartsWith("@@") -or -not $file) { continue }
        if ($line.StartsWith("+") -or $line.StartsWith("-")) {
            if (-not $files.ContainsKey($file)) { $files[$file] = [Text.StringBuilder]::new() }
            [void]$files[$file].AppendLine($line)
        }
    }
    $files
}
$runtimeChanged = @()
foreach ($patch in @($build | Where-Object { $_ -like "patches/ps2recomp/*.patch" })) {
    $old = Get-PatchLines $From "$pre$patch"
    $new = Get-PatchLines $To "$pre$patch"
    foreach ($f in (@($old.Keys) + @($new.Keys) | Select-Object -Unique)) {
        $a = if ($old.ContainsKey($f)) { $old[$f].ToString() } else { "" }
        $b = if ($new.ContainsKey($f)) { $new[$f].ToString() } else { "" }
        if ($a -ne $b -and $runtimeChanged -notcontains $f) { $runtimeChanged += $f }
    }
}

# 3. The game's code translated again: the recompiler's or the analyzer's sources, the tools that run them, the config.
$translation = @($build | Where-Object { $_ -like "transformers/config/*" -or $_ -in @("transformers/tools/find_functions.py",
    "transformers/tools/make_config.py", "transformers/tools/sync_generated.py") })
$recompiler = @($runtimeChanged | Where-Object { $_ -notlike "ps2xRuntime/*" -and $_ -notlike "ps2xIOP/*" })
$retranslate = $translation.Count -or $recompiler.Count

# 4. A header every part of the translated code includes: the includes of one generated file, followed (build.ps1's walk).
$headers = @{}
if ($runtimeChanged.Count) {
    $ps2 = Join-Path $Port "PS2Recomp"
    $incDirs = @((Join-Path $ps2 "ps2xRuntime\include"), (Join-Path $ps2 "ps2xIOP\include"), (Join-Path $ps2 "ps2xRuntime\src\lib"))
    $sample = $null
    try { $sample = [IO.Directory]::EnumerateFiles((Join-Path $Port "work\generated"), "*_0x*.cpp") | Select-Object -First 1 } catch {}
    $includeRe = [regex]'(?m)^\s*#\s*include\s*[<"]([^>"]+)[>"]'
    $todo = [Collections.Generic.Queue[string]]::new()
    if ($sample) { $todo.Enqueue($sample) }
    else {
        # (no generated code yet: the headers a generated file starts with - sync_generated.py's - followed from there)
        foreach ($h in "ps2_runtime_macros.h", "ps2_runtime.h", "ps2_syscalls.h", "ps2_stubs.h", "ps2_register_cache.h") {
            $f = Join-Path $ps2 "ps2xRuntime\include\$h"
            if (Test-Path -LiteralPath $f) { $todo.Enqueue($f) }
        }
    }
    $seen = @{}
    while ($todo.Count) {
        $path = $todo.Dequeue()
        if ($seen.ContainsKey($path)) { continue }
        $seen[$path] = $true
        foreach ($m in $includeRe.Matches([IO.File]::ReadAllText($path))) {
            foreach ($d in @([IO.Path]::GetDirectoryName($path)) + $incDirs) {
                $f = [IO.Path]::GetFullPath([IO.Path]::Combine($d, $m.Groups[1].Value))
                if ([IO.File]::Exists($f)) { $todo.Enqueue($f); break }
            }
        }
    }
    $root = [IO.Path]::GetFullPath($ps2).TrimEnd('\') + '\'
    foreach ($p in $seen.Keys) { if ($p.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { $headers[$p.Substring($root.Length).Replace('\', '/')] = $true } }
}
$hit = @($runtimeChanged | Where-Object { $headers.ContainsKey($_) })
if ($hit.Count) {
    $names = ($hit | ForEach-Object { Split-Path $_ -Leaf }) -join ", "
    Answer "most" (($fixed + $fullCompile) / 60) "a runtime header that all of the game's translated code includes changed ($names)"
}
# 5. The rest: the game's own files, a few runtime files - minutes (and the translation, when it is redone).
$others = @($build).Count + $runtimeChanged.Count
$small = $fixed + [Math]::Min(150, $others) * (T "compile_other" 0.8) * 3
if ($retranslate) { Answer "translate" (($small + $translate) / 60) "the game's code is translated again (the recompiler or what it reads changed)" }
Answer "small" ($small / 60) ""
