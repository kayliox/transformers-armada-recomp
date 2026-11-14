<#
.SYNOPSIS
    Brings your copy up to date with the repository and rebuilds what the new changes need.

.DESCRIPTION
    1. Fetches the repository. If nothing is new it says so and stops (-Force rebuilds anyway).
    2. Pulls the new commits and updates the PS2Recomp submodule; lists what changed. Your own work
       in the copy (commits of your own, changed files, new files that are not git-ignored) would
       stop the pull: it is first committed, all of it, to a branch of its own named like
       my-changes-2026-11-01-1430 ("git log my-changes-..." shows it, "git switch my-changes-..."
       brings it back), then the pull goes on. Ignored files (game data, builds, settings.ini,
       saves, Mods\) are never touched. If that branch cannot be made, nothing is changed and the
       update stops, saying why.
    3. Rebuilds only what the changes need:
       - the game, the renderer, the runtime patches  -> build.ps1
         (it translates the game's code again only when the recompiler itself changed, and
         compiles only the changed files; it shows its progress and an estimate of the time left)
       - the HD texture tools                         -> make_hd_textures.ps1
         (it only does what is missing, so usually seconds)
       Documents, the setup program (installer\) and these scripts need no rebuild.
    Uses the disc path setup.ps1 remembered (.setup.json). Not while the game runs: close it first.
    Your settings (settings.ini) and saves are git-ignored: a pull never touches them.

    -Root and -Hooks are for a repository that holds this folder as a subfolder (see setup.ps1):
    its Invoke-UpdateHook gets what changed and rebuilds its own parts.

    The game's own "Update now" (the F1 menu, About) runs this with -WaitForPid (the game closes
    first) and -Play (the game starts again afterwards, where it was: TODO 104). Everything it prints
    also goes to update.log in this folder.

.EXAMPLE
    .\update.ps1
.EXAMPLE
    .\update.ps1 -CheckOnly      # only says whether there is something new
#>
param(
    [switch]$Force,
    [switch]$CheckOnly,
    [int]$WaitForPid = 0,          # the game that started the update: waited for until it has closed
    [switch]$Play,                 # the game started again at the end (also after a failure: the old one)
    [switch]$StashLocalChanges,    # (kept for older callers: your own work is always kept on a branch now)
    [string]$Binaries,             # a folder of prebuilt binaries for the new commit, instead of building
    [string]$Root,                 # the git repository's top (default: this folder)
    [string]$Hooks,                # a script with extra steps (Invoke-UpdateHook), dot-sourced
    [hashtable]$HookArgs = @{}     # passed to the hooks as $ctx.Args
)

