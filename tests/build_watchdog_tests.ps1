# build_watchdog_tests.ps1 - (TODO 441) build.ps1's watchdog over its long one-program steps (the SDK scan, the
# translation; tools\step_watchdog.ps1), and the steps' usual times said on a first build. build.ps1's own functions are taken from it (its
# syntax tree) and run in a child Windows PowerShell; the "program" is a copy of Windows' waitfor.exe, which sits
# still (no output, no processor time) until its time-out - as an analyzer held by an antivirus does. Nothing is built.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests\build_watchdog_tests.ps1   (exit code 0: all passed)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failed++ }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("tfa_watchdog_test_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    # build.ps1's functions the steps use, as they are written there
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Root "build.ps1"), [ref]$null, [ref]$null)
    $want = "Format-Estimate", "Get-StageSeconds", "Usual", "RunLogged"
    $defs = $ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $want -contains $n.Name }, $true)
    Check ($defs.Count -eq $want.Count) "build.ps1 has the functions its steps use ($($defs.Count) of $($want.Count))"
    Check ((Get-Content (Join-Path $Root "build.ps1") -Raw).Contains('. (Join-Path $Root "tools\step_watchdog.ps1")')) "build.ps1 loads tools\step_watchdog.ps1"
    $first = $ast.FindAll({ param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and "$($n.Left)" -eq '$FirstTimes' }, $true) | Select-Object -First 1
    $exe = Join-Path $tmp "ps2_analyzer.exe"
    Copy-Item (Join-Path $env:SystemRoot "System32\waitfor.exe") $exe
    $harness = Join-Path $tmp "harness.ps1"
    $code = @(
        '$ErrorActionPreference = "Stop"'
        '$History = @{}; $Root = "C:\Games\TFA\transformers-armada-recomp"; $script:stage = "scan"'
        "$first"
        'function Assert-NotBlocked { }'
        ". '$(Join-Path $Root "tools\step_watchdog.ps1")'"
        ($defs | ForEach-Object { $_.Extent.Text })
        'Write-Output "USUAL-FIRST:$(Usual ''scan'')|$(Usual ''translate'')|$(Usual ''link'')"'
        '$History["scan"] = 19.2; Write-Output "USUAL-FAST:$(Usual ''scan'')"; $History["scan"] = 200; Write-Output "USUAL-SLOW:$(Usual ''scan'')"; $History = @{}'
        'try { RunLogged $args[0] @("/T", "9", "tfawatchdogtest") $args[1] "The SDK scan" } catch { Write-Output "RUNLOGGED-THREW:$($_.Exception.Message)" }'
        'Write-Output "DONE"'
    ) -join "`r`n"
    [IO.File]::WriteAllText($harness, $code)
    $log = Join-Path $tmp "analyzer.log"
    $steps = Join-Path $tmp "steps.log"
    $env:TF_WATCHDOG_SECONDS = "3"; $env:TF_WATCHDOG_LONG_SECONDS = "6"; $env:TF_STEP_LOG = $steps
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $out = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $harness $exe $log 2>&1 | ForEach-Object { "$_" })
    $took = $clock.Elapsed.TotalSeconds
    Remove-Item Env:TF_WATCHDOG_SECONDS, Env:TF_WATCHDOG_LONG_SECONDS, Env:TF_STEP_LOG
    $text = $out -join "`n"

    Check ($text -match "USUAL-FIRST: \(usually under a minute\)\| \(usually about a minute\)\|") "a first build says the scan's and the translation's usual time, nothing for others"
    Check ($text -match "USUAL-FAST:`n" -or $text -match "USUAL-FAST:$") "a fast step on this PC: nothing said (as before)"
    Check ($text -match "USUAL-SLOW: \(usually about 3 min here\)") "a slow step on this PC: its own time"
    Check ($text -match "The SDK scan has made no progress for \d+ min \(ps2_analyzer\.exe is running but doing nothing; it usually takes under a minute\)") "a still program: said, with what it is and its usual time"
    Check ($text -match "an antivirus checking or holding ps2_analyzer\.exe, which was just built on this PC" -and $text -match "the disc image: it must be a complete copy") "what to try: the antivirus, the disc"
    Check ($text -match "You can close the setup and run it again: finished steps are kept") "running the setup again is said to be safe"
    Check ($text -match "No progress for \d+ min: an antivirus may hold ps2_analyzer\.exe, or check the disc") "the short last line the setup window shows"
    Check ($text -match "The SDK scan has been running for \d+ min, far longer than usual") "a program busy far past its time: said too"
    Check ((Test-Path $steps) -and ((Get-Content $steps -Raw) -match "build: The SDK scan has made no progress")) "the step log has it too"
    Check ($text -match "RUNLOGGED-THREW:ps2_analyzer\.exe failed with exit code 1" -and $text -match "DONE") "the program's own ending still decides (waitfor timed out: exit code 1)"
    Check ($took -lt 30) "the watchdog never holds the step up ($([int]$took) s)"
    $said = ([regex]::Matches($text, "has made no progress")).Count
    Check ($said -eq 1) "said once, not every check ($said)"
    $lines = @($out | Where-Object { $_ -match "^No progress for|far longer than usual\. Check the disc;" })
    Check (($lines | Where-Object { $_.Length -gt 110 }).Count -eq 0) "the short line fits the setup window"
    Check ($text -notmatch [char]0x2014) "no long dashes"
    if ($failed) { Write-Host ($out -join "`n") }
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "`n$failed check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nall passed" -ForegroundColor Green
exit 0
