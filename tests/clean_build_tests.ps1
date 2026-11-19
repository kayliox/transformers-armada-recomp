# clean_build_tests.ps1 - (TODO 439) tools\clean_build.ps1 on a made-up build tree, the update forecast after it, and the
# setup's plan text (tools\setup_common.ps1 Get-SetupPlanText). Nothing outside a temporary folder is touched.
#   - -Report: the game's folder, the temporary files and the in-between files, each with its bytes;
#   - -Level temp: only work\staging goes; -Level all: everything but the game's own folder goes, and out\build-cleaned.txt
#     makes tools\update_forecast.ps1 say the next update rebuilds most of the game, until a build after it;
#   - the plan: a program already installed is said as such and not counted; a missing one with its sizes.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests\clean_build_tests.ps1   (exit code 0: all passed)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failed++ }
}
function Put([string]$path, [int]$bytes) {
    New-Item -ItemType Directory -Force (Split-Path $path -Parent) | Out-Null
    [IO.File]::WriteAllBytes($path, [byte[]]::new($bytes))
}
function Report([string]$port) {
    $r = @{}
    foreach ($l in @(& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $port "tools\clean_build.ps1") -Report)) {
        $p = "$l".Split('|'); $r[$p[0]] = [long]$p[1]
    }
    return $r
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("tfa_clean_test_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
try {
    # A game folder as a build leaves it (sizes small, the structure as it is).
    $port = Join-Path $tmp "transformers-armada-recomp"
    New-Item -ItemType Directory -Force (Join-Path $port "tools") | Out-Null
    Copy-Item (Join-Path $Root "tools\clean_build.ps1"), (Join-Path $Root "tools\update_forecast.ps1") (Join-Path $port "tools")
    $game = Join-Path $port "out\build\transformers\RelWithDebInfo"
    Put "$game\TransformersRecomp.exe" 1000; Put "$game\TransformersRecomp.pdb" 3000; Put "$game\amd_fidelityfx_upscaler_dx12.dll" 500
    Put "$port\out\build\transformers\TransformersRecomp.dir\RelWithDebInfo\nr_core.obj" 7000
    Put "$port\out\build\ps2xRuntime\ps2_runtime.dir\RelWithDebInfo\a.obj" 6000
    Put "$port\out\build\CMakeCache.txt" 100
    Put "$port\out\tools\ps2xRecomp\RelWithDebInfo\ps2_recomp.exe" 2000
    Put "$port\work\staging\func_0x100000.cpp" 400; Put "$port\work\staging\func_0x100010.cpp" 400
    Put "$port\work\generated\func_0x100000.cpp" 400; Put "$port\work\batches\batch_0000.cpp" 50
    Put "$port\out\build-times.json" 20
    # (a long path, deeper than Windows PowerShell's 260 characters: rd's \\?\ form deletes it)
    $deep = "$port\out\build\transformers\TransformersRecomp.dir\RelWithDebInfo\" + ("d" * 60) + "\" + ("e" * 60) + "\" + ("f" * 60) + "\x.tlog"
    try { [IO.Directory]::CreateDirectory("\\?\" + (Split-Path $deep -Parent)) | Out-Null; [IO.File]::WriteAllBytes("\\?\$deep", [byte[]]::new(10)) } catch {}

    $r = Report $port
    Check ($r.game -eq 4500) "report: the game's own folder ($($r.game) bytes)"
    Check ($r.temp -eq 800) "report: the temporary files, work\staging ($($r.temp))"
    Check ($r.build -ge 15550 -and $r.build -le 15560) "report: the in-between files, out\build without the game, out\tools, work\ ($($r.build))"

    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $port "tools\clean_build.ps1") -Level temp | Out-Null
    Check (-not (Test-Path "$port\work\staging")) "temp: work\staging deleted"
    Check ((Test-Path "$port\work\generated\func_0x100000.cpp") -and (Test-Path "$port\out\build\CMakeCache.txt") -and (Test-Path "$port\out\tools")) "temp: the in-between files kept"
    Check (-not (Test-Path "$port\out\build-cleaned.txt")) "temp: no full-rebuild mark"

    # The forecast before and after -Level all: a git history where a game source changed.
    Push-Location $port
    try {
        & git init -q 2>$null; & git config user.email t@t.invalid; & git config user.name t
        Put "$port\transformers\src\x.cpp" 10; & git add transformers 2>$null; & git commit -q -m a 2>$null; $from = (& git rev-parse HEAD).Trim()
        Put "$port\transformers\src\x.cpp" 11; & git add transformers 2>$null; & git commit -q -m b 2>$null; $to = (& git rev-parse HEAD).Trim()
    } finally { Pop-Location }
    $f1 = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $port "tools\update_forecast.ps1") -From $from -To $to -Port $port
    Check ("$f1" -like "small|*") "forecast before the clean-up: a few files ($f1)"

    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $port "tools\clean_build.ps1") -Level all | Out-Null
    Check ((Test-Path "$game\TransformersRecomp.exe") -and (Test-Path "$game\TransformersRecomp.pdb") -and (Test-Path "$game\amd_fidelityfx_upscaler_dx12.dll")) "all: the game's folder kept whole"
    $left = @(Get-ChildItem "$port\out\build" -Recurse -File | ForEach-Object { $_.FullName.Substring($port.Length + 1) })
    Check ($left.Count -eq 3) "all: nothing else left in out\build ($($left -join ', '))"
    Check (-not (Test-Path "$port\out\tools") -and -not (Test-Path "$port\work\generated") -and -not (Test-Path "$port\work\batches")) "all: out\tools and work\ deleted"
    Check ((Test-Path "$port\out\build-times.json") -and (Test-Path "$port\out\build-cleaned.txt")) "all: the build's time record kept, the full-rebuild mark written"
    $r = Report $port
    Check ($r.temp -eq 0 -and $r.build -eq 0 -and $r.game -eq 4500) "report after: nothing left to free"

    $f2 = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $port "tools\update_forecast.ps1") -From $from -To $to -Port $port
    Check ("$f2" -like "most|*|its build files were deleted*") "forecast after the clean-up: most of the game ($f2)"
    $t2 = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $port "tools\update_forecast.ps1") -From $from -To $to -Port $port -Text
    Check ("$t2" -like "This update rebuilds most of the game, because its build files were deleted to save space*") "the sentence the setup window shows ($t2)"
    # (a build after the clean-up: the game's exe newer than the mark - the forecast is its usual one again)
    [IO.File]::SetLastWriteTimeUtc("$port\out\build-cleaned.txt", (Get-Date).ToUniversalTime().AddMinutes(-5))
    [IO.File]::SetLastWriteTimeUtc("$game\TransformersRecomp.exe", (Get-Date).ToUniversalTime())
    $f3 = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $port "tools\update_forecast.ps1") -From $from -To $to -Port $port
    Check ("$f3" -like "small|*") "forecast after a rebuild: the mark no longer counts ($f3)"

    # The game running from the folder: nothing is deleted (simulated by the check's own words being there).
    Check ((Get-Content (Join-Path $Root "tools\clean_build.ps1") -Raw).Contains("the game is running from this folder")) "a running game stops the clean-up"
} finally {
    if (Test-Path $tmp) { & cmd.exe /d /c "rd /s /q `"\\?\$tmp`"" 2>$null | Out-Null }
}

# The setup's plan (tools\setup_common.ps1).
. (Join-Path $Root "tools\setup_common.ps1")
$none = @(Get-SetupPlanText -Missing @() -HdGB 1.4 -HdMinutes 25)
$all = @(Get-SetupPlanText -Missing @("git", "python", "vs") -HdGB 1.4 -HdMinutes 25)
$skip = @(Get-SetupPlanText -Missing @() -HdGB 0 -HdMinutes 0)
Check (($none | Where-Object { $_ -like "  *already installed*" }).Count -eq 3) "plan: three programs already installed"
Check (($all | Where-Object { $_ -like "  Visual Studio 2022 Build Tools (C++): to install (1.9 GB download, 6.1 GB on disk)*" }).Count -eq 1) "plan: the Build Tools with their sizes"
Check (($all | Where-Object { $_ -like "  Git: to install (67 MB download*" }).Count -eq 1) "plan: Git with its sizes"
$gbOf = { param($lines) [int][regex]::Match(($lines | Where-Object { $_ -like "In all:*" }), 'about (\d+) GB').Groups[1].Value }
Check ((& $gbOf $all) - (& $gbOf $none) -ge 6) "plan: the missing programs counted in the total ($(& $gbOf $none) vs $(& $gbOf $all) GB)"
Check (($skip | Where-Object { $_ -like "HD textures: left out*" }).Count -eq 1 -and (& $gbOf $skip) -lt (& $gbOf $none)) "plan: HD textures left out, not counted"
Check (($all | Where-Object { $_ -like "*download*" -and $_ -like "In all:*" }).Count -eq 1) "plan: the download total said"
$cost = Get-HdPresetCost "fast"; Check ($cost[0] -eq 0.6 -and $cost[1] -eq 10) "plan: the Fast preset's size and time"
Check (($none + $all) -join "`n" -notmatch [char]0x2014) "plan: no long dashes"

if ($failed) { Write-Host "`n$failed check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nall passed" -ForegroundColor Green
exit 0
