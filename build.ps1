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
$Game = Join-Path $Root "game"
$Work = Join-Path $Root "work"
$Build = Join-Path $Root $(if ($Public) { "out\build-public" } else { "out\build" })
$ToolsBuild = Join-Path $Root "out\tools"
$Tools = Join-Path $Root "transformers\tools"
$ElfName = "SLUS_206.68"

# --- Timing: each step's time, a summary at the end, the record used for the next estimates -------
$Clock = [Diagnostics.Stopwatch]::StartNew()
$script:stage = $null
$script:stageStart = 0.0
$Spent = [ordered]@{}       # stage -> seconds, this build
$TimesFile = Join-Path $Root "out\build-times.json"
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
# (a stage's time from earlier builds on this PC: "(usually about 2 min here)")
function Usual([string]$stage) {
    if ($History.ContainsKey($stage) -and [double]$History[$stage] -ge 20) { return " (usually $(Format-Estimate ([double]$History[$stage])) here)" }
    return ""
}

function Run {
    param([string]$Exe, [string[]]$Arguments)
    # Windows PowerShell turns any stderr line from a native tool into an
    # error record; judge success by the exit code only.
    $ErrorActionPreference = "Continue"
    & $Exe @Arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}
function RunLogged {
    param([string]$Exe, [string[]]$Arguments, [string]$Log)
    # The output goes into the log file directly, not through PowerShell: the analyzer writes ~9 MB
    # of lines, which PowerShell 5 passes one by one - with update.ps1's transcript running, that
    # alone took ~80 s instead of ~8 (TODO 115). Its error output is added at the end.
    $quoted = @($Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } })
    $err = "$Log.stderr"
    $p = Start-Process -FilePath $Exe -ArgumentList $quoted -WorkingDirectory (Get-Location).ProviderPath -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $Log -RedirectStandardError $err
    if (Test-Path -LiteralPath $err) {
        $bytes = [IO.File]::ReadAllBytes($err)
        if ($bytes.Length) { $f = [IO.File]::Open($Log, [IO.FileMode]::Append); try { $f.Write($bytes, 0, $bytes.Length) } finally { $f.Dispose() } }
        Remove-Item -LiteralPath $err -ErrorAction SilentlyContinue
    }
    if ($p.ExitCode -ne 0) { throw "$(Split-Path $Exe -Leaf) failed with exit code $($p.ExitCode), see $Log" }
}