$ErrorActionPreference = "Stop"
$Port = $PSScriptRoot
$Repo = if ($Root) { (Resolve-Path $Root).Path } else { $Port }   # (the git repository: this folder, or one holding it)
$StateFile = Join-Path $Repo ".setup.json"
$States = Join-Path $Port "states"
$ResultFile = Join-Path $States "update_result.txt"   # (the game reads it at its next start, updates.cpp)
$clock = [Diagnostics.Stopwatch]::StartNew()
# Each part's time: the summary at the end says what the update spent its time on (TODO 115)
$parts = [ordered]@{}
$script:part = $null
$script:partStart = 0.0
function Step($msg, [string]$part) {
    if ($part) {
        if ($script:part) { $parts[$script:part] += $clock.Elapsed.TotalSeconds - $script:partStart }
        if (-not $parts.Contains($part)) { $parts[$part] = 0.0 }
        $script:part = $part
        $script:partStart = $clock.Elapsed.TotalSeconds
    }
    Write-Host "`n==> $msg" -ForegroundColor Cyan
    if (Get-Command Write-StepLog -ErrorAction SilentlyContinue) { Write-StepLog "update: $msg" }
}
function Format-Time([double]$seconds) {
    $sec = [int][Math]::Round($seconds)
    if ($sec -lt 60) { "$sec s" } else { "{0} min {1} s" -f [int][Math]::Floor($sec / 60), ($sec % 60) }
}
function Result([string]$text) {
    if (-not $Play) { return }
    New-Item -ItemType Directory -Force $States | Out-Null
    Set-Content -Path $ResultFile -Value $text -Encoding ASCII
}
# (TODO 371) Windows 11's Smart App Control refusing a program: it lets run only what is signed or what Microsoft's cloud
# already knows, and the game and its tools are built on this PC (new to it). PowerShell: "An Application Control policy
# has blocked this file"; a program whose library was refused: 0xC0E90002; Win32 4551.
function Test-SmartAppBlock($Code, [string]$Text) {
    if ($Code -in -1058471934, 3236495362, 4551) { return $true }
    return ($Text -match "Application Control policy has blocked|0xc0e90002")
}
function Get-SmartAppBlockMessage([string]$What) {
    "Windows' Smart App Control blocked $What. It lets only programs Microsoft already knows run, and this game is built " +
    "on your PC, so its files are new to it. Nothing is damaged. To see every file it blocked: Event Viewer > Applications " +
    "and Services Logs > Microsoft > Windows > CodeIntegrity > Operational, event 3077. To play, turn it off: Windows " +
    "Security > App & browser control > Smart App Control settings > Off (on some Windows 11 versions it cannot be turned " +
    "back on without resetting Windows)."
}
function Start-Game {
    if (-not $Play) { return }
    Step "Starting the game"
    $exe = Join-Path $Port "out\build\transformers\RelWithDebInfo\TransformersRecomp.exe"
    if (-not (Test-Path $exe)) { Write-Host "update: the game's exe is missing ($exe)" -ForegroundColor Red; return }
    $settings = Join-Path $Port "settings.ini"
    if (-not (Test-Path $settings) -and (Test-Path (Join-Path $Port "settings.example.ini"))) {
        Copy-Item (Join-Path $Port "settings.example.ini") $settings
    }
    if ($env:TF_UPDATE_GAME_LOG) {
        # (tests: the restarted game's log, as the test runner keeps the first one's)
        Start-Process -FilePath $exe -WorkingDirectory $Port -WindowStyle Hidden `
            -RedirectStandardError $env:TF_UPDATE_GAME_LOG -RedirectStandardOutput "$($env:TF_UPDATE_GAME_LOG).out"
    } else {
        # (TODO 371) Windows 11's Smart App Control refusing the game built here: said plainly, not as an "unexpected error"
        try { Start-Process -FilePath $exe -WorkingDirectory $Port }
        catch {
            if (-not (Test-SmartAppBlock 0 "$($_.Exception.Message)")) { throw }
            $text = Get-SmartAppBlockMessage "the game (TransformersRecomp.exe)"
            Write-Host "`nupdate: $text" -ForegroundColor Red
            if (Get-Command Write-StepLog -ErrorAction SilentlyContinue) { Write-StepLog "update: the game was blocked by Smart App Control" }
            # (the update itself is done: its result stays "ok", or the game would say it did not finish. Nothing of the
            # game shows now, so a message box says it - also when this run has no window of its own.)
            try {
                Add-Type -AssemblyName PresentationFramework
                [System.Windows.MessageBox]::Show($text, "Transformers Armada: Windows blocked the game", "OK", "Warning") | Out-Null
            } catch {}
        }
    }
}
# git in the copy: its exit code and its output (stderr too), never an exception.
function Invoke-Git {
    $a = $args
    $eap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out = & git -C $Repo -c core.longpaths=true @a 2>&1 | ForEach-Object { "$_" }
        [pscustomobject]@{ Ok = ($LASTEXITCODE -eq 0); Out = (($out -join "`n").Trim()) }
    } finally { $ErrorActionPreference = $eap }
}
# What the player reads after their own work was kept (here, update.log, the setup, the game).
function Kept-Text([string]$name) { "Your own changes were kept on the branch $name; nothing was lost." }
# Your own work kept on a branch (see 2. above): local commits, changed files, new files that are not
# ignored are committed to my-changes-<date>-<time>; the copy's branch goes back to where it and the
# repository part, and the merge below moves it on. Returns the branch's name, "" when there was
# nothing to keep. Nothing that is not on that branch first is touched (it is checked before
# anything moves); if the branch cannot be made, nothing is changed and the update stops.
function Save-LocalWork([string]$branch) {
    $theirs = (Invoke-Git rev-parse --verify "origin/$branch").Out
    $head = (Invoke-Git rev-parse HEAD).Out
    $onBranch = (Invoke-Git rev-parse --abbrev-ref HEAD).Out -eq $branch
    # (nothing new to merge: the merge is a no-op, your work is not in its way)
    if ((Invoke-Git merge-base --is-ancestor $theirs HEAD).Ok) { return "" }
    $commits = [int](Invoke-Git rev-list --count "$theirs..HEAD").Out
    # (update.log: this script's own log; the PS2Recomp submodule always has the port's patches applied)
    $changes = Invoke-Git status --porcelain --ignore-submodules=dirty -- . ":(exclude)update.log"
    if (-not $changes.Ok) { Fail "git status failed: $($changes.Out)" }
    if ($commits -eq 0 -and -not $changes.Out -and $onBranch) { return "" }
    if (-not $onBranch) {
        $local = "refs/heads/$branch"
        if ((Invoke-Git rev-parse --verify --quiet $local).Ok -and -not (Invoke-Git merge-base --is-ancestor $local $theirs).Ok -and
            -not (Invoke-Git merge-base --is-ancestor $local HEAD).Ok) {
            Fail "your copy is not on its branch $branch, and that branch has commits of its own: put it back on $branch (git switch $branch), then update again"
        }
    }
    $base = Invoke-Git merge-base HEAD $theirs
    if (-not $base.Ok) { Fail "your copy shares no history with origin/$($branch): cannot update it" }
    # Ignored files the update would write over (a file of yours with the name of a new one): stop.
    foreach ($path in ((Invoke-Git diff --name-only --diff-filter=A --no-renames $head $theirs).Out -split "`n" | Where-Object { $_ })) {
        if ((Test-Path -LiteralPath (Join-Path $Repo $path)) -and (Invoke-Git check-ignore -q --no-index $path).Ok) {
            Fail "the update brings $path, and your copy has a file of its own there (an ignored one): move it away, then update again"
        }
    }
    $stamp = "my-changes-" + (Get-Date -Format "yyyy-MM-dd-HHmm")
    $name = $stamp
    for ($n = 2; (Invoke-Git rev-parse --verify --quiet "refs/heads/$name").Ok; $n++) { $name = "$stamp-$n" }
    $files = @($changes.Out -split "`n" | Where-Object { $_ })
    Write-Host "Your own work in this copy ($commits commit(s) of your own, $($files.Count) changed or new file(s)) is kept on the branch $($name):"
    $files | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
    # 1. The backup commit, made beside the copy (in a temporary index): the folder, its index and
    #    its branch are not touched until a branch holding all of it exists.
    $kept = $head
    if ($files.Count) {
        $temp = Join-Path $Repo (Invoke-Git rev-parse --git-path tfa-keep.index).Out
        $index = Join-Path $Repo (Invoke-Git rev-parse --git-path index).Out
        if (Test-Path -LiteralPath $index) { Copy-Item -LiteralPath $index $temp -Force }   # (none yet: git starts an empty one)
        $why = ""
        $env:GIT_INDEX_FILE = $temp
        try {
            $r = Invoke-Git add -A
            if (-not $r.Ok) { $why = "git add: $($r.Out)" }
            if (-not $why) {
                Invoke-Git reset -q -- update.log | Out-Null   # (this script's own log stays out: older copies do not ignore it)
                # (everything in: nothing untracked or unstaged left beside it)
                $left = Invoke-Git status --porcelain --ignore-submodules=dirty -- . ":(exclude)update.log"
                $out = @($left.Out -split "`n" | Where-Object { $_ -and ($_.StartsWith("??") -or $_[1] -ne ' ') })
                if (-not $left.Ok) { $why = "git status: $($left.Out)" } elseif ($out.Count) { $why = "a file left out: $($out[0])" }
            }
            if (-not $why) {
                $tree = Invoke-Git write-tree
                if (-not $tree.Ok) { $why = "git write-tree: $($tree.Out)" }
            }
        } finally { Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue; Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue }
        if (-not $why) {
            # (a commit needs a name: yours when git has one)
            $who = if ((Invoke-Git config user.email).Out) { @() } else { @("-c", "user.name=Transformers setup", "-c", "user.email=setup@transformers.local") }
            $c = Invoke-Git @who commit-tree (($tree.Out -split "`n")[-1]) -p $head -m "My own changes, kept before an update"
            if ($c.Ok) { $kept = ($c.Out -split "`n")[-1] } else { $why = "git commit-tree: $($c.Out)" }
        }
        if ($why) { Fail "your own changes could not be kept on a branch ($why): nothing was changed" }
    }
    $r = Invoke-Git branch $name $kept
    if (-not $r.Ok) { Fail "your own changes could not be kept on a branch (git branch: $($r.Out)): nothing was changed" }
    # From here on all of it is on the branch: a step that fails stops the update, nothing is lost.
    # 2. The copy's branch back to where it and the repository part (the merge below moves it on);
    #    its changed files go back to that version (yours are on the branch).
    $r = Invoke-Git checkout -q -f -B $branch $base.Out
    if (-not $r.Ok) { Fail "$(Kept-Text $name) But the update could not go on (git checkout: $($r.Out)): it stops here" }
    # 3. The new files out of the way, one by one: exactly the ones on the branch (never a folder,
    #    never an ignored file).
    $untracked = Invoke-Git status --porcelain -z -uall --ignore-submodules=dirty -- . ":(exclude)update.log"
    foreach ($entry in ($untracked.Out -split "`0")) {
        if (-not $entry.StartsWith("?? ")) { continue }
        $path = $entry.Substring(3)
        if (-not (Invoke-Git cat-file -e "$($kept):$path").Ok) { Fail "$(Kept-Text $name) But $path is not on that branch, so it is left as it is and the update stops here" }
        try { Remove-Item -LiteralPath (Join-Path $Repo $path) -Force -ErrorAction Stop }
        catch { Fail "$(Kept-Text $name) But the update could not go on ($($path): $($_.Exception.Message)): it stops here" }
    }
    Write-Host (Kept-Text $name) -ForegroundColor Green
    # (the game says so too at its next start: updates.cpp reads and deletes this file)
    New-Item -ItemType Directory -Force $States | Out-Null
    Set-Content -Path (Join-Path $States "update_kept.txt") -Value $name -Encoding ASCII
    return $name
}
function Fail($msg) {
    Write-Host "`nupdate: $msg" -ForegroundColor Red
    if (Get-Command Write-StepLog -ErrorAction SilentlyContinue) { Write-StepLog "update: stopped: $msg" }
    Result "failed $msg"
    Start-Game
    # (the console window the game opened: a moment to read why; not in tfa-setup's hidden run)
    if ($Play -and -not $env:TF_UPDATE_HIDDEN -and -not $env:TFA_UPDATE_NO_PAUSE) { Write-Host "`n(This window closes in 30 seconds.)"; Start-Sleep -Seconds 30 }
    try { Stop-Transcript | Out-Null } catch {}
    exit 1
}
# (anything unexpected ends like a failure: the result written, the game started again - never a
# player left without their game)
trap {
    # (TODO 371: a program Windows refused, named where PowerShell names it)
    if (Test-SmartAppBlock 0 "$($_.Exception.Message)") {
        $file = "$($_.InvocationInfo.BoundParameters['FilePath'])"
        $m = [regex]::Match("$($_.Exception.Message)", "Program '([^']+)'")
        if ($m.Success) { $file = $m.Groups[1].Value }
        Fail (Get-SmartAppBlockMessage $(if ($file) { Split-Path $file -Leaf } else { "a program the update runs" }))
    }
    if ("$($_.Exception.Message)".StartsWith("Windows' Smart App Control blocked")) { Fail $_.Exception.Message }  # (build.ps1's words)
    Fail ("unexpected error: " + $_.Exception.Message)
}
# (the shared helpers: real-Python detection, probes, git long paths)
. (Join-Path $Port "tools\setup_common.ps1")
if ($Hooks) { . $Hooks }
$pre = Get-PortPrefix $Repo $Port   # ("" in a clone of the game's repository; "port/" when one holds this folder)
# (git never asks for anything in a window without anyone to answer: the game's update is unattended)
if ($Play) { $env:GIT_TERMINAL_PROMPT = "0"; $env:GCM_INTERACTIVE = "never" }
# (TODO 279: the install's logs\ when its folders were moved; update.log in the checkout before that)
$UpdateLog = Join-Path $Repo "update.log"
if (Test-Path -LiteralPath (Join-Path $Port "tools\install_layout.ps1")) {
    . (Join-Path $Port "tools\install_layout.ps1")
    $logs = Get-LayoutLogsDir $Port
    if ($logs) { $UpdateLog = Join-Path $logs "update.log" }
}
try { Start-Transcript -Path $UpdateLog -Force | Out-Null } catch {}
# (TODO 310) Each step with its time, written straight to the disk (tools\step_log.ps1): update_steps.log beside
# update.log, unless the setup window set TF_STEP_LOG beside its own log.
if (Test-Path -LiteralPath (Join-Path $Port "tools\step_log.ps1")) {
    . (Join-Path $Port "tools\step_log.ps1")
    if (-not $env:TF_STEP_LOG) {
        $env:TF_STEP_LOG = Join-Path (Split-Path $UpdateLog) "update_steps.log"
        try { Set-Content -LiteralPath $env:TF_STEP_LOG -Value $null } catch {}
    }
}

