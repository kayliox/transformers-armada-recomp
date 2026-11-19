<#
.SYNOPSIS
    Sets up the Transformers PC version from your own disc: one command. Run it again any time
    (finished steps are kept, so an interrupted setup carries on).

.DESCRIPTION
    1. Checks what the build needs: Git, Python 3.11+ (with Pillow and numpy), CMake, Visual Studio's
       C++ tools. -InstallPrerequisites installs the missing ones with winget (Visual Studio Build
       Tools is a few GB); without it the script says what to install and stops.
    2. Builds the game (build.ps1): checks the disc is Transformers USA (SLUS-20668) - or the European
       disc (SLES-52388), experimental - extracts it into game\, recompiles the game's code and builds the
       exe. The first build takes 15-25 minutes.
    3. Makes the HD textures (tools\make_hd_textures.ps1): every texture upscaled on your GPU, tens of
       minutes to a few hours. -SkipHdTextures leaves the upscale out and makes only the sharp fonts
       and button prompts (the game then shows its original textures). -HdQuality fast / balanced /
       best: how much the textures are enlarged (fast: for the Steam Deck and 1080p, about 0.6 GB and
       one pass for most; balanced, the default: 1440p, about 1.4 GB; best: 4K, about 2.3 GB). The
       game can be played meanwhile: the upscale waits while it runs (-HdWhilePlaying on: it carries on,
       gently) and an unfinished set is carried on by the game itself, in a small window
       (tools\make_hd_textures.ps1 -Resume).
    4. Remembers your disc path in .setup.json (git-ignored) for update.ps1.
    Before step 1 it says its plan (TODO 439): each program it would install, why, its download and disk size, and the
    space and time in all. -CleanBuild temp / all: afterwards, the build's files that playing never needs are deleted
    (tools\clean_build.ps1: "temp" costs nothing; "all" frees most of the build's space, and the next update then
    rebuilds the whole game once). Without it the setup only says how much there is to free.

    -HdGentle on (or -Gentle on; the setup window's "Go easy on my PC"): for a PC that restarts, freezes or
    overheats under a long full load. The build uses half the processor threads at a lower priority, and the HD
    textures are made in short bursts with rests. About twice as long. Kept in hd_textures\hd_options.json
    ("gentle"), so update.ps1 goes easy too. Each step is also written, with its time, to setup_steps.log
    (TF_STEP_LOG): straight to the disk, so it names the step even when the PC went off.

    Everything made from your disc stays on your PC: the folders it writes are git-ignored. Never
    commit or share them.

    -Root and -Hooks are for a repository that holds this folder as a subfolder and adds steps of
    its own (it passes its top folder and a script with Invoke-SetupHook / Invoke-UpdateHook).

.EXAMPLE
    .\setup.ps1 -Iso "D:\Games\Transformers (USA).iso" -InstallPrerequisites
.EXAMPLE
    .\setup.ps1          # again later: reuses the path it remembered
#>
param(
    [string]$Iso,
    [switch]$SkipHdTextures,
    [ValidateSet("", "fast", "balanced", "best")][string]$HdQuality = "",   # HD textures preset (PR #20)
    [ValidateSet("", "on", "off")][string]$HdWhilePlaying = "", # keep making them while the game runs
    [Alias("Gentle")][ValidateSet("", "on", "off")][string]$HdGentle = "", # (TODO 310) easy on the PC: the build on half the processor threads at a lower priority, the HD textures with rests; about twice as slow
    [switch]$InstallPrerequisites,
    [ValidateSet("", "temp", "all")][string]$CleanBuild = "",  # (TODO 439) the build's leftovers deleted at the end (tools\clean_build.ps1)
    [string]$Root,                 # the git repository's top (default: this folder)
    [string]$Hooks,                # a script with extra steps (Invoke-SetupHook), dot-sourced
    [hashtable]$HookArgs = @{}     # passed to the hooks as $ctx.Args
)

$ErrorActionPreference = "Stop"
$Port = $PSScriptRoot
if (-not $Root) { $Root = $Port }
$Root = (Resolve-Path $Root).Path
$StateFile = Join-Path $Root ".setup.json"

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan; Write-StepLog "setup: $msg" }
function Fail($msg) {
    Write-Host "`nsetup: $msg" -ForegroundColor Red; Write-StepLog "setup: stopped: $msg"
    # (build robustness) One text file to attach when asking for help (tools\setup_report.ps1: no game files, the user
    # name taken out). Never stops the stop itself.
    try {
        foreach ($f in "install_layout.ps1", "preflight.ps1", "setup_report.ps1") { $p = Join-Path $Port "tools\$f"; if (Test-Path -LiteralPath $p) { . $p } }
        if (Get-Command New-SetupReport -ErrorAction SilentlyContinue) {
            $report = New-SetupReport -Port $Port -What "setup" -Message $msg
            if ($report) { Write-Host "A report to attach when you ask for help (Discord or GitHub) was saved $(Get-ReportPlace $report)" }
        }
    } catch {}
    exit 1
}

