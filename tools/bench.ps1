<#
.SYNOPSIS
    Measures how fast a stretch of the game runs: plays it from a save state as fast as the
    host can (turbo mode, so the work is the same every time) and reports the frame rate.

.DESCRIPTION
    The game draws 30 frames per 60 VBlanks. A turbo rate below 30 frames per second means
    the scene cannot run at full speed; the margin above 30 is what is left for sound and
    for a busier moment. -RealTime runs at the game's own pace instead and shows what a
    player gets (VBlanks per second below 60 = slow motion).

.EXAMPLE
    .\tools\bench.ps1                                  # the dropship area, 600 ticks
    .\tools\bench.ps1 -State amazon -Ticks 400 -Goto ""
    .\tools\bench.ps1 -Profile -Lines "DrawFast,PrepareTexture"   # with runs\profile.txt
    .\tools\bench.ps1 -Set @{ PS2X_GS_THREADS = "4" }
#>
param(
    [string]$State = "a11",
    [int]$Ticks = 600,
    # Where to walk (TF_GOTO, ticks from the start). The default is the way from the
    # dropship to the first Mini-Con, the heaviest stretch found so far.
    [string]$Goto = "20:468.5,-1743.7,2;20:465.5,-1740.7,2;20:462.5,-1740.7,2;20:459.5,-1739.7,2;20:456.5,-1737.7,2;20:450.5,-1737.7,2;20:444.5,-1737.7,2;20:441.5,-1737.7,2;20:436.5,-1732.7,2;20:431.5,-1727.7,2;20:428.5,-1725.7,2;20:427.5,-1724.7,2",
    [string]$Do = "",             # TF_INPUT, ticks from the start
    [string]$Name = "bench",
    [hashtable]$Set = @{},        # extra environment variables for this run
    [switch]$RealTime,
    [int]$Monitor = -2,           # the window's screen (run.ps1 -Monitor; default: run.ps1's, the second screen)
    [switch]$Fullscreen,          # borderless full screen there (run.ps1 -Fullscreen)
    [switch]$Fps30,               # the console's 30 frames a second (else 60: TF_60FPS=force, as the states are made at 30)
    [switch]$Sound,
    [switch]$Stats,               # PS2X_GS_STATS / PS2X_VIF_STATS / PS2X_PACE_STATS lines in the log
    [switch]$Profile,             # TF_PROFILE over the middle of the run -> runs\profile_<name>.txt
    [string]$Lines = "",          # TF_PROFILE_LINES: functions to break down by source line
    [int]$Stacks = 0,             # TF_PROFILE_STACKS depth
    [int]$Seconds = 0,
    [string]$Exe = ""             # another build to measure (e.g. out\play\TransformersRecomp.exe), else out\build's
)
$Root = Split-Path $PSScriptRoot -Parent
Push-Location $Root
try {
    $file = "states\$State.ps2state"
    if (-not (Test-Path $file)) { throw "no save state $file" }
    $start = [int]((python tools\state_dump.py $file | Select-String "vblank tick\s+(\d+)").Matches[0].Groups[1].Value)
    $end = $start + $Ticks

    $shift = {
        param($list, $separator)
        @(foreach ($item in ($list -split $separator | Where-Object { $_ })) {
            $at, $rest = $item.Split(":", 2)
            "$($start + [int]$at):$rest"
        }) -join $separator
    }
    $names = @("TF_INPUT", "TF_GOTO", "TF_GOD", "TF_FPS", "TF_60FPS", "PS2X_TURBO_VSYNCS", "TF_QUIT_VSYNC", "PS2X_SHOT_VSYNCS",
               "PS2X_GS_STATS", "PS2X_VIF_STATS", "PS2X_PACE_STATS", "TF_PROFILE", "TF_PROFILE_AFTER", "TF_PROFILE_LINES",
               "TF_PROFILE_STACKS") + @($Set.Keys)
    foreach ($n in $names) { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }

    $env:TF_INPUT = & $shift $Do ","
    if ($Goto) { $env:TF_GOTO = & $shift $Goto ";" }
    $env:TF_GOD = "1"
    $env:TF_FPS = "1"
    $env:TF_60FPS = if ($Fps30) { "0" } else { "force" } # (a level from a state keeps its rate: all are made at 30)
    $env:PS2X_TURBO_VSYNCS = if ($RealTime) { "0" } else { "100000000" }
    $env:TF_QUIT_VSYNC = "$end"
    if ($Stats) { $env:PS2X_GS_STATS = "1"; $env:PS2X_VIF_STATS = "1"; $env:PS2X_PACE_STATS = "1" }
    if ($Profile) {
        # Sample the middle of the run: it starts after the state has loaded and settled.
        $env:TF_PROFILE = "8"
        $env:TF_PROFILE_AFTER = "3"
        if ($Lines) { $env:TF_PROFILE_LINES = $Lines }
        if ($Stacks -gt 0) { $env:TF_PROFILE_STACKS = "$Stacks" }
    }
    foreach ($key in $Set.Keys) { Set-Item "Env:$key" $Set[$key] }
    if ($Seconds -le 0) { $Seconds = 90 + [int]($Ticks / 10) }

    $arguments = @{ Seconds = $Seconds; Name = $Name; LoadState = $State }
    if ($Sound) { $arguments.Sound = $true }
    if ($Exe) { $arguments.Exe = $Exe }
    if ($Monitor -ne -2) { $arguments.Monitor = $Monitor }
    if ($Fullscreen) { $arguments.Fullscreen = $true }
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    .\run.ps1 @arguments *> "runs\$Name.console.txt"
    $watch.Stop()
    foreach ($n in $names) { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }

    $rates = @(Get-Content "runs\$Name.err" | Select-String "^\[tf\] fps ([\d.]+) vblank/s ([\d.]+)" | ForEach-Object {
        [pscustomobject]@{ Fps = [double]$_.Matches[0].Groups[1].Value; VBlanks = [double]$_.Matches[0].Groups[2].Value }
    })
    # The first report covers the state load; drop it.
    if ($rates.Count -gt 1) { $rates = $rates[1..($rates.Count - 1)] }
    if ($rates.Count -eq 0) { "no frame rate reported (see runs\$Name.err)"; return }
    $fps = $rates | Measure-Object Fps -Average -Minimum -Maximum
    $vb = $rates | Measure-Object VBlanks -Average -Minimum
    "{0}: {1:F1} fps average ({2:F1}-{3:F1}), {4:F1} VBlanks/s average, lowest {5:F1}; {6} reports, {7:F0} s" -f `
        $Name, $fps.Average, $fps.Minimum, $fps.Maximum, $vb.Average, $vb.Minimum, $rates.Count, $watch.Elapsed.TotalSeconds
    # The 1% low: the last report's, which covers the frames of the last 30 s (the whole run when it is shorter).
    $low = Get-Content "runs\$Name.err" | Select-String "^\[tf\] fps .* 1% low ([\d.]+)" | Select-Object -Last 1
    if ($low) { "{0}: {1:F1} fps 1% low" -f $Name, [double]$low.Matches[0].Groups[1].Value }
    if ($Profile -and (Test-Path "runs\profile.txt")) {
        Copy-Item "runs\profile.txt" "runs\profile_$Name.txt" -Force
        "profile: runs\profile_$Name.txt"
    }
    Get-Content "runs\$Name.err" | Where-Object { $_ -match "error|did not return|watchdog|fatal" -and $_ -notmatch "modload" } |
        Select-Object -First 5
}
finally { Pop-Location }