if ($WaitForPid -gt 0) {
    $game = Get-Process -Id $WaitForPid -ErrorAction SilentlyContinue
    if ($game) {
        Step "Waiting for the game to close"
        if (-not $game.WaitForExit(120000)) { Fail "the game did not close (process $WaitForPid)" }
    }
}

if (-not (Test-Path $StateFile)) { Fail "run setup.ps1 first (this copy was never set up)" }
# (an empty or damaged .setup.json - a write cut off - is a warning, not a stop: only its disc path and build mark are read)
$state = $null
try { $state = Get-Content $StateFile -Raw | ConvertFrom-Json } catch {}
if (-not $state) { Write-Host "warning: .setup.json is empty or damaged: it is written again at the end" -ForegroundColor Yellow; $state = [pscustomobject]@{} }
if (-not $Binaries -and $state.PSObject.Properties["Binaries"]) { $Binaries = $state.Binaries }
$Python = Find-Python
if (-not $Python) { Fail "Python 3.11+ not found: run setup.ps1 again" }
Use-Python $Python

Step "Checking the repository for new changes" "checking"
if ((Invoke-Native { & git -C $Repo fetch --quiet origin }) -ne 0) { Fail "git fetch failed (no network, or the repository cannot be reached)" }
$branch = (& git -C $Repo rev-parse --abbrev-ref HEAD).Trim()
$old = (& git -C $Repo rev-parse HEAD).Trim()
$new = (& git -C $Repo rev-parse "origin/$branch").Trim()
$built = if ($state.PSObject.Properties["SetUpAt"]) { $state.SetUpAt } else { "" }
if ($old -eq $new -and $built -eq $old -and -not $Force) {
    Write-Host "Up to date ($($old.Substring(0, 7)))." -ForegroundColor Green
    Result "ok $old"
    Start-Game
    try { Stop-Transcript | Out-Null } catch {}
    exit 0
}
if ($old -ne $new) {
    Write-Host "New commits:"
    & git -C $Repo log --oneline "$old..$new"
}
$kept = ""   # (the branch your own work was kept on, Save-LocalWork)
if ($CheckOnly) { exit 0 }

