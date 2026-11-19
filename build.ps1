<#
.SYNOPSIS
    Builds the Transformers (PS2) recompilation from your own disc image.

.DESCRIPTION
    1. Extracts the ISO into game\ (skipped if already extracted).
    2. Builds PS2Recomp's recompiler and analyzer.
    3. Recovers function boundaries and generates the recompiler config (work\).
    4. Translates the game's MIPS code to C++ (work\generated\).
    5. Builds TransformersRecomp.exe.
       A tree with the private overlay (overlay\) builds it in, with its own steps (overlay\build_hooks.ps1);
       -Public leaves it out.

    Steps 3-4 run only when something they depend on changed: the recompiler and analyzer
    programs (after step 2 rebuilt them - a PS2Recomp patch that only changes the runtime leaves
    them as they were), the Python tools that run them, transformers\config\ and the game's ELF.
    work\translation-inputs.txt records those inputs for the last translation (TODO 115).
    Generated files are written only when their contents change, and the PS2Recomp patch series
    keeps the times of files it leaves as they were, so the compile redoes only what changed.
    Each step's time is recorded in out\build-times.json and used for the time estimates shown.

.EXAMPLE
    .\build.ps1 -Iso "Transformers (USA).iso"
.EXAMPLE
    .\build.ps1 -SkipRecompile      # only rebuild the executable after runtime changes
#>
param(
    [string]$Iso,
    [string]$Config = "RelWithDebInfo",
    [string]$Generator,
    [switch]$SkipRecompile,
    [switch]$NoPatches,     # leave PS2Recomp as it is (when editing it directly)
    [switch]$Public,        # the public build (no overlay\: no mods) into out\build-public, from a tree that has the overlay
    [switch]$Vulkan,        # the Vulkan backend too (-DTF_VULKAN=ON, and -Sdl3: it presents SDL3's window; in development, off by default). The build folder keeps it.
    [switch]$Sdl3,          # SDL3 as the window and its input, chosen at run time with PS2X_PLATFORM=sdl3 (-DPS2X_SDL3=ON). Kept too.
    [switch]$FsrSdk,        # (TODO 299) kept for old command lines: AMD's FidelityFX SDK DLLs are fetched next to the game by default now
    [switch]$NoFsrSdk,      # (TODO 299) AMD's FidelityFX SDK DLLs (FSR 3.1 / FSR 4) not fetched (-DTF_FSR_SDK=OFF): no FSR 3 / 4 in the game
    [switch]$Gentle,        # (TODO 310) easy on the PC: half the processor threads, at a lower priority (the setup's "Go easy on my PC"); about half as fast
    [string]$BuildDir,      # another build folder under out\ for an A/B build beside the played one, e.g. build-ab
    [string]$CMakeDefine,   # CMake cache settings for it, ";"-separated, e.g. "TF_LTCG=ON" (the folder keeps them)
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$OverlayArgs  # the overlay's own (overlay\build_hooks.ps1)
)

$ErrorActionPreference = "Stop"
# (git on a drive that records no file owners - exFAT, FAT32 - refuses the repository as "dubious ownership":
# this script's own git commands treat it as safe; setup_common.ps1 sets the same when the setup runs this)
if (-not $env:GIT_CONFIG_COUNT) {
    $env:GIT_CONFIG_COUNT = "2"; $env:GIT_CONFIG_KEY_0 = "core.longpaths"; $env:GIT_CONFIG_VALUE_0 = "true"
    $env:GIT_CONFIG_KEY_1 = "safe.directory"; $env:GIT_CONFIG_VALUE_1 = "*"
}
$Root = $PSScriptRoot
# (build robustness) Settings in this PC's environment that would change what CMake, the compiler or Python do: a
# Visual Studio developer prompt's INCLUDE / LIB, extra compiler options in CL / _CL_, another toolchain's CC / CXX /
# CFLAGS, a CMake toolchain file, generator or project include, HP PCs' "Platform", a PYTHONPATH / PYTHONHOME of
# another Python. Left out of this build and of every program it starts (never changed for the PC), and said once.
$LeftOut = @()
foreach ($v in "CL", "_CL_", "LINK", "_LINK_", "CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "INCLUDE", "LIB", "LIBPATH",
               "CMAKE_TOOLCHAIN_FILE", "CMAKE_GENERATOR", "CMAKE_GENERATOR_PLATFORM", "CMAKE_GENERATOR_TOOLSET", "CMAKE_GENERATOR_INSTANCE",
               "CMAKE_PREFIX_PATH", "CMAKE_PROJECT_INCLUDE", "CMAKE_PROJECT_INCLUDE_BEFORE", "CMAKE_PROJECT_TOP_LEVEL_INCLUDES",
               "CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER", "Platform", "Configuration", "PYTHONPATH", "PYTHONHOME") {
    if ([Environment]::GetEnvironmentVariable($v)) { $LeftOut += $v; Remove-Item -LiteralPath "Env:$v" }
}
if ($LeftOut) { Write-Host "  (settings of this PC's environment left out of the build: $($LeftOut -join ', '))" }
# (TODO 279) The disc's files: game\ here, or the install's data\game once its folders were moved (tools\install_layout.ps1)
. (Join-Path $Root "tools\install_layout.ps1")
# (TODO 310) Each step to TF_STEP_LOG with its time, when the setup or the update set it (tools\step_log.ps1)
. (Join-Path $Root "tools\step_log.ps1")
# (TODO 441) The stall watchdog over the long one-program steps (tools\step_watchdog.ps1: Start-StepWatchdog)
. (Join-Path $Root "tools\step_watchdog.ps1")
# (build robustness) The libraries CMake would clone with git, fetched once as pinned, hash-checked archives into out\deps
# (tools\deps.ps1, tools\fetch_deps.py): no network and no git in the configure, no git history kept (about 1 GB)
. (Join-Path $Root "tools\deps.ps1")
# (TODO 310) -Gentle: for a PC that resets or overheats under a long full load. The build and every program it starts
# (Windows passes the lower priority on to them) run below normal, and MSBuild and the compiler use half the processor
# threads - "/m" and the compiler's own count (MultiToolTask) both held to it. Without it the build is as before.
# (The compiler's count goes in as environment variables, which MSBuild reads as properties: given as /p: they are
# global properties, and the first test saw a gentle build compile every file again - 787 instead of the 71 touched.)
$BuildParallel = @("/m")
if ($Gentle) {
    $GentleJobs = [Math]::Max(1, [int][Math]::Floor([Environment]::ProcessorCount / 2))
    try { (Get-Process -Id $PID).PriorityClass = "BelowNormal" } catch {}
    $BuildParallel = @("/m:$GentleJobs")
    $env:MultiProcMaxCount = "$GentleJobs"
    $env:EnforceProcessCountAcrossBuilds = "true"
    Write-Host "Going easy on the PC: $GentleJobs of $([Environment]::ProcessorCount) processor threads, at a lower priority" -ForegroundColor Cyan
}
$Game = Get-GameDataDir $Root
$Work = Join-Path $Root "work"
$Build = Join-Path $Root $(if ($Public) { "out\build-public" } else { "out\build" })
if ($BuildDir) { $Build = Join-Path $Root "out\$BuildDir" } # (-BuildDir: an A/B build beside the played one)
$ToolsBuild = Join-Path $Root "out\tools"
$Tools = Join-Path $Root "transformers\tools"
# (TODO 444) The Python the build runs, by its exact path: the setup's choice (TF_PYTHON, set by setup.ps1 / update.ps1's
# Use-Python), else the first python on PATH outside a WindowsApps folder, else "python" as before (a build by hand).
# A bare "python" found the Microsoft Store's Python on a player's PC (its folder put first on PATH), which Windows does
# not let other programs start directly: "Access denied" at the build's first step.
function Resolve-BuildPython([string]$Chosen) {
    if ($Chosen -and "$Chosen" -notlike '*\WindowsApps\*' -and (Test-Path -LiteralPath $Chosen -PathType Leaf)) { return $Chosen }
    $py = @(Get-Command python -All -CommandType Application -ErrorAction SilentlyContinue | ForEach-Object Source |
            Where-Object { $_ -and $_ -notlike '*\WindowsApps\*' }) | Select-Object -First 1
    if ($py) { return $py }
    $first = "$((Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source)"
    if ($first -like "$env:ProgramFiles\WindowsApps\*") {
        throw ("The only Python found is the Microsoft Store's ($first), which Windows does not let other programs start directly. " +
            "Install Python 3.12 from https://www.python.org/downloads/windows/ (tick 'Add python.exe to PATH'), or run the setup: it installs it.")
    }
    return "python"
}
$Py = Resolve-BuildPython $env:TF_PYTHON
$env:TF_PYTHON = $Py
# (cpucheck) The processor first: the game needs SSE4.1 (2008 or newer) - said now, not after the long build
# (transformers\tools\cpu_check.py: numpy's own cpuid table; when it cannot tell, the game's own check at its start stands).
try { & $Py (Join-Path $Tools "cpu_check.py"); if ($LASTEXITCODE -eq 3) { exit 3 } } catch { }