# The shared helpers: real-Python detection (not Windows' Store shortcut), probes, git long paths.
. (Join-Path $Port "tools\setup_common.ps1")
# (TODO 310) Each step with its time, written straight to the disk (tools\step_log.ps1): the setup window sets
# TF_STEP_LOG beside its setup.log; run by hand, setup_steps.log in the install's logs\ (or here).
. (Join-Path $Port "tools\step_log.ps1")
if (-not $env:TF_STEP_LOG) {
    . (Join-Path $Port "tools\install_layout.ps1")
    $stepDir = Get-LayoutLogsDir $Port
    $env:TF_STEP_LOG = Join-Path $(if ($stepDir) { $stepDir } else { $Root }) "setup_steps.log"
    try { Set-Content -LiteralPath $env:TF_STEP_LOG -Value $null } catch {}
}
if ($Hooks) { . $Hooks }

# --- the remembered choices ---------------------------------------------------------------------
# (an empty or damaged .setup.json - a write cut off - is taken as missing: give the disc path again if it asks)
$state = $null
if (Test-Path $StateFile) { try { $state = Get-Content $StateFile -Raw | ConvertFrom-Json } catch {} }
if (-not $state) { $state = [pscustomobject]@{} }
if (-not $state.PSObject.Properties["Iso"]) { $state | Add-Member Iso "" }
if ($Iso) {
    if (-not (Test-Path -LiteralPath $Iso -PathType Leaf)) { Fail "no such file: $Iso" }
    $state.Iso = (Resolve-Path -LiteralPath $Iso).Path
}
# (written now: the build reads it - an install's version knows it was set up, transformers\version.cmake)
$state | ConvertTo-Json | Set-Content $StateFile -Encoding UTF8