$running = Get-Process TransformersRecomp -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Port, [StringComparison]::OrdinalIgnoreCase) }
if ($running) { Fail "the game is running: close it, then run update.ps1 again" }

if ($old -ne $new) {
    Step "Pulling" "downloading"
    $kept = Save-LocalWork $branch
    if ((Invoke-Native { & git -C $Repo merge --ff-only --quiet "origin/$branch" }) -ne 0) { Fail ("the new changes could not be brought in (git merge failed)" + $(if ($kept) { ". " + (Kept-Text $kept) } else { "" })) }
    if ((Invoke-Native { & git -C $Repo submodule update --init --recursive }) -ne 0) { Fail "git submodule update failed" }
}

# What changed since the last build (or since the old head when the setup never recorded one).
$since = if ($built) { $built } else { $old }
$changed = if ($Force) { @("${pre}build.ps1", "${pre}tools/hd_textures/") } else { @(& git -C $Repo diff --name-only $since HEAD) }
# (what never needs the game rebuilt: documents, the setup program, the setup scripts, GitHub's files)
$noBuild = @("docs/*", "*.md", "installer/*", ".github/*", ".githooks/*", "setup.ps1", "update.ps1", "Play.bat",
    "tools/setup_common.ps1", "tools/step_log.ps1", "tools/check_no_game_data.py", "tools/check_publishable.py", "LICENSE", ".gitignore", ".gitattributes")