# --- The release being built: USA (SLUS-20668, transformers\config\) or another one with its own
# config (transformers\config\versions\<serial>\). TF_GAME_VERSION when set; else the release whose
# executable is in game\; else the disc image's (extract_iso.py --detect). The Python tools read
# TF_GAME_VERSION for the config they use.
$Versions = [ordered]@{}
$Versions["SLUS-20668"] = (Get-Content (Join-Path $Root "transformers\config\game.json") -Raw | ConvertFrom-Json).elf_name
foreach ($f in Get-ChildItem (Join-Path $Root "transformers\config\versions") -Filter game.json -Recurse -ErrorAction SilentlyContinue) {
    $Versions[$f.Directory.Name] = (Get-Content $f.FullName -Raw | ConvertFrom-Json).elf_name
}
$GameVersion = $env:TF_GAME_VERSION
if (-not $GameVersion) {
    $GameVersion = @($Versions.Keys | Where-Object { Test-Path (Join-Path $Game $Versions[$_]) }) | Select-Object -First 1
}
if (-not $GameVersion) {
    $disc = if ($Iso) { $Iso } else { (Get-ChildItem $Root -Filter *.iso -File | Select-Object -First 1).FullName }
    if ($disc) { $GameVersion = (& $Py (Join-Path $Tools "extract_iso.py") --detect $disc | Out-String).Trim() }
}
if (-not $GameVersion) { $GameVersion = "SLUS-20668" }
if (-not $Versions.Contains($GameVersion)) { throw "No config for the release $GameVersion (transformers\config\versions\$GameVersion)." }
$env:TF_GAME_VERSION = $GameVersion
$ElfName = $Versions[$GameVersion]
# Another release runs the USA hooks at its own addresses (transformers\src\game_version.h, TF_GA): every code address
# the C++ uses must be written with TF_GA, or that build would hook the wrong place.
if ($GameVersion -ne "SLUS-20668") {
    & $Py (Join-Path $Tools "check_pal_addresses.py")
    if ($LASTEXITCODE -ne 0) { throw "Code addresses not translated for $GameVersion (see above)." }
    # (docs\PAL_ADDRESSES.md) The private tree's mods (overlay\) use USA addresses that are not wrapped: another release
    # is built without them, as -Public builds.
    if (-not $Public -and (Test-Path -LiteralPath (Join-Path $Root "overlay"))) {
        Write-Host "The mods (overlay\) are USA only: $GameVersion is built without them." -ForegroundColor Yellow
        $Public = $true
        $Build = Join-Path $Root "out\build-public"
    }
} else {
    # (docs\PAL_ADDRESSES.md) The USA build checks the same, as a warning: an address added without TF_GA shows at once.
    & $Py (Join-Path $Tools "check_pal_addresses.py") | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "warning: code addresses without TF_GA (above): the European build would stop on them. docs\PAL_ADDRESSES.md" -ForegroundColor Yellow }
}

# --- Timing: each step's time, a summary at the end, the record used for the next estimates -------
$Clock = [Diagnostics.Stopwatch]::StartNew()
$script:stage = $null
$script:stageStart = 0.0
$Spent = [ordered]@{}       # stage -> seconds, this build
# (a gentle build keeps its own record: its slower times would make a normal build's estimates too long)
$TimesFile = Join-Path $Root $(if ($Gentle) { "out\build-times-gentle.json" } else { "out\build-times.json" })
$History = @{}
try { if (Test-Path $TimesFile) { (Get-Content $TimesFile -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $History[$_.Name] = $_.Value } } } catch {}
function Close-Stage {
    if ($script:stage) {
        $s = $Clock.Elapsed.TotalSeconds - $script:stageStart
        if ($Spent.Contains($script:stage)) { $Spent[$script:stage] += $s } else { $Spent[$script:stage] = $s }
    }
    $script:stage = $null
}
function Step($msg, [string]$stage) {
    Close-Stage
    if ($stage) { $script:stage = $stage; $script:stageStart = $Clock.Elapsed.TotalSeconds }
    Write-Host "`n==> $msg" -ForegroundColor Cyan
    Write-StepLog "build: $msg"
}
function Format-Duration([double]$seconds) {
    $s = [int][Math]::Round($seconds)
    if ($s -lt 60) { return "$s s" }
    if ($s -lt 3600) { return "{0} min {1} s" -f [int][Math]::Floor($s / 60), ($s % 60) }
    return "{0} h {1} min" -f [int][Math]::Floor($s / 3600), [int][Math]::Floor(($s % 3600) / 60)
}
# "about 3 min" / "under a minute": the rough figure shown to the player
function Format-Estimate([double]$seconds) {
    if ($seconds -lt 45) { return "under a minute" }
    if ($seconds -lt 90) { return "about a minute" }
    return "about {0} min" -f [int][Math]::Round($seconds / 60)
}
# (a stage's time from earlier builds on this PC: "(usually about 2 min here)"; TODO 441: on a first build, the time it
# takes on most PCs, so a step that sits far longer than that is seen to be stuck - a player's SDK scan sat for an hour)
$FirstTimes = @{ scan = 30; functions = 60; translate = 60; sync = 20 }   # (seconds: the Testbench's 26 / 56 / 30 / 14, rounded up)
function Get-StageSeconds([string]$stage) {
    if ($History.ContainsKey($stage)) { return [double]$History[$stage] }
    if ($FirstTimes.ContainsKey($stage)) { return [double]$FirstTimes[$stage] }
    return 0.0
}
function Usual([string]$stage) {
    if ($History.ContainsKey($stage)) {
        if ([double]$History[$stage] -ge 20) { return " (usually $(Format-Estimate ([double]$History[$stage])) here)" }
        return ""
    }
    if ($FirstTimes.ContainsKey($stage)) { return " (usually $(Format-Estimate $FirstTimes[$stage]))" }
    return ""
}

# A program this build made on the player's PC and then runs (the recompiler, the analyzer, the game's own shader
# bake): an antivirus may quarantine it as soon as it is written - a player's "hypersensitive" one flagged
# ps2_analyzer.exe at every try. That is said plainly, with what to do; nothing is turned off or excluded for him.
$BuiltHere = @{ "ps2_analyzer.exe" = "PS2Recomp\ps2xAnalyzer"; "ps2_recomp.exe" = "PS2Recomp\ps2xRecomp"; "TransformersRecomp.exe" = "transformers" }
function Get-AntivirusBlockMessage([string]$Exe) {
    $leaf = Split-Path $Exe -Leaf
    "Your antivirus blocked $leaf. It is built on your PC from the open-source code in $(Join-Path $Root $BuiltHere[$leaf]). " +
    "Don't turn the antivirus off: add the game folder ($Root) as an exclusion, or restore the file and allow it, " +
    "then run the setup again (finished steps are kept)."
}
# Whether a run of a program built here was stopped by an antivirus: the file gone (quarantined right after the
# build), Windows' "contains a virus" (225 / 0xC0000906 STATUS_VIRUS_INFECTED) or access denied to it.
function Test-AntivirusBlock([string]$Exe, $Code, [string]$Text) {
    if (-not $BuiltHere.ContainsKey((Split-Path $Exe -Leaf))) { return $false }
    if (-not (Test-Path -LiteralPath $Exe)) { return $true }
    if ($Code -in 225, -1073739514, 3221227782) { return $true }
    return ($Text -match "contains a virus|potentially unwanted software|Access is denied|0xC0000906")
}
# (TODO 371) Windows 11's Smart App Control refusing a program or a library it loads: it lets run only what is signed or
# what Microsoft's cloud already knows, and the programs built here are new on every build. PowerShell says "An
# Application Control policy has blocked this file"; a program whose library was refused ends with 0xC0E90002; Win32
# 4551. Said with the file's name and where the switch is - any program, not only the ones built here.
function Test-SmartAppBlock($Code, [string]$Text) {
    if ($Code -in -1058471934, 3236495362, 4551) { return $true }
    return ($Text -match "Application Control policy has blocked|0xc0e90002|ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION")
}
function Get-SmartAppBlockMessage([string]$What) {
    "Windows' Smart App Control blocked $What. It lets only programs Microsoft already knows run, and this game's programs " +
    "are built on your PC, so they are new to it. The file is not damaged. To see every file it blocked: Event Viewer > " +
    "Applications and Services Logs > Microsoft > Windows > CodeIntegrity > Operational, event 3077. To play, turn it off: " +
    "Windows Security > App & browser control > Smart App Control settings > Off (on some Windows 11 versions it cannot be " +
    "turned back on without resetting Windows), then run the setup again (finished steps are kept)."
}
function Assert-NotBlocked([string]$Exe, $Code = 0, [string]$Text = "") {
    if (Test-SmartAppBlock $Code $Text) { throw (Get-SmartAppBlockMessage (Split-Path $Exe -Leaf)) }
    if (Test-AntivirusBlock $Exe $Code $Text) { throw (Get-AntivirusBlockMessage $Exe) }
}