# (TODO 279: the disc's files may be in the install's data\game - tools\install_layout.ps1)
. (Join-Path $Port "tools\install_layout.ps1")
$gameDir = Get-GameDataDir $Port
$haveGame = (Test-Path (Join-Path $gameDir "SLUS_206.68")) -or (Test-Path (Join-Path $gameDir "SLES_523.88"))
# (TODO 441) The game folder's executable checked (its checksum): a wrong one - left by an extraction that refused it
# only after copying it, or copied in by hand - was never checked again, and the SDK scan ran on it for an hour. With the
# disc image remembered it is checked and extracted again; without, the setup asks for it.
if ($haveGame) {
    $py0 = try { Find-Python } catch { $null }
    $elf0 = @("SLUS_206.68", "SLES_523.88") | ForEach-Object { Join-Path $gameDir $_ } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($py0 -and $elf0) {
        $elfText = "$(Invoke-Probe { & $py0 (Join-Path $Port "transformers\tools\extract_iso.py") --check-elf $elf0 } | Out-String)".Trim()
        if ($LASTEXITCODE -eq 2) {
            Write-StepLog "setup: wrong executable in the game folder: $elfText"
            if ($state.Iso -and (Test-Path -LiteralPath $state.Iso -PathType Leaf)) {
                Write-Host "$elfText`nIt is extracted again from your disc image ($($state.Iso)), which is checked first." -ForegroundColor Yellow
                $haveGame = $false
            } else {
                Fail "$elfText`nGive your disc image (a complete, unmodified copy) again, e.g.  .\setup.ps1 -Iso `"D:\Games\Transformers (USA).iso`""
            }
        }
    }
}
if (-not $haveGame -and -not $state.Iso) {
    Fail "first setup: give your disc image, e.g.  .\setup.ps1 -Iso `"D:\Games\Transformers (USA).iso`""
}
# The disc, checked before the long installs: Transformers (USA), SLUS-20668, or the European disc, SLES-52388
# (experimental). (The build checks the game's executable inside it; another size is only a warning.)
if (-not $haveGame) {
    if (-not (Test-Path -LiteralPath $state.Iso -PathType Leaf)) { Fail "the disc image is gone: $($state.Iso) (give it again with -Iso)" }
    $size = (Get-Item -LiteralPath $state.Iso).Length
    if ($size -eq (Get-PalDiscSize)) {
        Write-Host "The European disc (SLES-52388, PAL): support for it is experimental - it is played less than the USA disc, so" -ForegroundColor Yellow
        Write-Host "something may not work yet. Please report what you find (docs\TROUBLESHOOTING.md says how)." -ForegroundColor Yellow
    } elseif ($size -ne (Get-UsaDiscSize)) {
        Write-Host "warning: $($state.Iso) is not the size of the USA release (SLUS-20668, $(Get-UsaDiscSize) bytes) or of the European one (SLES-52388); the build checks the disc's executable." -ForegroundColor Yellow
    }
}

# (build robustness) The checks before the long steps: disk space, the folder, the disc, Windows (tools\preflight.ps1).
# A problem that would stop the install later stops it now, in plain words; the others are said and the setup goes on.
if (Test-Path -LiteralPath (Join-Path $Port "tools\preflight.ps1")) {
    . (Join-Path $Port "tools\preflight.ps1")
    Step "Checking this PC first (free space, the folder, the disc)"
    $pfPython = try { Find-Python } catch { $null }
    $pf = @(Invoke-Preflight -Iso $(if (-not $haveGame) { $state.Iso } else { "" }) -Port $Port -Python $pfPython `
        -HdQuality $(if ($SkipHdTextures) { "none" } else { $HdQuality }))
    if (-not (Write-PreflightReport $pf)) { Fail (($pf | Where-Object { $_.Level -eq "stop" } | ForEach-Object { $_.Text }) -join "`n") }
}

# --- 1. what the build needs --------------------------------------------------------------------
Step "Checking what the build needs"
$missing = @(Get-MissingPrerequisites)
# (TODO 439) The plan, before anything is installed or built: what, why, how big, how long.
$planQuality = $HdQuality
if (-not $planQuality) {
    try { $planQuality = "$((Get-Content (Join-Path $Port "hd_textures\hd_options.json") -Raw | ConvertFrom-Json).quality)" } catch {}
}
$planHd = if ($SkipHdTextures) { @(0, 0) } else { Get-HdPresetCost $planQuality }
Write-Host ""
foreach ($line in (Get-SetupPlanText -Missing $missing -HdGB $planHd[0] -HdMinutes $planHd[1])) { Write-Host $line }
Write-StepLog ("setup: plan: missing " + $(if ($missing.Count) { $missing -join ", " } else { "nothing" }))
if ($missing.Count -gt 0) {
    $how = Get-PrerequisiteInstall
    if (-not $InstallPrerequisites) {
        $manual = Get-PrerequisiteManualSteps
        $lines = $missing | ForEach-Object { "  - $($how[$_][0]):  $($how[$_][1])`n    or by hand: $($manual[$_])" }
        Fail ("missing:`n" + ($lines -join "`n") + "`nInstall them (or run setup.ps1 -InstallPrerequisites), open a NEW PowerShell, run setup.ps1 again.")
    }
    # Each one installed (winget, or Visual Studio's own installer for an install without its C++ tools) and checked
    # again at once; what is still missing comes back with the reason and the exact steps to take by hand.
    Write-PrerequisiteEvidence "missing: $($missing -join ', ')"
    $result = Install-MissingPrerequisites $missing
    if ($result.Still.Count -gt 0) {
        Write-PrerequisiteEvidence "after the installs"
        $names = ($result.Still | ForEach-Object { $how[$_][0] }) -join ", "
        $text = "these tools are still missing: $names`n" + ($result.Report -join "`n")
        if ($result.Restart) { $text += "`nRestart the PC, then run the setup again." }
        elseif ($result.Retry) {
            # ("prerequisites installed" is the phrase the setup program (tfa-setup.exe) looks for to run this script once
            # more with a fresh PATH - only when the installers said they succeeded)
            $text = "prerequisites installed, but not found yet: $($result.Still -join ', ')`n" + ($result.Report -join "`n") +
                "`nIf the setup stops here again: restart the PC and run the setup again."
        } else {
            $text += "`nThen run the setup again: finished steps are kept."
        }
        if ($result.Still -contains "vs") { $text += "`n(Visual Studio's installer keeps its logs as dd_*.log files in $env:TEMP.)" }
        Fail $text
    }
}
$Python = Find-Python
Use-Python $Python
Write-Host "Python: $Python"
foreach ($pkg in @(@("PIL", "pillow", "Pillow (Python imaging)"), @("numpy", "numpy", "numpy (Python): the sharp text and button symbols"))) {
    Invoke-Probe { & $Python -c "import $($pkg[0])" } | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Step "Installing $($pkg[2])"
        if ((Invoke-Native { & $Python -m pip install --user $pkg[1] }) -ne 0) { Fail "pip install $($pkg[1]) failed" }
    }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Step "Installing CMake (pip)"
    if ((Invoke-Native { & $Python -m pip install --user cmake }) -ne 0) { Fail "pip install cmake failed" }
    Use-Python $Python   # (its user Scripts folder, where cmake.exe went, now exists)
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Fail "cmake installed but not found on PATH" }
}
# (TODO 407) A CMake from before the PC's Visual Studio (Visual Studio 2026 alone, and an older pip CMake): updated, as
# the build could not use that Visual Studio.
$unknownVs = @(Get-VsUnknownToCmake)
if ($unknownVs.Count) {
    Step "Updating CMake (pip): this one does not know $($unknownVs -join ', ')"
    if ((Invoke-Native { & $Python -m pip install --user --upgrade cmake }) -ne 0) { Fail "pip install --upgrade cmake failed" }
    Use-Python $Python
    $unknownVs = @(Get-VsUnknownToCmake)
    if ($unknownVs.Count) {
        Fail ("CMake ($("$(Invoke-Probe { & cmake --version } | Select-Object -First 1)".Trim()) still does not know $($unknownVs -join ', '). " +
            "If another CMake comes first on PATH, remove it or update it; or install Visual Studio 2022's Build Tools (run the setup with -InstallPrerequisites).")
    }
}

# The check that refuses commits with game data (tools\check_no_game_data.py), for anyone who commits.
if (Test-Path (Join-Path $Root ".githooks")) { Invoke-Native { & git -C $Root config core.hooksPath .githooks } | Out-Null }

# --- 2. the game ----------------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $Port "PS2Recomp\CMakeLists.txt"))) {
    Step "Fetching the PS2Recomp submodule"
    if ((Invoke-Native { & git -C $Root submodule update --init --recursive }) -ne 0) { Fail "git submodule update failed" }
}
# (TODO 219) The choices, kept in hd_textures\hd_options.json: the upscale reads them (the "while playing" one at
# every batch), and the game's own window that carries on with an unfinished set. (TODO 310) Written before the build:
# "gentle" is the build's too, also when the HD textures are left out.
$hdDir = Join-Path $Port "hd_textures"
$optFile = Join-Path $hdDir "hd_options.json"
$opt = [ordered]@{ quality = "balanced"; while_playing = $false; gentle = $false }
if (Test-Path $optFile) {
    try { $o = Get-Content $optFile -Raw | ConvertFrom-Json; $opt.quality = "$($o.quality)"; $opt.while_playing = [bool]$o.while_playing; $opt.gentle = [bool]$o.gentle } catch {}
}
if ($HdQuality -or $HdWhilePlaying -or $HdGentle) {
    New-Item -ItemType Directory -Force $hdDir | Out-Null
    if ($HdQuality) { $opt.quality = $HdQuality }
    if ($HdWhilePlaying) { $opt.while_playing = ($HdWhilePlaying -eq "on") }
    if ($HdGentle) { $opt.gentle = ($HdGentle -eq "on") }
    [System.IO.File]::WriteAllText($optFile, ($opt | ConvertTo-Json))
}

Step "Building the game (build.ps1)"
$buildArgs = @{}
if (-not $haveGame) { $buildArgs.Iso = $state.Iso }
# (TODO 310) "Go easy on my PC": half the processor threads at a lower priority (a build.ps1 from before it: as it is)
if ($opt.gentle -and (Select-String -LiteralPath (Join-Path $Port "build.ps1") -SimpleMatch '[switch]$Gentle' -Quiet)) { $buildArgs.Gentle = $true }
try {
    & (Join-Path $Port "build.ps1") @buildArgs
    if (-not $?) { Fail "the build failed (see above)" }
} catch {
    # (a program built on this PC taken by an antivirus: build.ps1 says so in words a player can act on)
    $m = [regex]::Match("$_", "Your antivirus blocked (\S+?\.exe)\.")
    if ($m.Success) { Write-AntivirusEvidence $m.Groups[1].Value $Port; Fail "$_" }
    if ("$_".StartsWith("Windows' Smart App Control blocked")) { Fail "$_" }  # (TODO 371: build.ps1's plain words)
    throw
}

# --- 3. the HD textures ----------------------------------------------------------------------------
if ($SkipHdTextures) {
    Step "Making the button prompts and fonts (no GPU upscale)"
    Invoke-OptionalFonts -Python $Python -Port $Port
} else {
    # (the choices were written before the build, above)
    Step "Making the HD textures (tools\make_hd_textures.ps1)"
    & (Join-Path $Port "tools\make_hd_textures.ps1") -Python $Python
    $hdOk = $?; $hdCode = $LASTEXITCODE
    # (TODO 371) 7: Windows' Smart App Control refused a file the HD textures need - said above; the setup goes on, the
    # game draws its own textures.
    if ($hdCode -eq 7) { Write-StepLog "setup: HD textures skipped (Smart App Control)" }
    elseif (-not $hdOk) { Fail "making the HD textures failed (see above); -SkipHdTextures plays without them" }
}

# --- the steps of a repository that holds this folder (its hooks) ---------------------------------
if (Get-Command Invoke-SetupHook -ErrorAction SilentlyContinue) {
    $ctx = New-HookContext -Root $Root -Port $Port -Python $Python -State $state -HookArgs $HookArgs
    try { Invoke-SetupHook $ctx } catch { Fail $_.Exception.Message }
}

# (TODO 439) The build's leftovers: deleted when asked (-CleanBuild), else only said with how much they are.
$cleanPs1 = Join-Path $Port "tools\clean_build.ps1"
if (Test-Path -LiteralPath $cleanPs1) {
    if ($CleanBuild) {
        Step "Deleting the build's $(if ($CleanBuild -eq 'all') { 'temporary and in-between' } else { 'temporary' }) files (tools\clean_build.ps1 -Level $CleanBuild)"
        & $cleanPs1 -Level $CleanBuild
        if ($LASTEXITCODE -ne 0) { Write-Host "warning: some of them could not be deleted (see above); the game plays as before" -ForegroundColor Yellow }
    } else {
        $sizes = @{}
        foreach ($l in @(& $cleanPs1 -Report)) { $p = "$l".Split('|'); if ($p.Count -ge 2) { $sizes[$p[0]] = [double]$p[1] } }
        $free = ($sizes.temp + $sizes.build) / 1GB
        if ($free -ge 0.1) {
            Write-Host ("`nThe build left {0:N1} GB that playing never needs. To free it: .\tools\clean_build.ps1 -Level all" -f $free)
            Write-Host "(the next update then rebuilds the whole game once; -Level temp frees $("{0:N1}" -f ($sizes.temp / 1GB)) GB of it at no cost)."
        }
    }
}

$state | Add-Member -Force SetUpAt (& git -C $Root rev-parse HEAD)
Add-DesktopShortcutOnce -Repo $Root -Port $Port
$state | ConvertTo-Json | Set-Content $StateFile -Encoding UTF8
Write-StepLog "setup: ready"
Write-Host "`nReady. Play with Play.bat in $Root. Settings: F1 in the game." -ForegroundColor Green