$needBuild = @($changed | Where-Object {
    $path = $_
    $path.StartsWith($pre) -and -not ($noBuild | Where-Object { $path.Substring($pre.Length) -like $_ })
})
$needTextures = @($changed | Where-Object { $_ -like "${pre}tools/hd_textures/*" -or $_ -like "${pre}tools/make_hd_textures.ps1" })
# (PR #20) The HD textures preset chosen (hd_options.json: the setup's update screen, F1 > Graphics) is not the one the
# set was made for: the HD step runs too, and shrinks or makes what the chosen one needs.
try {
    $hdOpt = Get-Content (Join-Path $Port "hd_textures\hd_options.json") -Raw -ErrorAction Stop | ConvertFrom-Json
    $hdSt = Get-Content (Join-Path $Port "hd_textures\upscale_status.json") -Raw -ErrorAction Stop | ConvertFrom-Json
    if ($hdOpt.quality -and $hdSt.quality -and "$($hdOpt.quality)" -ne "$($hdSt.quality)") { $needTextures += "hd_options.json" }
} catch {}

if ($needBuild -and $Binaries) {
    # Prebuilt binaries (a test, or binaries made elsewhere for this very commit): copied instead of a
    # build, only when they say they are this commit's (commit.txt), never an exe of another version.
    Step "Installing the prebuilt game ($Binaries)" "the game"
    $head = (& git -C $Repo rev-parse HEAD).Trim()
    $theirs = if (Test-Path (Join-Path $Binaries "commit.txt")) { (Get-Content (Join-Path $Binaries "commit.txt") -Raw).Trim() } else { "" }
    if ($theirs -ne $head) { Fail "the prebuilt binaries are for $theirs, not for $head" }
    $bin = Join-Path $Port "out\build\transformers\RelWithDebInfo"
    New-Item -ItemType Directory -Force $bin | Out-Null
    Get-ChildItem $Binaries -File | Where-Object { $_.Extension -in ".exe", ".dll", ".pdb" } | Copy-Item -Destination $bin -Force
} elseif ($needBuild) {
    # build.ps1 itself decides whether the game's code must be translated again: only when the
    # recompiler programs (rebuilt only when their own sources change - a PS2Recomp patch for the
    # runtime alone leaves them as they were), the Python tools that run them, the config or the
    # game's ELF changed (TODO 115). Otherwise only the changed files are compiled.
    $buildArgs = @{}
    $buildPs1 = Join-Path $Port "build.ps1"
    if ($Force) {
        # (the translation redone too: its record forgotten)
        Remove-Item (Join-Path $Port "work\*-inputs.txt"), (Join-Path $Port "work\generated\.sync_cache.json") -ErrorAction SilentlyContinue
    } elseif (-not (Select-String -LiteralPath $buildPs1 -SimpleMatch "translation-inputs.txt" -Quiet)) {
        # (a build.ps1 from before that: the changed paths decide, as in TODO 104)
        $needRecompile = $changed | Where-Object {
            $_ -like "${pre}PS2Recomp*" -or $_ -like "${pre}patches/ps2recomp/*" -or $_ -like "${pre}transformers/tools/*" -or
            $_ -like "${pre}transformers/config/*" -or $_ -eq "${pre}build.ps1" }
        if (-not $needRecompile -and (Test-Path (Join-Path $Port "work\generated\register_functions.cpp"))) { $buildArgs.SkipRecompile = $true }
    }
    # (TODO 310) "Go easy on my PC" (hd_textures\hd_options.json "gentle", the setup's choice): half the processor
    # threads at a lower priority (a build.ps1 from before it: as it is)
    $gentle = $false
    try { $gentle = [bool](Get-Content (Join-Path $Port "hd_textures\hd_options.json") -Raw -ErrorAction Stop | ConvertFrom-Json).gentle } catch {}
    if ($gentle -and (Select-String -LiteralPath $buildPs1 -SimpleMatch '[switch]$Gentle' -Quiet)) { $buildArgs.Gentle = $true }
    Step ("Rebuilding the game (build.ps1{0}{1})" -f $(if ($buildArgs.SkipRecompile) { " -SkipRecompile: the game's code is translated as before" } else { "" }),
        $(if ($buildArgs.Gentle) { ", going easy on the PC" } else { "" })) "the game"
    & $buildPs1 @buildArgs
    if (-not $?) { Fail "the build failed (see above)" }
}
if ($needTextures -and (Test-Path (Join-Path $Port "hd_textures"))) {
    # (0.7.0, PR #20: a set made before the presets, or for another preset, is adjusted here - most images shrunk on the
    # processor, a few made on the graphics card; the lines below count them, with the time left)
    Step "Updating the HD textures (adjusting them to your preset; this can take some minutes)" "HD textures"
    # (TODO 124: the first time the colour match is there - its record, colour_gains.json, not made yet - it goes
    # through every level's HD image once, and the renderer compresses the changed ones again on their first use.
    # make_hd_textures.ps1 says the same itself: this update.ps1 runs as it was before the pull.)
    if ((Test-Path (Join-Path $Port "hd_textures\upscaled")) -and
        (Test-Path (Join-Path $Port "tools\hd_textures\match_colour.py")) -and
        -not (Test-Path (Join-Path $Port "hd_textures\upscaled_precolour\colour_gains.json"))) {
        Write-Host "Matching the HD textures' colours to the game's (one time, about 10 minutes)."
        if (Test-Path (Join-Path $Port "hd_textures\upscaled_bc7_cache")) {
            Write-Host "Afterwards the first visit to each level loads a little slower, once: its changed textures are compressed again."
        }
    }
    if (Test-Path (Join-Path $Port "hd_textures\upscaled")) {
        & (Join-Path $Port "tools\make_hd_textures.ps1") -Python $Python
        $hdOk = $?; $hdCode = $LASTEXITCODE
        # (TODO 371) 7: Smart App Control refused a file the HD textures need - said above; the update goes on.
        if ($hdCode -eq 7) { if (Get-Command Write-StepLog -ErrorAction SilentlyContinue) { Write-StepLog "update: HD textures skipped (Smart App Control)" } }
        elseif (-not $hdOk) { Fail "updating the HD textures failed (see above)" }
    } else {
        Invoke-OptionalFonts -Python $Python -Port $Port
    }
}
if (-not (Test-Path (Join-Path $Port "hd_textures"))) {
    Step "Making the button prompts and fonts (no GPU upscale)" "fonts"
    Invoke-OptionalFonts -Python $Python -Port $Port
}
# The sharp fonts are there but not the game's own font images they are matched with (hd_textures\converted): the
# game cannot use them and shows its old PS2 button symbols, Xbox prompts included. Made again from the disc (fonts only).
if (-not $needTextures -and (Test-Path (Join-Path $Port "hd_textures\crisp_fonts\manifest.json")) -and
    -not (Test-Path (Join-Path $Port "hd_textures\converted\textures\TFA2\FONTS"))) {
    Step "Making the button prompts and fonts again (the game's own font images are missing)" "fonts"
    Invoke-OptionalFonts -Python $Python -Port $Port
    $needTextures = $true
}
# (TODO 186) The sharp fonts and PS5 button symbols (hd_textures\crisp_fonts): made when an install with the HD textures
# has none - one made before they existed, or without numpy, kept the game's old letters through every update.
$crispMissing = (Test-Path (Join-Path $Port "hd_textures\converted\textures")) -and
    -not (Test-Path (Join-Path $Port "hd_textures\crisp_fonts\manifest.json")) -and
    (Select-String -LiteralPath (Join-Path $Port "tools\make_hd_textures.ps1") -SimpleMatch "CrispFontsOnly" -Quiet)
