# step_watchdog.ps1 (TODO 441) - a watchdog over a long setup / build step that runs one program: it says, in plain
# words, when the step is stuck and what to try, instead of the window sitting on the same line for an hour (a
# player's setup sat on "Scanning for SDK functions" for over an hour; it takes about 20 s).
# Dot-sourced by build.ps1 (its SDK scan and translation, RunLogged); usable by any script that runs a long program.
#
#   $w = Start-StepWatchdog -What "The SDK scan" -Exe <program's full path> -Log <its log file> -Expected 30 -Folder <game folder>
#   ... run the program ...
#   Stop-StepWatchdog $w
#
# Progress = the log file grows, or the program (every running process with that exact path) uses processor time.
# Without either for TF_WATCHDOG_SECONDS (default 300 s) it says what is stuck and what to try, and again every 10
# minutes while it stays so. A program still busy far past its usual time (20 times -Expected, at least 10 minutes;
# tests: TF_WATCHDOG_LONG_SECONDS) is said too: a loop. It only speaks, never stops anything: the player decides
# (closing the setup and running it again keeps the finished steps). It runs on a thread of its own, so it also speaks
# while the calling script is itself held - an antivirus scanning a program just built holds even its start.
# Its lines go to the console (the setup window's log; the window shows the last, short one) and to TF_STEP_LOG.
# -Hints: the "what usually holds it" lines (default: an antivirus, the disc image, the disk).
# Deps: Windows PowerShell 5.1 or newer.

function Start-StepWatchdog {
    param([Parameter(Mandatory)][string]$What, [Parameter(Mandatory)][string]$Exe, [string]$Log = "", [double]$Expected = 60,
          [string]$Folder = "", [string[]]$Hints = @(), [string]$LogShown = "")
    $quiet = 300
    if ($env:TF_WATCHDOG_SECONDS -match '^\d+$' -and [int]$env:TF_WATCHDOG_SECONDS -gt 0) { $quiet = [int]$env:TF_WATCHDOG_SECONDS }
    $long = 0
    if ($env:TF_WATCHDOG_LONG_SECONDS -match '^\d+$') { $long = [int]$env:TF_WATCHDOG_LONG_SECONDS }
    $leaf = Split-Path $Exe -Leaf
    if (-not $Hints.Count) {
        $where = if ($Folder) { "the game folder ($Folder)" } else { "the game folder" }
        $Hints = @(
            "  - an antivirus checking or holding $leaf, which was just built on this PC: wait a few more minutes, or add",
            "    $where to your antivirus' exclusions;",
            "  - the disc image: it must be a complete copy of Transformers USA (SLUS-20668) or the European disc (SLES-52388);",
            "  - a very slow or nearly full disk.")
    }
    $ps = [PowerShell]::Create()
    [void]$ps.AddScript({
        param($What, $Exe, $Log, $Expected, $Quiet, $Hints, $StepLog, $LongAfter, $LogShown)
        function Say([string]$t) {
            [Console]::Out.WriteLine($t); [Console]::Out.Flush()
            if ($StepLog -and $t) { try { [IO.File]::AppendAllText($StepLog, "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  build: $t`r`n") } catch {} }
        }
        $leaf = Split-Path $Exe -Leaf
        $name = [IO.Path]::GetFileNameWithoutExtension($Exe)
        $size = -1L; $cpu = -1.0; $since = [DateTime]::UtcNow; $next = $Quiet
        $began = [DateTime]::UtcNow
        $cpuMark = 0.0; $ranMark = 0.0
        $busyLimit = if ($LongAfter -gt 0) { $LongAfter } else { [Math]::Max(600, 20 * $Expected) }
        $usual = if ($Expected -ge 90) { "about $([int][Math]::Round($Expected / 60)) min" } elseif ($Expected -ge 45) { "about a minute" } else { "under a minute" }
        $logText = if ($LogShown) { " The step's log: $LogShown" } elseif ($Log) { " The step's log: $Log" } else { "" }
        while ($true) {
            Start-Sleep -Seconds ([Math]::Min(10, [Math]::Max(1, [int]($Quiet / 6))))
            $s = 0L; if ($Log) { try { $s = ([IO.FileInfo]::new($Log)).Length } catch {} }
            $c = 0.0; $running = $false
            foreach ($p in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
                try { if ($p.Path -and $p.Path -ieq $Exe) { $running = $true; $c += $p.TotalProcessorTime.TotalSeconds } } catch {}
            }
            $ran = ([DateTime]::UtcNow - $began).TotalSeconds
            if ($ran -ge $busyLimit) {
                $m = [int][Math]::Floor($ran / 60)
                # (TODO 441) busy or held: the processor time it used since the last time this was said (a player's
                # report could not tell the two apart: this line was said by the clock alone)
                $used = $c - $cpuMark; $span = $ran - $ranMark; $cpuMark = $c; $ranMark = $ran
                Say ""
                if ($running -and $used -ge 0.3 * $span) {
                    Say "$What has been running for $m min, far longer than usual ($usual), busy all along ($([int]$used) s of"
                    Say "processor time in the last $([int]($span / 60)) min). Check that the disc image is a complete copy of Transformers USA"
                    Say "(SLUS-20668) or the European disc (SLES-52388). You can close the setup and run it again: finished steps are kept.$logText"
                    Say "${What}: $m min and busy, far longer than usual. Check the disc; rerunning the setup is safe."
                } else {
                    Say "$What has been running for $m min, far longer than usual ($usual), mostly waiting ($([int]$used) s of"
                    Say "processor time in the last $([int]($span / 60)) min): something holds it. Most often an antivirus or Windows' Controlled"
                    Say "Folder Access (allow the game folder), or a slow external drive. You can close the setup and run it again: finished"
                    Say "steps are kept.$logText"
                    Say "${What}: $m min, mostly waiting: an antivirus or a slow drive may hold it. Rerunning is safe."
                }
                $busyLimit = $ran + 600
            }
            if ($s -ne $size -or $c -gt $cpu + 0.5) { $size = $s; $cpu = $c; $since = [DateTime]::UtcNow; $next = $Quiet; continue }
            $still = ([DateTime]::UtcNow - $since).TotalSeconds
            if ($still -lt $next) { continue }
            $min = [int][Math]::Floor($still / 60)
            $state = if ($running) { "$leaf is running but doing nothing" } else { "$leaf has not started yet" }
            Say ""
            Say "$What has made no progress for $min min ($state; it usually takes $usual). What usually holds it:"
            foreach ($h in $Hints) { Say $h }
            Say "  You can close the setup and run it again: finished steps are kept.$logText"
            Say "No progress for $min min: an antivirus may hold $leaf, or check the disc. Rerunning the setup is safe."
            $next = $still + 600
        }
    }).AddArgument($What).AddArgument($Exe).AddArgument($Log).AddArgument($Expected).AddArgument($quiet).AddArgument($Hints).AddArgument($env:TF_STEP_LOG).AddArgument($long).AddArgument($LogShown)
    return [pscustomobject]@{ Shell = $ps; Async = $ps.BeginInvoke() }
}

function Stop-StepWatchdog($Watch) {
    if (-not $Watch) { return }
    try { $Watch.Shell.Stop() } catch {}
    try { $Watch.Shell.Dispose() } catch {}
}