function Run {
    param([string]$Exe, [string[]]$Arguments)
    # Windows PowerShell turns any stderr line from a native tool into an
    # error record; judge success by the exit code only.
    $ErrorActionPreference = "Continue"
    if ($BuiltHere.ContainsKey((Split-Path $Exe -Leaf))) { Assert-NotBlocked $Exe }
    try { & $Exe @Arguments 2>&1 | ForEach-Object { "$_" } }
    catch { Assert-NotBlocked $Exe -1 "$_"; throw }
    if ($LASTEXITCODE -ne 0) { Assert-NotBlocked $Exe $LASTEXITCODE; throw "$Exe failed with exit code $LASTEXITCODE" }
}
function RunLogged {
    param([string]$Exe, [string[]]$Arguments, [string]$Log, [string]$What = "", [int]$TimeLimit = 0)
    # The output goes into the log file directly, not through PowerShell: the analyzer writes ~9 MB
    # of lines, which PowerShell 5 passes one by one - with update.ps1's transcript running, that
    # alone took ~80 s instead of ~8 (TODO 115). Its error output is added at the end.
    # (TODO 441) While the program runs its output goes to a file in Windows' temp folder, and is moved to $Log when it
    # ends: the analyzer writes each of its ~150,000 lines to the disk at once (it flushes every line), which on an
    # external or slow drive, or in a folder an antivirus checks write by write, can take an hour instead of 20 s.
    $quoted = @($Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } })
    $runLog = $Log
    try {
        $tmpDir = [IO.Path]::GetTempPath()
        if ($tmpDir -and (Test-Path -LiteralPath $tmpDir)) { $runLog = Join-Path $tmpDir ("tfa_{0}_{1}" -f $PID, (Split-Path $Log -Leaf)) }
    } catch { $runLog = $Log }
    $err = "$runLog.stderr"
    Assert-NotBlocked $Exe
    # (TODO 441) the watchdog: says so when the program sits still for minutes (Start-StepWatchdog)
    $watch = $null
    if ($What) { try { $watch = Start-StepWatchdog -What $What -Exe $Exe -Log $runLog -LogShown $Log -Expected (Get-StageSeconds $script:stage) -Folder $Root } catch {} }
    $timedOut = $false; $cpu = 0.0; $ran = 0.0
    $started = [Diagnostics.Stopwatch]::StartNew()
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList $quoted -WorkingDirectory (Get-Location).ProviderPath -NoNewWindow -PassThru `
            -RedirectStandardOutput $runLog -RedirectStandardError $err
        $null = $p.Handle   # (kept open: the exit code can be read after it ends)
        # (TODO 441) -TimeLimit: a program that runs far past any normal time is stopped (only this process, which this
        # script started) and said in plain words, instead of the setup waiting forever
        if (-not $p.WaitForExit($(if ($TimeLimit -gt 0) { $TimeLimit * 1000 } else { -1 }))) {
            $timedOut = $true
            $ran = $started.Elapsed.TotalSeconds
            try { $cpu = $p.TotalProcessorTime.TotalSeconds } catch {}
            try { $p.Kill() } catch {}
            [void]$p.WaitForExit(15000)
        }
        $p.WaitForExit()
    } catch { Assert-NotBlocked $Exe -1 "$($_.Exception.Message)"; throw }
    finally {
        Stop-StepWatchdog $watch
        # (the output moved to the step's own log, its error output added at the end; on any failure the temp copy stays
        # where it is and the log says so)
        if ($runLog -ne $Log -and (Test-Path -LiteralPath $runLog)) {
            try { Move-Item -LiteralPath $runLog -Destination $Log -Force -ErrorAction Stop }
            catch { try { Copy-Item -LiteralPath $runLog -Destination $Log -Force -ErrorAction Stop; Remove-Item -LiteralPath $runLog -ErrorAction SilentlyContinue } catch { Write-Host "  (the step's output stays in $runLog)" } }
        }
        if (Test-Path -LiteralPath $err) {
            try {
                $bytes = [IO.File]::ReadAllBytes($err)
                if ($bytes.Length) { $f = [IO.File]::Open($Log, [IO.FileMode]::Append); try { $f.Write($bytes, 0, $bytes.Length) } finally { $f.Dispose() } }
            } catch {}
            Remove-Item -LiteralPath $err -ErrorAction SilentlyContinue
        }
    }
    if ($timedOut) {
        $leaf = Split-Path $Exe -Leaf
        $min = [int][Math]::Round($ran / 60)
        $head = "$What was stopped after $min minutes ($leaf; it usually takes about a minute or less)."
        if ($cpu -ge 0.5 * $ran) {
            throw ("$head It was busy the whole time, which happens when the game's executable is not the one the build is made for. " +
                "Check that your disc image is a complete, unmodified copy of Transformers USA (SLUS-20668) or the European disc (SLES-52388): " +
                "delete the folder $Game and run the setup again with a fresh copy of your disc. The step's log: $Log")
        }
        throw ("$head It was waiting most of the time (it used the processor for $([int]$cpu) s), so something held it: " +
            "most often an antivirus (add the game folder $Root to its exclusions), Windows' Controlled Folder Access (Windows Security > " +
            "Virus & threat protection > Ransomware protection: allow the setup, or turn it off during the install), or a slow external drive " +
            "(choose a folder on the PC's own disk, for example C:\Games\TFA). Running the setup as administrator helped one player. " +
            "Then run the setup again: finished steps are kept. The step's log: $Log")
    }
    if ($p.ExitCode -ne 0) {
        Assert-NotBlocked $Exe $p.ExitCode
        throw "$(Split-Path $Exe -Leaf) failed with exit code $($p.ExitCode), see $Log"
    }
}

# (TODO 378) A damaged object file: a build that stopped while the compiler was writing one (an update cancelled or
# stopped, a crash, the PC switched off) can leave it cut short or empty with a fresh time, and MSBuild then counts it
# as up to date, so every later build fails at the same link (a player's: "batch_00270000.obj : fatal error LNK1136").
# Before a build the empty ones are deleted (Remove-EmptyBuildFiles); after a failed build the files the linker or the
# librarian refused are found by their error code - never by the message, which is in the PC's language - deleted and
# the build is run once more (Get-RefusedObjects, Invoke-HealingBuild).
function Remove-EmptyBuildFiles([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir)) { return }
    $gone = [Collections.Generic.List[string]]::new()
    try {
        foreach ($fi in [IO.DirectoryInfo]::new($Dir).EnumerateFiles("*", [IO.SearchOption]::AllDirectories)) {
            if ($fi.Length -eq 0 -and ($fi.Extension -ieq ".obj" -or $fi.Extension -ieq ".lib")) {
                try { $fi.Delete(); $gone.Add($fi.Name) } catch {}
            }
        }
    } catch {}
    if ($gone.Count) {
        $text = "$($gone.Count) empty compiled file(s) left by a build that stopped half way were deleted, so they are made again: $($gone -join ', ')"
        Write-Host "  $text" -ForegroundColor Yellow
        Write-StepLog "build: $text"
    }
}
# The object files a failed build's output says the linker or the librarian could not read: LNK1136 (invalid or
# corrupt file), LNK1107 (invalid or corrupt file: cannot read at ...), LNK1106 (invalid file or disk full: cannot seek
# to ... - what a file cut short gives, found by test), LNK1143 and LNK1235 (a corrupt section or symbol table), and
# LNK1104 (cannot open file) when that file is empty. Only files under one of $Roots that are there. A name without a folder is read from the project's folder
# (MSBuild adds the project in [brackets] at the end of the line).
function Get-RefusedObjects([string[]]$Lines, [string[]]$Roots) {
    $found = [Collections.Generic.List[string]]::new()
    $roots = @($Roots | Where-Object { $_ } | ForEach-Object { [IO.Path]::GetFullPath($_).TrimEnd('\') + '\' })
    foreach ($line in $Lines) {
        $file = $null
        $empty = $false
        $m = [regex]::Match($line, '^\s*(?<file>[^\[\r\n]+?\.obj)\s*:[^\[]*?\b(?:LNK1136|LNK1107|LNK1106|LNK1143|LNK1235)\b', 'IgnoreCase')
        if ($m.Success) {
            $file = $m.Groups['file'].Value.Trim()
        } elseif ($line -match '\bLNK1104\b') {
            $qc = '[''"\u2018\u2019\u201A\u201C\u201D\u201E\u00AB\u00BB]'    # (the quotes round the name: any language's)
            $q = [regex]::Match($line, $qc + '(?<file>(?:(?!' + $qc + ').)+?\.obj)' + $qc, 'IgnoreCase')
            if ($q.Success) { $file = $q.Groups['file'].Value.Trim(); $empty = $true }
        }
        if (-not $file) { continue }
        try {
            if (-not [IO.Path]::IsPathRooted($file)) {
                $p = [regex]::Match($line, '\[(?<proj>[^\[\]]+\.(?:vcxproj|vcproj))\]\s*$', 'IgnoreCase')
                if (-not $p.Success) { continue }
                $file = Join-Path (Split-Path $p.Groups['proj'].Value -Parent) $file
            }
            $file = [IO.Path]::GetFullPath($file)
        } catch { continue }
        if (-not ($roots | Where-Object { $file.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) })) { continue }
        if (-not [IO.File]::Exists($file)) { continue }
        if ($empty -and ([IO.FileInfo]::new($file)).Length -ne 0) { continue }
        if (-not ($found | Where-Object { $_ -ieq $file })) { $found.Add($file) }
    }
    , $found.ToArray()
}
# A cmake --build (its output through $Each, a script block given each line, which passes on what is shown) that
# heals damaged object files: the refused ones deleted, the build run again. The linker stops at the first damaged
# file it meets, so a build stopped with several compiles under way needs a round for each (found by test: a third
# file was named after the second); it stops with the file's name when a file it made again is refused again, or after
# 24 rounds.
function Invoke-HealingBuild([string[]]$Arguments, [string]$Root, [scriptblock]$Each) {
    $healed = [Collections.Generic.List[string]]::new()
    for ($attempt = 1; ; $attempt++) {
        $lines = [Collections.Generic.List[string]]::new()
        $ErrorActionPreference = "Continue"
        try { & cmake $Arguments 2>&1 | ForEach-Object { $line = "$_"; $lines.Add($line); & $Each $line } }
        catch { $ErrorActionPreference = "Stop"; Assert-NotBlocked "cmake" -1 "$_"; throw }
        $code = $LASTEXITCODE
        $ErrorActionPreference = "Stop"
        if ($code -eq 0) { return }
        $refused = Get-RefusedObjects $lines.ToArray() @($Root)
        if ($refused.Count -eq 0) { Assert-NotBlocked "cmake" $code; throw "cmake --build failed with exit code $code$(if ($script:CompilerText) { " (built with $($script:CompilerText))" })" }
        $names = ($refused | ForEach-Object { Split-Path $_ -Leaf }) -join ", "
        if ($attempt -gt 24 -or ($refused | Where-Object { $healed -icontains $_ })) {
            throw "The build stopped again at a damaged file the compiler made: $($refused[0]). Delete that file and run " +
                "the update (or the build) again: it is made again. If it stops at another one, delete the folder " +
                "$(Split-Path $refused[0] -Parent) instead: everything in it is compiled again (10 to 20 minutes)."
        }
        foreach ($f in $refused) { Remove-Item -LiteralPath $f -Force; $healed.Add($f) }
        $text = "a file the compiler made earlier was damaged ($names), most likely left by a build that stopped half way: deleted, the build runs once more and makes it again"
        Write-Host "  A$($text.Substring(1))" -ForegroundColor Yellow
        Write-StepLog "build: $text"
    }
}

foreach ($tool in "cmake", $Py, "git") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool was not found on PATH. See README.md for the requirements."
    }
}

if (-not (Test-Path (Join-Path $Root "PS2Recomp\CMakeLists.txt"))) {
    Step "Fetching PS2Recomp" "fetch"
    Run git @("-C", $Root, "submodule", "update", "--init", "--recursive")
}

# Fixes to PS2Recomp that this game needs, kept as patches so the submodule
# stays on a plain upstream commit.
$patches = if ($NoPatches) { @() } else { Get-ChildItem (Join-Path $Root "patches\ps2recomp") -Filter *.patch -ErrorAction SilentlyContinue | Sort-Object Name }
# The series is applied as a whole and recorded (out\ps2recomp-patches.txt: each patch's name and
# hash). A later patch may change the lines around an earlier one's, so "does this one patch
# reverse?" can no longer tell whether the earlier one is in; when the record differs from the
# series, the submodule goes back to its commit and the whole series is applied again in order.
# Files that come out of that exactly as they were keep their times (TODO 115): otherwise every
# file any patch touches - ps2_runtime.h among them, which all of the game's code includes - would
# look new, and one new runtime patch would recompile the whole game.
$ps2recomp = Join-Path $Root "PS2Recomp"
$patchRecord = Join-Path $Root "out\ps2recomp-patches.txt"
$series = ($patches | ForEach-Object { "$($_.Name) $((Get-FileHash $_.FullName -Algorithm SHA256).Hash)" }) -join "`n"
$recorded = if (Test-Path $patchRecord) { (Get-Content $patchRecord -Raw).Trim() } else { $null }
# (build robustness) The record says which series was applied, not that the files are still as it left them: a
# PS2Recomp file changed or put back since (an antivirus restore, a hand edit, a "git submodule update --force", a
# copy cut short) would be built as it is - the recompiler without its fixes (the TODO 407 error came back that
# way if patch 1005 was undone). So the files the series writes are hashed into out\ps2recomp-tree.txt after
# it is applied, and checked at every build (well under a second); any difference applies the series again.
$treeRecord = Join-Path $Root "out\ps2recomp-tree.txt"
function Get-PatchedTreeState {
    $rels = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    foreach ($p in $patches) {
        foreach ($m in [regex]::Matches((Get-Content -LiteralPath $p.FullName -Raw), '(?m)^\+\+\+ b/(.+?)\s*$|^rename to (.+?)\s*$')) {
            $rel = if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }
            [void]$rels.Add($rel)
        }
    }
    ($rels | ForEach-Object {
        $path = Join-Path $ps2recomp $_
        "$_ $(if (Test-Path -LiteralPath $path -PathType Leaf) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { 'absent' })"
    }) -join "`n"
}
$treeChanged = $false
if ($patches -and $recorded -eq $series) {
    $state = Get-PatchedTreeState
    if (Test-Path -LiteralPath $treeRecord) {
        $was = (Get-Content -LiteralPath $treeRecord -Raw).Trim()
        if ($was -ne $state) {
            $old = @{}; foreach ($l in ($was -split "`n")) { $k, $v = $l -split ' ', 2; $old[$k] = $v }
            $diff = @(($state -split "`n") | Where-Object { $k, $v = $_ -split ' ', 2; $old[$k] -ne $v } | ForEach-Object { ($_ -split ' ', 2)[0] })
            Write-Host "  PS2Recomp's files are not as the patch series left them ($($diff.Count): $(($diff | Select-Object -First 3) -join ', ')$(if ($diff.Count -gt 3) { ', ...' })): the series is applied again" -ForegroundColor Yellow
            Write-StepLog "build: PS2Recomp changed since its patches ($($diff.Count) file(s)): applied again"
            $treeChanged = $true
        }
    } else {
        # (an install from before this record: the series record matches, so the files are taken as they are)
        Set-Content -LiteralPath $treeRecord -Value $state -NoNewline
    }
}
if ($patches -and ($recorded -ne $series -or $treeChanged)) {
    # With a record, the tree holds the recorded series: it is reset and the new one applied. A
    # tree without one (from before the record) that already has exactly this series in it (each
    # patch reverses) needs no reset.
    $allIn = $false
    if ($null -eq $recorded) {
        $allIn = $true
        foreach ($patch in $patches) {
            $ErrorActionPreference = "Continue"
            & git -C $ps2recomp apply --reverse --check $patch.FullName 2>$null
            if ($LASTEXITCODE -ne 0) { $allIn = $false }
            $ErrorActionPreference = "Stop"
            if (-not $allIn) { break }
        }
    }
    if (-not $allIn) {
        Step "Applying the PS2Recomp patch series ($($patches.Count) patches) to a clean submodule" "patches"
        # Every file the series in the tree changed or added: its contents and time, kept
        $before = @{}
        $ErrorActionPreference = "Continue"
        $listed = (& git -C $ps2recomp -c core.quotepath=false ls-files -z -m -o --exclude-standard 2>$null) -join ""
        $ErrorActionPreference = "Stop"
        foreach ($rel in ($listed -split "`0" | Where-Object { $_ } | Select-Object -Unique)) {
            $path = Join-Path $ps2recomp $rel
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                $before[$rel] = @((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash, [IO.File]::GetLastWriteTimeUtc($path))
            }
        }
        Run git @("-C", $ps2recomp, "checkout", "--", ".")
        Run git @("-C", $ps2recomp, "clean", "-fdq")
        foreach ($patch in $patches) {
            Write-Host "  $($patch.Name)"
            Run git @("-C", $ps2recomp, "apply", "--whitespace=nowarn", $patch.FullName)
        }
        $same = 0
        foreach ($rel in $before.Keys) {
            $path = Join-Path $ps2recomp $rel
            if ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $before[$rel][0]) {
                [IO.File]::SetLastWriteTimeUtc($path, $before[$rel][1])
                $same++
            }
        }
        Write-Host "  $($before.Count - $same) file(s) changed by the new series; $same kept as they were"
    }
    New-Item -ItemType Directory -Force (Split-Path $patchRecord) | Out-Null
    Set-Content -Path $patchRecord -Value $series -NoNewline
    Set-Content -LiteralPath $treeRecord -Value (Get-PatchedTreeState) -NoNewline
}