if ($crispMissing) {
    Step "Making the sharp fonts and PS5 button symbols (hd_textures\crisp_fonts)" "HD textures"
    Invoke-Probe { & $Python -c "import numpy" } | Out-Null
    if ($LASTEXITCODE -ne 0) { Invoke-Native { & $Python -m pip install --user numpy } | Out-Null }
    & (Join-Path $Port "tools\make_hd_textures.ps1") -CrispFontsOnly
    if (-not $?) { Write-Host "warning: the sharp fonts were not made (see above); the game keeps its own letters" -ForegroundColor Yellow }
    $needTextures = $true
}
# (the steps of a repository that holds this folder: its hook says whether it rebuilt anything)
$hooked = $false
if (Get-Command Invoke-UpdateHook -ErrorAction SilentlyContinue) {
    $ctx = New-HookContext -Root $Repo -Port $Port -Python $Python -State $state -HookArgs $HookArgs -Changed $changed -Force $Force.IsPresent
    try { $hooked = [bool](Invoke-UpdateHook $ctx) } catch { Fail $_.Exception.Message }
}
if (-not ($needBuild -or $needTextures -or $hooked)) { Write-Host "Nothing to rebuild (documentation only)." }

$state | Add-Member -Force SetUpAt (& git -C $Repo rev-parse HEAD)
$state | ConvertTo-Json | Set-Content $StateFile -Encoding UTF8
$took = $clock.Elapsed
if ($script:part) { $parts[$script:part] += $took.TotalSeconds - $script:partStart }
if ($kept) { Write-Host (Kept-Text $kept) -ForegroundColor Green }
$spentOn = @($parts.Keys | Where-Object { $parts[$_] -ge 1 } | ForEach-Object { "$_ $(Format-Time $parts[$_])" })
Write-Host ("`nUp to date ({0}), in {1}{2}. Play with Play.bat." -f (& git -C $Repo rev-parse --short HEAD), (Format-Time $took.TotalSeconds),
    $(if ($spentOn.Count -gt 1) { ": " + ($spentOn -join ", ") } else { "" })) -ForegroundColor Green
Add-DesktopShortcutOnce -Repo $Repo -Port $Port
if (Get-Command Write-StepLog -ErrorAction SilentlyContinue) { Write-StepLog "update: up to date" }
Result "ok $((& git -C $Repo rev-parse HEAD).Trim())"
Start-Game
try { Stop-Transcript | Out-Null } catch {}
