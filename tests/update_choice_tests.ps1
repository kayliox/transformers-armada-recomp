# update_choice_tests.ps1 - TODO 425 / 426, against small throw-away repositories (nothing of the game is needed):
#   1. tools\update_forecast.ps1: how much of the game an update rebuilds (none / small / translate / most), from git
#      and the patch series alone - a patch that changes a header the translated code includes, a patch that only moved
#      to new context lines, a runtime .cpp, the recompiler's sources, the config, documents.
#   2. update.ps1 -Play and "Start the game when the update is done" (states\update_options.json): ticked or missing,
#      the game is started; unticked, the update ends on "Update done" and the game stays closed - after a failure too.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests\update_choice_tests.ps1   (exit code 0: all passed)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
# (the report a stopped setup / update writes, tools\setup_report.ps1: into a temp folder, never the Desktop)
$env:TF_REPORT_DIR = Join-Path ([IO.Path]::GetTempPath()) "tfa_test_reports"
New-Item -ItemType Directory -Force $env:TF_REPORT_DIR | Out-Null
$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failed++ }
}
function G([string]$Dir) {
    $a = $args
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { $out = & git -C $Dir -c user.name=test -c user.email=test@test -c core.autocrlf=false @a 2>&1 | ForEach-Object { "$_" } } finally { $ErrorActionPreference = $eap }
    if ($LASTEXITCODE -ne 0) { throw "git $($a -join ' '): $out" }
    $out
}
function Write-Text([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
    [IO.File]::WriteAllText($Path, $Text.Replace("`r`n", "`n"), [Text.UTF8Encoding]::new($false))
}
$temp = Join-Path ([IO.Path]::GetTempPath()) ("tfa_update_tests_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory $temp | Out-Null
try {
    # ---------------------------------------------------------------------------------------------------------------
    Write-Host "1. update_forecast.ps1"
    $r = Join-Path $temp "fc"
    New-Item -ItemType Directory $r | Out-Null
    G $r init -q | Out-Null
    Copy-Item (Join-Path $Root "tools\update_forecast.ps1") (Join-Path $r "update_forecast.ps1")
    # (the patched runtime as a build leaves it, and a generated file: its includes are what the translated code uses)
    Write-Text "$r\PS2Recomp\ps2xRuntime\include\ps2_runtime.h" "#include `"runtime/ee_scheduler.h`"`n"
    Write-Text "$r\PS2Recomp\ps2xRuntime\include\runtime\ee_scheduler.h" "// scheduler`n"
    Write-Text "$r\PS2Recomp\ps2xRuntime\include\runtime\ps2_host_present.h" "// present, not included by the translated code`n"
    Write-Text "$r\work\generated\sub_0x100000.cpp" "#include `"ps2_runtime.h`"`n"
    Write-Text "$r\.gitignore" "/PS2Recomp/`n/work/`n/update_forecast.ps1`n"
    function Patch([string]$File, [string]$Context, [string]$Line) {
        "From: test`nSubject: [PATCH] test`n---`ndiff --git a/$File b/$File`n--- a/$File`n+++ b/$File`n@@ -1,1 +1,2 @@`n $Context`n+$Line`n"
    }
    Write-Text "$r\patches\ps2recomp\0001-sched.patch" (Patch "ps2xRuntime/include/runtime/ee_scheduler.h" "// a" "int one;")
    Write-Text "$r\patches\ps2recomp\0002-present.patch" (Patch "ps2xRuntime/src/lib/ps2_host_present.cpp" "// b" "int two;")
    Write-Text "$r\README.md" "readme`n"
    G $r add -A | Out-Null; G $r commit -qm base | Out-Null
    $script:prev = (G $r rev-parse HEAD).Trim()
    function Step-Forecast([string]$What, [scriptblock]$Change, [string]$Want, [string]$WhyLike = "*") {
        & $Change
        G $r add -A | Out-Null; G $r commit -qm $What | Out-Null
        $head = (G $r rev-parse HEAD).Trim()
        $out = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r "update_forecast.ps1") -From $script:prev -To $head -Port $r | Select-Object -First 1)
        $kind, $min, $why = "$out" -split '\|', 3
        Check ($kind -eq $Want -and "$why" -like $WhyLike) "$What -> $Want (got: $out)"
        $script:prev = $head
    }
    Step-Forecast "documents only" { Write-Text "$r\README.md" "readme 2`n"; Write-Text "$r\docs\x.md" "x`n" } "none"
    Step-Forecast "a runtime .cpp's patch" { Write-Text "$r\patches\ps2recomp\0002-present.patch" (Patch "ps2xRuntime/src/lib/ps2_host_present.cpp" "// b" "int three;") } "small"
    Step-Forecast "a header the translated code includes" { Write-Text "$r\patches\ps2recomp\0001-sched.patch" (Patch "ps2xRuntime/include/runtime/ee_scheduler.h" "// a" "int four;") } "most" "*ee_scheduler.h*"
    Step-Forecast "the same patch on new context lines" { Write-Text "$r\patches\ps2recomp\0001-sched.patch" (Patch "ps2xRuntime/include/runtime/ee_scheduler.h" "// a moved" "int four;") } "small"
    Step-Forecast "a new patch on a header the translated code does not include" { Write-Text "$r\patches\ps2recomp\0003-ph.patch" (Patch "ps2xRuntime/include/runtime/ps2_host_present.h" "// c" "int five;") } "small"
    Step-Forecast "a new patch on that header" { Write-Text "$r\patches\ps2recomp\0004-rt.patch" (Patch "ps2xRuntime/include/ps2_runtime.h" "// d" "int six;") } "most" "*ps2_runtime.h*"
    Step-Forecast "the recompiler's sources" { Write-Text "$r\patches\ps2recomp\0005-rc.patch" (Patch "ps2xRecomp/src/code_generator.cpp" "// e" "int seven;") } "translate"
    Step-Forecast "the translation's config" { Write-Text "$r\transformers\config\game.json" "{}`n" } "translate"
    Step-Forecast "the game's own code" { Write-Text "$r\transformers\src\main.cpp" "int main(){}`n" } "small"
    $text = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r "update_forecast.ps1") -From "HEAD~7" -To "HEAD~6" -Port $r -Text | Select-Object -First 1)
    Check ("$text" -like "This update rebuilds most of the game, because *ee_scheduler.h*: about * on this PC.") "-Text: the sentence for the player ($text)"
    $bad = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r "update_forecast.ps1") -From "0000000000000000000000000000000000000000" -To HEAD -Port $r
    Check ($LASTEXITCODE -ne 0 -and -not $bad) "a commit git does not have: nothing said, a failing exit code"

    # ---------------------------------------------------------------------------------------------------------------
    Write-Host "2. update.ps1 -Play and the choice"
    $origin = Join-Path $temp "origin.git"
    $c = Join-Path $temp "copy"
    New-Item -ItemType Directory $c | Out-Null
    G $c init -q -b main | Out-Null
    foreach ($f in "update.ps1", "tools\setup_common.ps1", "tools\step_log.ps1", "tools\install_layout.ps1", "tools\update_forecast.ps1") {
        New-Item -ItemType Directory -Force (Split-Path (Join-Path $c $f)) | Out-Null
        Copy-Item (Join-Path $Root $f) (Join-Path $c $f)
    }
    Write-Text "$c\.gitignore" "/states/`n/.setup.json`n/update.log`n/update_steps.log`n/out/`n"
    G $c add -A | Out-Null; G $c commit -qm one | Out-Null
    G $temp clone -q --bare $c $origin | Out-Null
    G $c remote add origin $origin | Out-Null
    G $c fetch -q origin | Out-Null
    G $c branch -q -u origin/main | Out-Null
    $head = (G $c rev-parse HEAD).Trim()
    Write-Text "$c\.setup.json" "{ `"Iso`": `"`", `"SetUpAt`": `"$head`" }"
    $env:TFA_UPDATE_NO_PAUSE = "1"
    function Run-Update([string]$Choice) {
        $opt = Join-Path $c "states\update_options.json"
        if ($Choice) { Write-Text $opt $Choice } else { Remove-Item -LiteralPath $opt -ErrorAction SilentlyContinue }
        Remove-Item -LiteralPath (Join-Path $c "states\update_result.txt") -ErrorAction SilentlyContinue
        $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $c "update.ps1") -Play 6>&1 2>&1 | ForEach-Object { "$_" }
        , @($out)
    }
    # (an up-to-date copy: the update ends at once, where it starts the game - here, a missing exe says so)
    $out = Run-Update ""
    Check (($out -join "`n") -match "Starting the game") "no choice saved: the game is started (as before the choice)"
    $out = Run-Update "{`n    `"start_game`":  true`n}`n"
    Check (($out -join "`n") -match "Starting the game") "ticked: the game is started"
    $out = Run-Update "{`"start_game`":false}"
    $all = $out -join "`n"
    Check ($all -match "Update done\. The game is not started" -and $all -notmatch "Starting the game") "unticked: 'Update done', the game not started"
    Check ((Get-Content (Join-Path $c "states\update_result.txt") -Raw) -like "ok *") "unticked: the result still written for the game's next start"
    $out = Run-Update "not json"
    Check (($out -join "`n") -match "Starting the game") "an unreadable choice counts as ticked"
    # (a failure: the setup state gone - the update stops; unticked, the game is not started then either)
    Rename-Item (Join-Path $c ".setup.json") "setup.json.away"
    $out = Run-Update "{`"start_game`": false}"
    $all = $out -join "`n"
    Check ($all -match "run setup.ps1 first" -and $all -match "The game is not started" -and $all -notmatch "Update done\." -and $all -notmatch "Starting the game") "unticked, after a failure: the game not started, no 'Update done'"
    $out = Run-Update ""
    Check (($out -join "`n") -match "Starting the game") "ticked, after a failure: the game started as it was"
    Rename-Item (Join-Path $c "setup.json.away") ".setup.json"
    # (TODO 425: a new commit waiting - the update says up front how much it rebuilds, here nothing: -CheckOnly stops there)
    $w = Join-Path $temp "work2"
    G $temp clone -q $origin $w | Out-Null
    Write-Text "$w\docs\notes.md" "notes`n"
    G $w add -A | Out-Null; G $w commit -qm docs | Out-Null; G $w push -q origin main | Out-Null
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $c "update.ps1") -CheckOnly 6>&1 2>&1 | ForEach-Object { "$_" }
    $all = $out -join "`n"
    Check ($all -match "This update needs no rebuild of the game\." -and $LASTEXITCODE -eq 0) "a documents-only update: 'This update needs no rebuild' said before it starts"
} finally {
    Remove-Item Env:TFA_UPDATE_NO_PAUSE -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "`n$failed check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nall passed" -ForegroundColor Green
exit 0