# 1. Game data -------------------------------------------------------------
# (TODO 441) The game's executable already in the game folder is checked (its checksum, a fraction of a second): a wrong
# one - left by an extraction that refused it only after copying it, or copied in by hand - went straight to the SDK scan,
# never checked again. With the disc image given it is extracted again (the extraction checks the disc first); without,
# the build stops in plain words.
$elfPath = Join-Path $Game $ElfName
if ((Test-Path -LiteralPath $elfPath) -and $Py) {
    $ErrorActionPreference = "Continue"
    $elfCheck = "$(& $Py (Join-Path $Tools "extract_iso.py") --check-elf $elfPath 2>&1 | Out-String)".Trim()
    $elfCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($elfCode -eq 2) {
        Write-StepLog "build: wrong executable in the game folder: $elfCheck"
        if ($Iso) {
            Write-Host "  $elfCheck" -ForegroundColor Yellow
            Write-Host "  The game folder's executable is extracted again from $Iso." -ForegroundColor Yellow
            Remove-Item -LiteralPath $elfPath -Force
        } else {
            throw ("$elfCheck It is in $Game. Delete that folder and run the setup again with your disc image " +
                "(a complete, unmodified copy of Transformers USA, SLUS-20668, or the European disc, SLES-52388): finished steps are kept.")
        }
    } elseif ($elfCode -ne 0) { Write-Host "  (the game's executable could not be checked: $elfCheck)" }
}
if (-not (Test-Path (Join-Path $Game $ElfName))) {
    if (-not $Iso) {
        $found = Get-ChildItem $Root -Filter *.iso -File | Select-Object -First 1
        if (-not $found) { throw "No game data. Pass your disc image: .\build.ps1 -Iso `"Transformers (USA).iso`"" }
        $Iso = $found.FullName
    }
    Step "Extracting $Iso" "extract"
    Run $Py @((Join-Path $Tools "extract_iso.py"), $Iso, $Game)
}

# 2. Tools -----------------------------------------------------------------
# The Visual Studio that builds: an install that has the C++ tools and that this CMake knows - Visual Studio
# 2022 when there is one (the version the releases are built and tested with), else the newest.
# Left to itself, CMake takes the first install of its default version, which can be one without the
# C++ tools (a C#-only Visual Studio next to the Build Tools): "No CMAKE_CXX_COMPILER could be found"
# (a player's setup, 2026-11-09). A build folder already set up with a C++ install keeps it; one set up
# with an install without them (that configure failed) is set up again.
# (TODO 407) Each build says which Visual Studio and which compiler it used (Write-CompilerReport), and a failed
# compile says it in its message: a player's report then names the compiler that failed.
$TestedVsMajor = 17
# vswhere's C++ installs (its JSON objects) as: Path, Version, Major, Name, Shown (the version Visual Studio shows),
# Generator (CMake's name for it) and Known (whether this CMake has that generator).
function ConvertTo-VsCppInstances($List, [string]$CMakeHelp) {
    $names = @{ 15 = "Visual Studio 15 2017"; 16 = "Visual Studio 16 2019"; 17 = "Visual Studio 17 2022"; 18 = "Visual Studio 18 2026" }
    @($List | Where-Object { $_.installationPath -and $_.installationVersion } | ForEach-Object {
        $major = [int]("$($_.installationVersion)" -split '\.')[0]
        $gen = if ($names.ContainsKey($major)) { $names[$major] } else { "Visual Studio $major" }
        $shown = if ($_.catalog -and $_.catalog.productDisplayVersion) { "$($_.catalog.productDisplayVersion)" } else { "$($_.installationVersion)" }
        $name = if ($_.displayName) { "$($_.displayName)" } else { $gen }
        [pscustomobject]@{ Path = "$($_.installationPath)"; Version = [version]"$($_.installationVersion)"; Major = $major; Name = $name
                           Shown = $shown; Generator = $gen; Known = ($names.ContainsKey($major) -and $CMakeHelp.Contains($gen)) }
    })
}
# The order they are tried in: the known ones, the tested major first, then the newest.
function Select-VsInstances($All) {
    $known = @($All | Where-Object { $_.Known })
    @(@($known | Where-Object { $_.Major -eq $TestedVsMajor } | Sort-Object Version -Descending) +
      @($known | Where-Object { $_.Major -ne $TestedVsMajor } | Sort-Object Version -Descending))
}
$script:VsAll = $null
function Get-VsAllCpp {
    if ($null -ne $script:VsAll) { return $script:VsAll }
    $script:VsAll = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { return $script:VsAll }
    $ErrorActionPreference = "Continue"
    $json = (& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json 2>$null | Out-String)
    # (Windows PowerShell 5.1 passes a JSON array down the pipe as ONE object; ForEach-Object unrolls it. Without
    # that, with two Visual Studio installs every field below is an array: "Cannot convert System.Object[] to
    # System.Version", public issue #25.)
    try { $list = @($json | ConvertFrom-Json | ForEach-Object { $_ }) } catch { return $script:VsAll }
    $help = (& cmake --help 2>$null | Out-String)
    $script:VsAll = @(ConvertTo-VsCppInstances $list $help)
    $script:VsAll
}
function Get-VsCppInstances { Select-VsInstances (Get-VsAllCpp) }
# What a build folder was set up with, in one line: "Visual Studio Community 2022 17.14.20, MSVC 19.44.35229
# (tools 14.44.35207, Hostx64), CMake 4.1.2". From the folder's own files (CMakeCache.txt and the compiler file
# CMake writes when it tests the compiler), so it is right whatever chose it.
function Get-CompilerReport([string]$Dir, $Installs) {
    $cache = Join-Path $Dir "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cache)) { return "" }
    $get = { param($key) $m = Select-String -LiteralPath $cache -Pattern "^$key(?::[A-Za-z]+)?=(.*)$" | Select-Object -First 1
             if ($m) { $m.Matches[0].Groups[1].Value.Trim() } else { "" } }
    $gen = & $get "CMAKE_GENERATOR"
    $inst = & $get "CMAKE_GENERATOR_INSTANCE"
    $parts = [Collections.Generic.List[string]]::new()
    $norm = { param($p) ("$p" -replace '\\', '/').TrimEnd('/').ToLowerInvariant() }
    $vs = if ($inst) { @($Installs | Where-Object { (& $norm $_.Path) -eq (& $norm $inst) }) | Select-Object -First 1 }
    if ($vs) { $parts.Add("$($vs.Name) $($vs.Shown)") } elseif ($gen) { $parts.Add("$gen$(if ($inst) { " ($inst)" })") }
    $files = Join-Path $Dir "CMakeFiles"
    $cxx = if (Test-Path -LiteralPath $files) { Get-ChildItem -LiteralPath $files -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-Item -LiteralPath (Join-Path $_.FullName "CMakeCXXCompiler.cmake") -ErrorAction SilentlyContinue } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1 }
    if ($cxx) {
        $text = Get-Content -LiteralPath $cxx.FullName -Raw
        $id = [regex]::Match($text, 'set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)').Groups[1].Value
        $ver = [regex]::Match($text, 'set\(CMAKE_CXX_COMPILER_VERSION "([^"]*)"\)').Groups[1].Value
        $path = [regex]::Match($text, 'set\(CMAKE_CXX_COMPILER "([^"]*)"\)').Groups[1].Value
        $tools = [regex]::Match($path, '(?i)/MSVC/([0-9.]+)/bin/(Host[A-Za-z0-9]+)/')
        $c = "$(if ($id -eq 'MSVC') { 'MSVC' } elseif ($id) { $id } else { 'compiler' }) $ver"
        if ($tools.Success) { $c += " (tools $($tools.Groups[1].Value), $($tools.Groups[2].Value))" }
        $parts.Add($c)
    }
    $cm = & $get "CMAKE_COMMAND"
    $cmVer = @("CMAKE_CACHE_MAJOR_VERSION", "CMAKE_CACHE_MINOR_VERSION", "CMAKE_CACHE_PATCH_VERSION" | ForEach-Object { & $get $_ }) -join "."
    if ($cmVer -ne "..") { $parts.Add("CMake $cmVer$(if ($cm -match '(?i)site-packages') { ' (pip)' })") }
    $parts -join ", "
}
$script:CompilerText = ""
function Write-CompilerReport([string]$Dir, [string]$What) {
    $text = Get-CompilerReport $Dir (Get-VsAllCpp)
    if (-not $text) { return }
    if ($text -ne $script:CompilerText) { Write-Host "  $What built with: $text" }
    $script:CompilerText = $text
    Write-StepLog "build: $What built with: $text"
}
function Get-GeneratorArgs([string]$Dir) {
    $a = @("-A", "x64")
    if ($Generator) { return $a + @("-G", $Generator) }
    $vs = @(Get-VsCppInstances)
    if (-not $vs) {
        # (TODO 407) C++ installs there, but none this CMake knows (Visual Studio 2026 with a CMake from before it): CMake's
        # own choice would stop with "could not find any instance of Visual Studio" - say what to do instead.
        $newer = @(Get-VsAllCpp | Where-Object { -not $_.Known })
        if ($newer) {
            $cmv = "$(& cmake --version 2>$null | Select-Object -First 1)".Trim()
            throw "This CMake ($cmv) does not know $(($newer | ForEach-Object { "$($_.Name) $($_.Shown)" }) -join ' or '). " +
                "Update CMake (in PowerShell: python -m pip install --user --upgrade cmake), then run the setup (or the build) again: finished steps are kept."
        }
        return $a   # (no vswhere, or no C++ install: CMake's own choice, as before)
    }
    $norm = { param($p) ("$p" -replace '\\', '/').TrimEnd('/').ToLowerInvariant() }
    $cache = Join-Path $Dir "CMakeCache.txt"
    if (Test-Path -LiteralPath $cache) {
        # (TODO 381: any type - CMake keeps the -D we pass as UNINITIALIZED, with backslashes; one it set itself is INTERNAL)
        $m = Select-String -LiteralPath $cache -Pattern '^CMAKE_GENERATOR_INSTANCE(?::[A-Za-z]+)?=(.*)$' | Select-Object -First 1
        $was = if ($m) { & $norm $m.Matches[0].Groups[1].Value } else { "" }
        if ($was -and @($vs | Where-Object { (& $norm $_.Path) -eq $was })) { return $a }
        Write-Host "  (the build folder $(Split-Path $Dir -Leaf) was set up with a Visual Studio without the C++ tools: setting it up again)" -ForegroundColor Yellow
        Remove-Item -LiteralPath $cache -Force
        $files = Join-Path $Dir "CMakeFiles"
        if (Test-Path -LiteralPath $files) { Remove-Item -LiteralPath $files -Recurse -Force }
    }
    if ($vs[0].Major -lt $TestedVsMajor) {
        Write-Host "  ($($vs[0].Name) is older than the Visual Studio 2022 the game is built and tested with. If the build stops, install Visual Studio 2022's Build Tools (the setup offers them) and run it again.)" -ForegroundColor Yellow
    }
    $a + @("-G", $vs[0].Generator, "-DCMAKE_GENERATOR_INSTANCE=$($vs[0].Path)")
}
$genArgs = Get-GeneratorArgs $ToolsBuild
$cmakeArgs = @("-S", $Root, "-B", $Build, "-DTF_GAME_VERSION=$GameVersion") + (Get-GeneratorArgs $Build)
if ($Public) { $cmakeArgs += "-DTF_OVERLAY_DIR=" } # (the overlay left out: transformers\CMakeLists.txt)
if ($Vulkan) { $cmakeArgs += "-DTF_VULKAN=ON" }     # (the Vulkan backend: transformers\CMakeLists.txt; -DTF_VULKAN=OFF turns it off again)
# (the FidelityFX SDK's DLLs, transformers\CMakeLists.txt: always said, so a build folder made before they were the default -
# its cache holding OFF - fetches them too; fetched once a release, a failed fetch only a warning)
$cmakeArgs += $(if ($NoFsrSdk) { "-DTF_FSR_SDK=OFF" } else { "-DTF_FSR_SDK=ON" })
$cmakeArgs += "-DTF_GAME_DIR=$Game"                 # (the disc's files where they are: game\ or the install's data\game)
if ($Sdl3 -or $Vulkan) { $cmakeArgs += "-DPS2X_SDL3=ON" } # (SDL3 as the window at run time: PS2Recomp's ps2_platform.h - Vulkan presents it, so -Vulkan too)
# (TODO 444) CMake's own Python (find_package(Python3) for the files it generates; on Windows it looks in the registry
# first, where the Microsoft Store's Python is listed): the build's, when the folder has none yet, or a WindowsApps one or
# one that is gone. A usable one already there is kept (another value would make those files again).
if ($Py -ne "python") {
    $cachedPy = ""
    $pyCache = Join-Path $Build "CMakeCache.txt"
    if (Test-Path -LiteralPath $pyCache) {
        $hit = Select-String -LiteralPath $pyCache -Pattern '^Python3_EXECUTABLE:[^=]*=(.*)$' | Select-Object -First 1
        if ($hit) { $cachedPy = $hit.Matches[0].Groups[1].Value.Trim() }
    }
    if (-not $cachedPy -or ($cachedPy.Replace('/', '\')) -like '*\WindowsApps\*' -or $cachedPy -like "*-NOTFOUND" -or -not (Test-Path -LiteralPath $cachedPy -PathType Leaf)) {
        $cmakeArgs += "-DPython3_EXECUTABLE=$($Py.Replace('\', '/'))"
    }
}
foreach ($define in ($CMakeDefine -split ";" | Where-Object { $_ })) { $cmakeArgs += "-D$define" } # (-CMakeDefine: cache settings for that folder, which keeps them)

# What the translation (steps 3-4) depends on, one line per input: "<name> <SHA-256>". The
# recompiler and analyzer are the programs themselves - MSBuild relinks them only when a source
# they are made from changed (PS2Recomp patches included), so this follows their real inputs.
function Get-TranslationInputs {
    $files = [ordered]@{
        "ps2_recomp.exe"   = Join-Path $ToolsBuild "ps2xRecomp\$Config\ps2_recomp.exe"
        "ps2_analyzer.exe" = Join-Path $ToolsBuild "ps2xAnalyzer\$Config\ps2_analyzer.exe"
        $ElfName           = Join-Path $Game $ElfName
    }
    # the Python tools steps 3-4 run, and the ones they import from the same folder
    $todo = [Collections.Generic.Queue[string]]::new()
    foreach ($t in "find_functions.py", "make_config.py", "sync_generated.py") { $todo.Enqueue($t) }
    while ($todo.Count) {
        $name = $todo.Dequeue()
        if ($files.Contains("tools/$name")) { continue }
        $path = Join-Path $Tools $name
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $files["tools/$name"] = $path
        foreach ($m in [regex]::Matches((Get-Content -LiteralPath $path -Raw), '(?m)^\s*(?:from|import)\s+([A-Za-z_]\w*)')) {
            if (Test-Path -LiteralPath (Join-Path $Tools "$($m.Groups[1].Value).py")) { $todo.Enqueue("$($m.Groups[1].Value).py") }
        }
    }
    $configDir = Join-Path $Root "transformers\config"
    if (Test-Path $configDir) {
        Get-ChildItem $configDir -File -Recurse | Sort-Object FullName | ForEach-Object {
            $files["config/" + $_.FullName.Substring($configDir.Length + 1).Replace('\', '/')] = $_.FullName
        }
    }
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("scheme 1 $Config")
    foreach ($key in $files.Keys) {
        $hash = if (Test-Path -LiteralPath $files[$key]) { (Get-FileHash -LiteralPath $files[$key] -Algorithm SHA256).Hash } else { "missing" }
        $lines.Add("$key $hash")
    }
    # settings the tools read from the environment
    Get-ChildItem Env: | Where-Object { $_.Name -match '^(PS2X_CODEGEN|PS2RECOMP_)' } | Sort-Object Name | ForEach-Object { $lines.Add("env $($_.Name)=$($_.Value)") }
    [pscustomobject]@{ Text = ($lines -join "`n"); Files = $files }
}

if (-not $SkipRecompile) {
    Step "Building recompiler tools" "tools"
    # (configured at every build, ~15 s: the build's own reconfigure, when a source file came, takes
    # effect only at the build after - a new file would be left out of this one)
    Run cmake (@("-S", (Join-Path $Root "PS2Recomp"), "-B", $ToolsBuild,
                 "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF") + $genArgs + (Get-DepDefines $Root "tools" $ToolsBuild))
    Remove-EmptyBuildFiles $ToolsBuild    # (TODO 378)
    Write-CompilerReport $ToolsBuild "The recompiler tools are"
    Invoke-HealingBuild (@("--build", $ToolsBuild, "--config", $Config, "--target", "ps2_recomp", "ps2_analyzer", "--") + $BuildParallel + @("/v:m")) $ToolsBuild { param($l) $l }
    # (both there right after the build: an antivirus that took one says so here, not as a puzzling failure later)
    foreach ($t in "ps2xAnalyzer\$Config\ps2_analyzer.exe", "ps2xRecomp\$Config\ps2_recomp.exe") { Assert-NotBlocked (Join-Path $ToolsBuild $t) }

    # 3-4. Recompile, when its inputs changed ---------------------------------
    $inputsFile = Join-Path $Work "translation-inputs.txt"
    $syncCache = Join-Path $Work "generated\.sync_cache.json"
    $haveOutput = (Test-Path (Join-Path $Work "generated\register_functions.cpp")) -and (Test-Path $syncCache)
    $inputs = Get-TranslationInputs
    # Three records, so a step is redone only when what it reads changed:
    #   scan-inputs.txt         the SDK scan: the analyzer and the ELF
    #   config-inputs.txt       the function list and the recompiler's config: the scan's result
    #                           (analyzer.toml, by content), the Python tools, the config, the ELF
    #   translation-inputs.txt  the translation and the generated files: all of the above and the
    #                           recompiler itself
    # A change to the recompiler alone (a code-generator patch) translates again with the function
    # list as it was; a patch that rebuilds the analyzer without changing what it finds skips the
    # function search (~1 min).
    $lines = $inputs.Text -split "`n"
    $scanText = ($lines | Where-Object { $_ -match ('^(scheme|ps2_analyzer\.exe|' + [regex]::Escape($ElfName) + '|env) ') }) -join "`n"
    $configBase = ($lines | Where-Object { $_ -notmatch '^(ps2_recomp\.exe|ps2_analyzer\.exe|tools/sync_generated\.py) ' }) -join "`n"
    $scanFile = Join-Path $Work "scan-inputs.txt"
    $configFile = Join-Path $Work "config-inputs.txt"
    function Get-ConfigText { $configBase + "`nanalyzer.toml " + (Get-FileHash -LiteralPath (Join-Path $Work "analyzer.toml") -Algorithm SHA256).Hash }
    function Test-Record([string]$file, [string]$text) { (Test-Path -LiteralPath $file) -and (Get-Content -LiteralPath $file -Raw).Trim() -eq $text }
    $upToDate = $false
    $why = ""
    if ($haveOutput -and (Test-Path $inputsFile)) {
        $old = (Get-Content $inputsFile -Raw).Trim()
        $upToDate = $old -eq $inputs.Text
        if (-not $upToDate) {
            # (which inputs differ, for the log: the first word of each line names it)
            $was = @{}; $now = @{}
            foreach ($l in ($old -split "`n")) { $was[($l -split ' ')[0]] = $l }
            foreach ($l in $lines) { $now[($l -split ' ')[0]] = $l }
            $why = (@($now.Keys) + @($was.Keys) | Select-Object -Unique | Where-Object { $was[$_] -ne $now[$_] } | Sort-Object) -join ", "
        }
    } elseif ($haveOutput) {
        # A tree translated before these records existed: up to date when nothing it depends on is
        # newer than the last translation's output (the sync after a translation that finished).
        $done = [IO.File]::GetLastWriteTimeUtc($syncCache)
        $log = Join-Path $Work "recomp.log"
        $upToDate = (Test-Path $log) -and [IO.File]::GetLastWriteTimeUtc($log) -le $done -and (Test-Path (Join-Path $Work "analyzer.toml"))
        foreach ($path in $inputs.Files.Values) {
            if (-not (Test-Path -LiteralPath $path) -or [IO.File]::GetLastWriteTimeUtc($path) -ge $done) { $upToDate = $false }
        }
        if ($upToDate -and (Test-Path (Join-Path $Work "transformers.toml"))) {
            # (and the recompiler's config made again must be the one the translation used: settings
            # read from the environment leave no file time behind)
            $probe = Join-Path $Work "transformers.check.toml"
            try {
                Run $Py @((Join-Path $Tools "make_config.py"), $Work, $probe) | Out-Null
                $upToDate = (Get-FileHash -LiteralPath $probe).Hash -eq (Get-FileHash -LiteralPath (Join-Path $Work "transformers.toml")).Hash
            } catch { $upToDate = $false }
            Remove-Item -LiteralPath $probe -ErrorAction SilentlyContinue
        } else { $upToDate = $false }
        if ($upToDate) {
            # (the records written from what is there)
            Set-Content -Path $scanFile -Value $scanText -NoNewline
            Set-Content -Path $configFile -Value (Get-ConfigText) -NoNewline
            Set-Content -Path $inputsFile -Value $inputs.Text -NoNewline
        }
    }
    if ($upToDate) {
        Step "The game's code is translated as before (the recompiler and what it reads are unchanged)" "check"
    } else {
        if ($why) { Write-Host "  changed since the last translation: $why" }
        # (a translation that does not finish leaves no record: the next build translates again)
        Remove-Item $inputsFile -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $Work | Out-Null
        Push-Location $Work
        try {
            if ((Test-Record $scanFile $scanText) -and (Test-Path (Join-Path $Work "analyzer.toml")) -and (Test-Path (Join-Path $Work $ElfName))) {
                Write-Host "  (the SDK scan is as before)"
            } else {
                Remove-Item $scanFile -ErrorAction SilentlyContinue
                Copy-Item (Join-Path $Game $ElfName) $Work -Force
                Step "Scanning for SDK functions$(Usual 'scan')" "scan"
                # (TODO 441) stopped in plain words after 30 min (TF_SCAN_LIMIT_MINUTES; 0 = no limit): it takes 20-60 s
                $scanLimit = 30 * 60
                if ($env:TF_SCAN_LIMIT_MINUTES -match '^\d{1,4}$') { $scanLimit = [int]$env:TF_SCAN_LIMIT_MINUTES * 60 }
                RunLogged (Join-Path $ToolsBuild "ps2xAnalyzer\$Config\ps2_analyzer.exe") @($ElfName, "analyzer.toml") (Join-Path $Work "analyzer.log") "The SDK scan" -TimeLimit $scanLimit
                Set-Content -Path $scanFile -Value $scanText -NoNewline
            }
            $configText = Get-ConfigText
            if ((Test-Record $configFile $configText) -and -not (@("functions.csv", "jump_tables.json", "transformers.toml") | Where-Object { -not (Test-Path (Join-Path $Work $_)) })) {
                Write-Host "  (the function list and the recompiler's config are as before)"
            } else {
                Remove-Item $configFile -ErrorAction SilentlyContinue
                Step "Recovering function boundaries$(Usual 'functions')" "functions"
                Run $Py @((Join-Path $Tools "find_functions.py"), $ElfName, "functions.csv", "--names", "analyzer.toml", "--jump-tables", "jump_tables.json")
                Run $Py @((Join-Path $Tools "make_config.py"), $Work, "transformers.toml")
                Set-Content -Path $configFile -Value $configText -NoNewline
            }

            Step "Translating MIPS to C++$(Usual 'translate')" "translate"
            if (Test-Path staging) { Remove-Item -Recurse -Force staging }
            RunLogged (Join-Path $ToolsBuild "ps2xRecomp\$Config\ps2_recomp.exe") @("transformers.toml") (Join-Path $Work "recomp.log") "The translation"

            # Install only what changed, so the compile below is incremental.
            Step "Updating generated sources" "sync"
            Run $Py @((Join-Path $Tools "sync_generated.py"), "staging", "generated", "batches")
        }
        finally { Pop-Location }
        Set-Content -Path $inputsFile -Value $inputs.Text -NoNewline
    }
}

# 4b. The overlay's own build steps (overlay\build_hooks.ps1: the private tree's mods), when the tree has one and this is
# not the public build. It is dot-sourced here: it may add CMake arguments ($cmakeArgs) and read its own switches from
# $OverlayArgs (the arguments this script does not know, e.g. -Decomp).
$ForecastIncludeDirs = @()   # (more include folders for the compile forecast below: the overlay hooks add theirs)
$overlayHooks = Join-Path $Root "overlay\build_hooks.ps1"
if (-not $Public -and (Test-Path -LiteralPath $overlayHooks)) { . $overlayHooks }

# 5. Game executable ---------------------------------------------------------
# A rough count of the files the compile below will redo (shown with the time it should take): a
# source whose object file is missing, or older than the source or than any header it includes
# (followed through the includes); a batch of generated code also when one of its generated files
# is newer. MSBuild decides for real; this only feeds the estimate, so it never stops the build.
function Get-CompileForecast {
    $rt = Join-Path $Root "PS2Recomp\ps2xRuntime"
    $iop = Join-Path $Root "PS2Recomp\ps2xIOP"
    $tf = Join-Path $Root "transformers"
    $gen = Join-Path $Work "generated"
    $incDirs = @((Join-Path $rt "include"), (Join-Path $iop "include"), (Join-Path $rt "src\lib"), (Join-Path $tf "src"),
                 (Join-Path $tf "decomp"), (Join-Path $tf "native_render")) + @($ForecastIncludeDirs)
    $memo = @{}
    $includeRe = [regex]'(?m)^\s*#\s*include\s*[<"]([^>"]+)[>"]'
    # the newest time of a file and everything it includes
    function Newest([string]$path) {
        if ($memo.ContainsKey($path)) { return $memo[$path] }
        $t = [IO.File]::GetLastWriteTimeUtc($path)
        $memo[$path] = $t                       # (an include cycle stops here)
        $dir = [IO.Path]::GetDirectoryName($path)
        foreach ($m in $includeRe.Matches([IO.File]::ReadAllText($path))) {
            $name = $m.Groups[1].Value
            foreach ($d in @($dir) + $incDirs) {
                $f = [IO.Path]::GetFullPath([IO.Path]::Combine($d, $name))
                if ([IO.File]::Exists($f)) { $n = Newest $f; if ($n -gt $t) { $t = $n }; break }
            }
        }
        $memo[$path] = $t
        $t
    }
    function ObjTime([string]$dir, [string]$source) {
        $obj = Join-Path $dir ([IO.Path]::GetFileNameWithoutExtension($source) + ".obj")
        if ([IO.File]::Exists($obj)) { [IO.File]::GetLastWriteTimeUtc($obj) } else { [datetime]::MinValue }
    }
    $other = 0
    $batches = 0
    # the runtime and IOP libraries
    foreach ($lib in @(@($rt, "ps2xRuntime\ps2_runtime.dir"), @($iop, "ps2xIOP\ps2_iop.dir"))) {
        $dir = Join-Path $Build "$($lib[1])\$Config"
        foreach ($src in [IO.Directory]::EnumerateFiles((Join-Path $lib[0] "src"), "*.c*", [IO.SearchOption]::AllDirectories)) {
            if ($src -notmatch '\.(c|cc|cpp)$') { continue }
            $o = ObjTime $dir $src
            if ($o -eq [datetime]::MinValue -and (Test-Path $dir)) { continue }   # (not part of this build)
            if ($o -lt (Newest $src)) { $other++ }
        }
    }
    # the game: batches of generated code (each generated file starts with the same includes: one
    # stands for all), then the game's own sources
    $dir = Join-Path $Build "transformers\TransformersRecomp.dir\$Config"
    $sample = [IO.Directory]::EnumerateFiles($gen, "*_0x*.cpp") | Select-Object -First 1
    $genHeaders = [datetime]::MinValue
    if ($sample) {
        $head = ([IO.File]::ReadLines($sample) | Select-Object -First 40) -join "`n"
        foreach ($m in $includeRe.Matches($head)) {
            foreach ($d in $incDirs) {
                $f = [IO.Path]::Combine($d, $m.Groups[1].Value)
                if ([IO.File]::Exists($f)) { $n = Newest $f; if ($n -gt $genHeaders) { $genHeaders = $n }; break }
            }
        }
    }
    # (the times of all ~22,000 generated files from one listing of their folder, and of the objects)
    $genTime = @{}
    foreach ($fi in [IO.DirectoryInfo]::new($gen).EnumerateFiles("*.cpp")) { $genTime[$fi.Name] = $fi.LastWriteTimeUtc }
    $objs = @{}
    if (Test-Path $dir) { foreach ($fi in [IO.DirectoryInfo]::new($dir).EnumerateFiles("*.obj")) { $objs[$fi.Name] = $fi.LastWriteTimeUtc } }
    foreach ($fi in [IO.DirectoryInfo]::new((Join-Path $Work "batches")).EnumerateFiles("batch_*.cpp")) {
        $o = $objs[$fi.BaseName + ".obj"]
        if ($null -eq $o -or $o -lt $genHeaders -or $o -lt $fi.LastWriteTimeUtc) { $batches++; continue }
        foreach ($line in [IO.File]::ReadLines($fi.FullName)) {
            $i = $line.LastIndexOf('/')
            if ($i -lt 0) { continue }
            $t = $genTime[$line.Substring($i + 1).TrimEnd('"')]
            if ($null -eq $t -or $t -gt $o) { $batches++; break }
        }
    }
    $sources = @(Join-Path $gen "register_functions.cpp")
    foreach ($d in "src", "decomp", "native_render") {
        if (Test-Path (Join-Path $tf $d)) { $sources += [IO.Directory]::EnumerateFiles((Join-Path $tf $d), "*.c*", [IO.SearchOption]::AllDirectories) | Where-Object { $_ -match '\.(c|cc|cpp)$' } }
    }
    foreach ($src in $sources) {
        if ([IO.File]::Exists($src) -and (ObjTime $dir $src) -lt (Newest $src)) { $other++ }
    }
    # updates.cpp: the version header (written during the build) names the commit
    $header = Join-Path $Build "transformers\tf_version\tf_version.h"
    $ErrorActionPreference = "Continue"
    $commit = (& git -C $Root rev-parse HEAD 2>$null | Out-String).Trim()
    if ($commit -and (Test-Path $header) -and -not (Select-String -LiteralPath $header -SimpleMatch $commit -Quiet)) { $other++ }
    [pscustomobject]@{ Batches = $batches; Other = $other; Total = $batches + $other }
}

# (configured at every build, as the tools above: a new source file - the game's or a generated
# batch - must be in the project before this build, the build's own check adds it only to the next)
Step "Configuring game build" "configure"
Run cmake ($cmakeArgs + (Get-DepDefines $Root "game" $Build))
Write-CompilerReport $Build "The game is"

$forecast = $null
try { $forecast = Get-CompileForecast } catch {}
# Seconds per compiled file on this PC (the record of earlier builds; else a middling 8-core PC's):
# a batch of generated code is the heavy kind, the game's and the runtime's own files the light.
$perBatch = if ($History.ContainsKey("compile_batch")) { [double]$History["compile_batch"] } else { 3.0 }
$perOther = if ($History.ContainsKey("compile_other")) { [double]$History["compile_other"] } else { 0.8 }
$linkTime = if ($History.ContainsKey("link")) { [double]$History["link"] } else { 30.0 }
function Expected([int]$b, [int]$o) { $b * $perBatch + $o * $perOther }
$title = "Building TransformersRecomp ($Config)"
if ($null -ne $forecast) {
    $title += if ($forecast.Total -eq 0) { " - nothing to compile" } else { " - about $($forecast.Total) file(s) to compile, $(Format-Estimate ((Expected $forecast.Batches $forecast.Other) + $linkTime))" }
} else { $title += " - the first build takes a while" }
Step $title "compile"
# MSBuild names each source as it compiles it: those lines are counted (and kept out of the
# output) and turned into a progress line every few seconds.
$compiled = 0
$compiledBatches = 0
$lastCompileAt = $null
$lastShown = -100.0
$buildStart = $Clock.Elapsed.TotalSeconds
Remove-EmptyBuildFiles $Build    # (TODO 378)
Invoke-HealingBuild (@("--build", $Build, "--config", $Config, "--target", "TransformersRecomp", "tfa_launcher", "--") + $BuildParallel + @("/v:m", "/p:UseMultiToolTask=true")) $Build {
    param($line)
    if ($line -match '^\s+([\w.+-]+)\.(c|cc|cpp|cxx)\s*$') {
        $script:compiled++
        if ($Matches[1].StartsWith("batch_")) { $script:compiledBatches++ }
        $now = $Clock.Elapsed.TotalSeconds
        $script:lastCompileAt = $now
        if ($now - $script:lastShown -ge 10 -or $script:compiled -eq 1) {
            $script:lastShown = $now
            $progressLine = if ($forecast -and $script:compiled -lt $forecast.Total) {
                # what is left at the record's rates, corrected by how this build is going so far
                $left = Expected ([Math]::Max(0, $forecast.Batches - $script:compiledBatches)) ([Math]::Max(0, $forecast.Other - ($script:compiled - $script:compiledBatches)))
                $done = Expected $script:compiledBatches ($script:compiled - $script:compiledBatches)
                if ($script:compiled -ge 8 -and $done -gt 0) { $left *= [Math]::Min(4.0, [Math]::Max(0.25, ($now - $buildStart) / $done)) }
                "    compiling the game: $script:compiled of about $($forecast.Total) files, $(Format-Estimate ($left + $linkTime)) left"
            } elseif ($forecast) {
                # (MSBuild names a file when it starts it: the last ones are still compiling)
                "    compiling the game: $script:compiled files started, finishing them and linking"
            } else {
                "    compiling the game: $script:compiled file(s) so far"
            }
            Write-StepLog "build: $progressLine"
            $progressLine
        }
    } else { $line }
}    # (a failed build: Invoke-HealingBuild throws)
Close-Stage
$end = $Clock.Elapsed.TotalSeconds
$compileSeconds = if ($lastCompileAt) { $lastCompileAt - $buildStart } else { 0 }
$linkSeconds = $end - $(if ($lastCompileAt) { $lastCompileAt } else { $buildStart })

# 5b. The native renderer's shaders built into the executable (nr_d3d.cpp, cache::loadBaked): the executable just linked
# compiles them all (--bake-shaders: no window, no device, no game) into the table CMake gave it empty; when that table
# changed - the first build, or a changed shader - the renderer's file is compiled again and linked (~1 min). So no
# start compiles them, the first one on a PC included.
$builtExe = Join-Path $Build "transformers\$Config\TransformersRecomp.exe"
Assert-NotBlocked $builtExe
$bakedTable = Join-Path $Build "transformers\baked\nr_shaders_baked.inc"
# (The shaders live in nr_d3d.cpp: when that file is as at the last bake and the table holds shaders, the bake - ~45 s
# on a small PC - is left out. bake-input.txt records the file's hash.)
$bakeSource = Join-Path $Root "transformers\native_render\nr_d3d.cpp"
$bakeRecord = Join-Path $Build "transformers\baked\bake-input.txt"
$bakeInput = (Get-FileHash -LiteralPath $bakeSource -Algorithm SHA256).Hash
$bakeDone = (Test-Path -LiteralPath $bakeRecord) -and (Get-Content -LiteralPath $bakeRecord -Raw).Trim() -eq $bakeInput -and
            (Test-Path -LiteralPath $bakedTable) -and (Select-String -LiteralPath $bakedTable -SimpleMatch "kBaked0[]" -Quiet)
if ((Test-Path -LiteralPath $builtExe) -and $bakeDone) {
    Write-Host "  (the renderer's shaders are built in as before)"
} elseif (Test-Path -LiteralPath $builtExe) {
    Step "Building the renderer's shaders into the game" "shaders"
    $ErrorActionPreference = "Continue"
    $bakeText = ""
    try { & $builtExe --bake-shaders $bakedTable 2>&1 | ForEach-Object { "  $_" } } catch { $bakeText = "$_" }
    $baked = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if (Test-AntivirusBlock $builtExe $baked $bakeText) { throw (Get-AntivirusBlockMessage $builtExe) }
    if ($baked -eq 2) {
        Run cmake (@("--build", $Build, "--config", $Config, "--target", "TransformersRecomp", "--") + $BuildParallel + @("/v:m", "/p:UseMultiToolTask=true")) | Out-Null
    }
    if ($baked -eq 0 -or $baked -eq 2) {
        Set-Content -LiteralPath $bakeRecord -Value $bakeInput -NoNewline
    } else {
        Write-Host "  warning: the shaders could not be built in; the game compiles them at its first start instead" -ForegroundColor Yellow
    }
    Close-Stage
}

# --- The install's folders (TODO 279, docs/INSTALL_LAYOUT_PLAN.md) ---------------------------------
# In a player's install (tfa-setup.exe in the folder above): the launcher "Transformers Armada.exe" put beside the setup
# program, and the player's things moved out of the checkout once (tools\move_layout.ps1: the backup first, saves
# copied and checked, nothing deleted). Never in a developer's checkout (no tfa-setup.exe above it). Every install
# moves, RecompLauncher's too (recomplauncher.json names the new places) and one built without the mods (a European
# disc on the mods' repository builds as -Public): the new layout is the standard, with no way back. A failure here
# leaves the game as it was built: it never fails the build.
$installRoot = Get-InstallRoot $Root
if ($installRoot) {
    $launcher = Join-Path $Build "transformers\$Config\Transformers Armada.exe"
    if (Test-Path -LiteralPath $launcher) {
        $dest = Join-Path $installRoot "Transformers Armada.exe"
        try {
            if (-not (Test-Path -LiteralPath $dest) -or (Get-FileHash -LiteralPath $dest).Hash -ne (Get-FileHash -LiteralPath $launcher).Hash) {
                Copy-Item -LiteralPath $launcher -Destination $dest -Force
                Write-Host "  The launcher: $dest"
            }
        } catch { Write-Host "  warning: the launcher was not copied to $dest ($($_.Exception.Message)); Play.bat and tfa-setup.exe start the game as before" -ForegroundColor Yellow }
    }
    # (it decides itself: once, or an interrupted move carried on)
    & (Join-Path $Root "tools\move_layout.ps1") -GameFolder $Root -Root $installRoot
}

# --- The record for the next estimates, and the summary ---------------------------------------
foreach ($k in $Spent.Keys) { if ($k -ne "compile") { $History[$k] = [Math]::Round($Spent[$k], 1) } }
# (the rates: from a build with enough of one kind, the other kind's share taken at its old rate)
$others = $compiled - $compiledBatches
if ($compiledBatches -ge 20) {
    $History["compile_batch"] = [Math]::Round([Math]::Max(0.2, ($compileSeconds - $others * $perOther) / $compiledBatches), 2)
} elseif ($others -ge 8 -and $compiledBatches -eq 0) {
    $History["compile_other"] = [Math]::Round([Math]::Max(0.1, $compileSeconds / $others), 2)
}
$History.Remove("compile_per_file")
# (the time after the last file started: the link, when that file was a light one - after a batch
# of generated code it is that batch's own compile too)
if ($compiled -gt 0 -and $compiledBatches -eq 0) { $History["link"] = [Math]::Round($linkSeconds, 1) }
try {
    New-Item -ItemType Directory -Force (Split-Path $TimesFile) | Out-Null
    [pscustomobject]$History | ConvertTo-Json | Set-Content $TimesFile -Encoding ASCII
} catch {}
$names = [ordered]@{ fetch = "PS2Recomp"; patches = "patches"; extract = "the disc"; tools = "recompiler tools"; check = "checks";
                     scan = "SDK scan"; functions = "functions"; translate = "translation"; sync = "generated files"; configure = "configure"; shaders = "shaders" }
$parts = @()
foreach ($k in $Spent.Keys) {
    if ($k -eq "compile" -or $Spent[$k] -lt 1) { continue }
    $parts += "$($names[$k]) $(Format-Duration $Spent[$k])"
}
if ($compiled -gt 0) {
    $parts += "compiling $compiled file(s) and linking $(Format-Duration ($end - $buildStart))"
} else {
    $parts += "compile check $(Format-Duration ($end - $buildStart))"
}

$exe = Join-Path $Build "transformers\$Config\TransformersRecomp.exe"
Write-Host "`nDone: $exe" -ForegroundColor Green
Write-Host "Built in $(Format-Duration $Clock.Elapsed.TotalSeconds): $($parts -join ', ')." -ForegroundColor Green
Write-StepLog "build: built in $(Format-Duration $Clock.Elapsed.TotalSeconds)"
