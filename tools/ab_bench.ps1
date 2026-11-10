<#
.SYNOPSIS
    Compares the speed of two builds: runs tools\bench.ps1 on each in turn, so that whatever
    else the computer is doing weighs on both alike, and prints each run and the averages.

.EXAMPLE
    .\tools\ab_bench.ps1 -Base out\play\TransformersRecomp.exe
    .\tools\ab_bench.ps1 -Base out\play\TransformersRecomp.exe -Rounds 3 -State a11 -Ticks 600 -FrameRate 144
    .\tools\ab_bench.ps1 -Base "" -BaseSet @{ PS2X_VU1_JIT_FINITE = "0" }   # one build, a switch off and on
#>
param(
    [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Base,  # the build to compare against ("": out\build's)
    [string]$New = "",                              # else out\build's
    [hashtable]$BaseSet = @{},                      # extra environment variables for the base runs
    [hashtable]$NewSet = @{},                       # ... and for the new ones
    [int]$Rounds = 2,
    [string]$State = "amazon",
    [int]$Ticks = 1500,
    [string]$Goto = "",                             # where to walk (bench.ps1's -Goto); left out: standing at
                                                    # amazon, bench.ps1's dropship walk elsewhere
    [int]$FrameRate = 144,
    [string]$Name = "ab"
)
$Root = Split-Path $PSScriptRoot -Parent
Push-Location $Root
try {
    $results = @{ base = @(); new = @() }
    $lows = @{ base = @(); new = @() }
    foreach ($round in 1..$Rounds) {
        foreach ($side in "base", "new") {
            $exe = if ($side -eq "base") { $Base } else { $New }
            $run = "${Name}_${side}_$round"
            $set = @{ TF_FRAME_RATE = "$FrameRate" }
            foreach ($entry in $(if ($side -eq "base") { $BaseSet } else { $NewSet }).GetEnumerator()) { $set[$entry.Key] = $entry.Value }
            $arguments = @{ State = $State; Ticks = $Ticks; Name = $run; Set = $set }
            if ($PSBoundParameters.ContainsKey("Goto")) { $arguments.Goto = $Goto }
            elseif ($State -eq "amazon") { $arguments.Goto = "" }
            if ($exe) { $arguments.Exe = $exe }
            $output = & .\tools\bench.ps1 @arguments
            $line = ($output | Select-String "fps average" | Select-Object -First 1).Line
            $fps = if ($line -match ":\s*([\d.]+) fps average") { [double]$Matches[1] } else { [double]::NaN }
            $results[$side] += $fps
            $lowLine = ($output | Select-String "fps 1% low" | Select-Object -First 1).Line
            $low = if ($lowLine -match ":\s*([\d.]+) fps 1% low") { [double]$Matches[1] } else { [double]::NaN }
            $lows[$side] += $low
            "{0,-4} {1}: {2}; 1% low {3:N1}" -f $side, $round, $line, $low
        }
    }
    foreach ($side in "base", "new") {
        $values = $results[$side] | Where-Object { -not [double]::IsNaN($_) }
        $lowValues = $lows[$side] | Where-Object { -not [double]::IsNaN($_) }
        $lowText = if ($lowValues) { ", 1% low {0:N1}" -f (($lowValues | Measure-Object -Average).Average) } else { "" }
        if ($values) { "{0,-4} average {1:N1} fps{2}" -f $side, (($values | Measure-Object -Average).Average), $lowText }
    }
    $missing = Select-String -Path "runs\${Name}_*.err" -Pattern "missing-target" -ErrorAction SilentlyContinue
    if ($missing) { "missing-target reports: $(@($missing).Count)" }
}
finally { Pop-Location }