foreach ($tool in "cmake", "python", "git") {
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
if ($patches -and $recorded -ne $series) {
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
}

# 1. Game data -------------------------------------------------------------
if (-not (Test-Path (Join-Path $Game $ElfName))) {
    if (-not $Iso) {
        $found = Get-ChildItem $Root -Filter *.iso -File | Select-Object -First 1
        if (-not $found) { throw "No game data. Pass your disc image: .\build.ps1 -Iso `"Transformers (USA).iso`"" }
        $Iso = $found.FullName
    }
    Step "Extracting $Iso" "extract"
    Run python @((Join-Path $Tools "extract_iso.py"), $Iso, $Game)
}

# 2. Tools -----------------------------------------------------------------
$genArgs = @("-A", "x64")
if ($Generator) { $genArgs += @("-G", $Generator) }
$cmakeArgs = @("-S", $Root, "-B", $Build) + $genArgs
if ($Public) { $cmakeArgs += "-DTF_OVERLAY_DIR=" } # (the overlay left out: transformers\CMakeLists.txt)

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
                 "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF") + $genArgs)
    Run cmake @("--build", $ToolsBuild, "--config", $Config, "--target", "ps2_recomp", "ps2_analyzer", "--", "/m", "/v:m")

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
                Run python @((Join-Path $Tools "make_config.py"), $Work, $probe) | Out-Null
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
                RunLogged (Join-Path $ToolsBuild "ps2xAnalyzer\$Config\ps2_analyzer.exe") @($ElfName, "analyzer.toml") (Join-Path $Work "analyzer.log")
                Set-Content -Path $scanFile -Value $scanText -NoNewline
            }
            $configText = Get-ConfigText
            if ((Test-Record $configFile $configText) -and -not (@("functions.csv", "jump_tables.json", "transformers.toml") | Where-Object { -not (Test-Path (Join-Path $Work $_)) })) {
                Write-Host "  (the function list and the recompiler's config are as before)"
            } else {
                Remove-Item $configFile -ErrorAction SilentlyContinue
                Step "Recovering function boundaries$(Usual 'functions')" "functions"
                Run python @((Join-Path $Tools "find_functions.py"), $ElfName, "functions.csv", "--names", "analyzer.toml", "--jump-tables", "jump_tables.json")
                Run python @((Join-Path $Tools "make_config.py"), $Work, "transformers.toml")
                Set-Content -Path $configFile -Value $configText -NoNewline
            }

            Step "Translating MIPS to C++$(Usual 'translate')" "translate"
            if (Test-Path staging) { Remove-Item -Recurse -Force staging }
            RunLogged (Join-Path $ToolsBuild "ps2xRecomp\$Config\ps2_recomp.exe") @("transformers.toml") (Join-Path $Work "recomp.log")

            # Install only what changed, so the compile below is incremental.
            Step "Updating generated sources" "sync"
            Run python @((Join-Path $Tools "sync_generated.py"), "staging", "generated", "batches")
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
Run cmake $cmakeArgs

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
$ErrorActionPreference = "Continue"
& cmake @("--build", $Build, "--config", $Config, "--target", "TransformersRecomp", "--", "/m", "/v:m", "/p:UseMultiToolTask=true") 2>&1 | ForEach-Object {
    $line = "$_"
    if ($line -match '^\s+([\w.+-]+)\.(c|cc|cpp|cxx)\s*$') {
        $compiled++
        if ($Matches[1].StartsWith("batch_")) { $compiledBatches++ }
        $now = $Clock.Elapsed.TotalSeconds
        $lastCompileAt = $now
        if ($now - $lastShown -ge 10 -or $compiled -eq 1) {
            $lastShown = $now
            if ($forecast -and $compiled -lt $forecast.Total) {
                # what is left at the record's rates, corrected by how this build is going so far
                $left = Expected ([Math]::Max(0, $forecast.Batches - $compiledBatches)) ([Math]::Max(0, $forecast.Other - ($compiled - $compiledBatches)))
                $done = Expected $compiledBatches ($compiled - $compiledBatches)
                if ($compiled -ge 8 -and $done -gt 0) { $left *= [Math]::Min(4.0, [Math]::Max(0.25, ($now - $buildStart) / $done)) }
                "    compiling the game: $compiled of about $($forecast.Total) files, $(Format-Estimate ($left + $linkTime)) left"
            } elseif ($forecast) {
                # (MSBuild names a file when it starts it: the last ones are still compiling)
                "    compiling the game: $compiled files started, finishing them and linking"
            } else {
                "    compiling the game: $compiled file(s) so far"
            }
        }
    } else { $line }
}
$code = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($code -ne 0) { throw "cmake --build failed with exit code $code" }
Close-Stage
$end = $Clock.Elapsed.TotalSeconds
$compileSeconds = if ($lastCompileAt) { $lastCompileAt - $buildStart } else { 0 }
$linkSeconds = $end - $(if ($lastCompileAt) { $lastCompileAt } else { $buildStart })

# 5b. The native renderer's shaders built into the executable (nr_d3d.cpp, cache::loadBaked): the executable just linked
# compiles them all (--bake-shaders: no window, no device, no game) into the table CMake gave it empty; when that table
# changed - the first build, or a changed shader - the renderer's file is compiled again and linked (~1 min). So no
# start compiles them, the first one on a PC included.
$builtExe = Join-Path $Build "transformers\$Config\TransformersRecomp.exe"
$bakedTable = Join-Path $Build "transformers\baked\nr_shaders_baked.inc"
if (Test-Path -LiteralPath $builtExe) {
    Step "Building the renderer's shaders into the game" "shaders"
    $ErrorActionPreference = "Continue"
    & $builtExe --bake-shaders $bakedTable 2>&1 | ForEach-Object { "  $_" }
    $baked = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($baked -eq 2) {
        Run cmake @("--build", $Build, "--config", $Config, "--target", "TransformersRecomp", "--", "/m", "/v:m", "/p:UseMultiToolTask=true") | Out-Null
    } elseif ($baked -ne 0) {
        Write-Host "  warning: the shaders could not be built in; the game compiles them at its first start instead" -ForegroundColor Yellow
    }
    Close-Stage
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
