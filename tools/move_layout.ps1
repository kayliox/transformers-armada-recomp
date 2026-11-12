<#
.SYNOPSIS
    (TODO 279, docs/INSTALL_LAYOUT_PLAN.md section 3) Moves a player's install to the new folder layout, once: the
    player's own things out of the checkout into the install folder, a backup first, nothing deleted.

.DESCRIPTION
    Started by build.ps1 at the end of a good build, in a player's install only (tfa-setup.exe in the folder above the
    checkout). The game is closed then (the updater waited for it); when it is running this does nothing and the next
    build tries again. Without a question to the player (the user's decision, 2026-11-10).

    The new places, all in the install folder (<install>):
      settings.ini                  from the checkout's settings.ini
      saves\mc0, saves\mc1          the memory cards, from game\mc0, game\mc1
      saves\states\                 save states: quick.ps2state and states\*.ps2state (the updater's own files in
                                    states\ stay: the updater and old setup programs write them there)
      saves\backups, saves\exports  F1 > Saves' folders, from Saves\Backups, Saves\Exports (and the rest of Saves\)
      logs\                         the newest game logs (runs\play_*), copied
      data\game                     the disc's files, moved (a rename on the same drive: instant)
      data\movies_hd                the HD movies, moved
      layout.json                   {"layout": 2}, written once the copies are checked and before the big folders
                                    move: the game uses each new place only once it is there
    The HD textures and the mods stay in the checkout: the tools that make them, and the updater of the release before
    this one (which runs during this very update), write them there. The game finds them there in both layouts.

    Safety, in this order: a journal (<install>\layout_move.json: each step and its state, so an interrupted move
    carries on); a backup of the saves, the settings and the states in <install>\saves\backups\before_layout2_<date>\,
    every file checked by its hash - when it cannot be made (no room, no rights) NOTHING moves and the install plays
    on as it was; saves and settings COPIED and checked by hash (the originals stay where they were); then layout.json, then the
    big folders renamed (each new place is used only once it is there, so a stop half-way leaves everything findable). The old memory cards, now inside data\game, are renamed mc0.moved and
    mc1.moved (two checked copies of them exist: saves\ and the backup) so they cannot be taken for the cards in use.

    No absolute path is written anywhere (layout.json holds only the number): a moved install can be copied whole to
    another drive or PC (section 2.4).

    There is no way back and no switch to stay: the new layout is the standard (the user's decision, 2026-11-11).
    A settings.ini already in the install folder (RecompLauncher writes its settings there) is merged with the
    checkout's: the checkout's file is the base, the install folder's values win (both are in the backup).
#>
param(
    [Parameter(Mandatory)][string]$GameFolder,   # the checkout (or its port\)
    [Parameter(Mandatory)][string]$Root          # the install folder
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "install_layout.ps1")

function Say([string]$m) { Write-Host "  [layout] $m" }
$journalFile = Join-Path $Root "layout_move.json"

$GameFolder = (Resolve-Path -LiteralPath $GameFolder).Path.TrimEnd('\')
$Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
$running = Get-Process TransformersRecomp -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase) }
if ($running) { Say "the game is running: the folders are moved at the next update"; return }

# (moved and finished: nothing to do; layout.json there but the journal unfinished: an interrupted move carries on)
$unfinished = (Test-Path -LiteralPath $journalFile) -and -not ((Get-Content -LiteralPath $journalFile -Raw) -match '"finished"')
if ((Get-LayoutVersion $Root) -eq 2 -and -not $unfinished) { return }
Write-Host "`n==> Moving your saves, settings and the disc's files into the install folder (once)" -ForegroundColor Cyan
$stamp = Get-Date -Format "yyyy-MM-dd_HHmmss"

# What there is to move: [kind, from (relative to the checkout), to (relative to the install folder)].
#   copy: copied and checked (the original stays); move: renamed (big folders)
function Get-Plan {
    $p = @()
    foreach ($card in "mc0", "mc1") { $p += , @("copy", "game\$card", "saves\$card") }
    $p += , @("copy", "settings.ini", "settings.ini")
    $p += , @("copy", "quick.ps2state", "saves\states\quick.ps2state")
    $states = Join-Path $GameFolder "states"
    if (Test-Path -LiteralPath $states) {
        # (not the updater's own files: update_resume.*, update_result.txt, update_kept.txt stay in states\)
        foreach ($f in Get-ChildItem -LiteralPath $states -File | Where-Object { $_.Name -notlike "update_*" }) {
            $p += , @("copy", "states\$($f.Name)", "saves\states\$($f.Name)")
        }
    }
    $saves = Join-Path $GameFolder "Saves"
    if (Test-Path -LiteralPath $saves) {
        foreach ($item in Get-ChildItem -LiteralPath $saves) {
            $to = switch ($item.Name) { "Backups" { "saves\backups" } "Exports" { "saves\exports" } "README.txt" { "saves\README_old.txt" } default { "saves\$($item.Name)" } }
            $p += , @("copy", "Saves\$($item.Name)", $to)
        }
    }
    $p += , @("move", "game", "data\game")
    $p += , @("move", "movies_hd", "data\movies_hd")
    return $p
}

# The files under a path (a file or a folder), relative to it.
function Get-Files([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) { return @("") }
    return @(Get-ChildItem -LiteralPath $Path -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($Path.Length).TrimStart('\') })
}
function Join-Rel([string]$a, [string]$b) { if ($b) { Join-Path $a $b } else { $a } }

# Copies a file or folder and checks every file by size and hash; true when every file arrived intact.
function Copy-Checked([string]$From, [string]$To) {
    foreach ($rel in (Get-Files $From)) {
        $src = Join-Rel $From $rel; $dst = Join-Rel $To $rel
        New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
        $same = (Test-Path -LiteralPath $dst) -and (Get-Item -LiteralPath $dst).Length -eq (Get-Item -LiteralPath $src).Length -and
                (Get-FileHash -LiteralPath $dst).Hash -eq (Get-FileHash -LiteralPath $src).Hash
        if (-not $same) { Copy-Item -LiteralPath $src -Destination $dst -Force }
        if ((Get-FileHash -LiteralPath $dst).Hash -ne (Get-FileHash -LiteralPath $src).Hash) { return $false }
    }
    if ((Test-Path -LiteralPath $From -PathType Container) -and -not (Test-Path -LiteralPath $To)) { New-Item -ItemType Directory -Force $To | Out-Null }
    return $true
}

function Save-Journal($j) { [IO.File]::WriteAllText($journalFile, ($j | ConvertTo-Json -Depth 5)) }

# settings.ini: the checkout's lines as they are, each KEY=value of the install folder's file put over the same key
# (or added at the end). Comments and blank lines of the checkout's file are kept.
function Merge-Settings([string]$Base, [string]$Over) {
    $keyOf = { param($line) $m = [regex]::Match($line, '^\s*([^#;=\s][^=]*?)\s*='); if ($m.Success) { $m.Groups[1].Value.ToUpperInvariant() } else { $null } }
    $lines = [Collections.Generic.List[string]]::new()
    foreach ($l in [IO.File]::ReadAllLines($Base)) { $lines.Add($l) }
    foreach ($l in [IO.File]::ReadAllLines($Over)) {
        $k = & $keyOf $l
        if (-not $k) { continue }
        $at = -1
        for ($i = 0; $i -lt $lines.Count; $i++) { if ((& $keyOf $lines[$i]) -eq $k) { $at = $i } }
        if ($at -ge 0) { $lines[$at] = $l } else { $lines.Add($l) }
    }
    return ($lines -join "`r`n") + "`r`n"
}
$checkoutName = $GameFolder.Substring($Root.Length).TrimStart('\')   # (transformers-armada-recomp, or ...\port)

try {
    # --- 1. The journal: started, or carried on ------------------------------------------------------------------
    if (Test-Path -LiteralPath $journalFile) {
        $journal = Get-Content -LiteralPath $journalFile -Raw | ConvertFrom-Json
        Say "carrying on the move started $($journal.started)"
    } else {
        $steps = @(Get-Plan | Where-Object { Test-Path -LiteralPath (Join-Path $GameFolder $_[1]) } |
            ForEach-Object { [pscustomobject]@{ kind = $_[0]; from = $_[1]; to = $_[2]; state = "todo" } })
        $journal = [pscustomobject]@{ started = $stamp; backup = "saves\backups\before_layout2_$stamp"; backup_done = $false; steps = $steps }
        Save-Journal $journal
    }

    # --- 2. The backup: the saves, the settings, the states - checked, or nothing moves ----------------------------
    if (-not $journal.backup_done) {
        $backup = Join-Path $Root $journal.backup
        $need = 0
        foreach ($s in $journal.steps | Where-Object { $_.kind -eq "copy" }) {
            $need += (Get-Files (Join-Path $GameFolder $s.from) | ForEach-Object { (Get-Item -LiteralPath (Join-Rel (Join-Path $GameFolder $s.from) $_)).Length } | Measure-Object -Sum).Sum
        }
        $free = (Get-PSDrive -Name (Split-Path $Root -Qualifier).TrimEnd(':') -ErrorAction SilentlyContinue).Free
        # (twice what is copied - the backup and the copy - and 200 MB to spare)
        if ($free -and $free -lt (2 * $need + 200MB)) { throw "not enough free space for the backup ($([math]::Round($need / 1MB)) MB, twice)" }
        foreach ($s in $journal.steps | Where-Object { $_.kind -eq "copy" }) {
            if (-not (Copy-Checked (Join-Path $GameFolder $s.from) (Join-Path $backup $s.from))) { throw "the backup of $($s.from) did not check out" }
        }
        New-Item -ItemType Directory -Force $backup | Out-Null
        [IO.File]::WriteAllText((Join-Path $backup "README.txt"),
            "A copy of your saves, settings and save states as they were before the game's folders were tidied up on $($journal.started).`r`n" +
            "They were in the folder $checkoutName (game\mc0, game\mc1, settings.ini, quick.ps2state, states, Saves).`r`n" +
            "Nothing needs to be done with it: keep it until you are sure your saves are fine, then delete it if you need the space.`r`n")
        $journal.backup_done = $true
        Save-Journal $journal
        Say "backup made: $($journal.backup)"
    }

    # --- 3. Saves, settings, states: copied and checked (the originals stay) --------------------------------------
    foreach ($s in $journal.steps | Where-Object { $_.kind -eq "copy" -and $_.state -ne "done" }) {
        $from = Join-Path $GameFolder $s.from; $to = Join-Path $Root $s.to
        if ($s.to -eq "settings.ini" -and (Test-Path -LiteralPath $to)) {
            # (one there already, written by RecompLauncher: merged - its values win, the player's other settings stay)
            Copy-Item -LiteralPath $to -Destination (Join-Path (Join-Path $Root $journal.backup) "settings_install_folder.ini") -Force
            $tmp = "$to.tmp"
            [IO.File]::WriteAllText($tmp, (Merge-Settings $from $to))
            Move-Item -LiteralPath $tmp -Destination $to -Force
            $s.state = "done"; Save-Journal $journal; continue
        }
        if (-not (Copy-Checked $from $to)) { throw "the copy of $($s.from) did not check out" }
        $s.state = "done"
        Save-Journal $journal
    }

    # --- 4. layout.json: from now on the game, the launcher and the scripts use the new places that are there ------
    # (written before the big folders move: each place is used only once it is there - install_layout.cpp, Get-GameDataDir -
    # so a move stopped half-way leaves every item findable, in its old place or its new one)
    # (saves\mc0 made even when there was no card yet: the game's new saves must land there, not in data\game)
    New-Item -ItemType Directory -Force (Join-Path $Root "saves\mc0"), (Join-Path $Root "saves\states"), (Join-Path $Root "data"), (Join-Path $Root "logs") | Out-Null
    # (a fresh install has no settings.ini yet: one made from the example here, so every way of starting the game -
    # the setup's Play button, Play.bat, the launcher - finds the same file in the install folder)
    $rootSettings = Join-Path $Root "settings.ini"
    if (-not (Test-Path -LiteralPath $rootSettings)) {
        $example = @((Join-Path $GameFolder "overlay\settings.example.ini"), (Join-Path $GameFolder "settings.example.ini")) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if ($example) { Copy-Item -LiteralPath $example -Destination $rootSettings }
    }
    [IO.File]::WriteAllText((Join-Path $Root "layout.json"), "{`"layout`": 2}`r`n")

    # --- 5. The big folders: renamed (same drive, instant) -------------------------------------------------------
    foreach ($s in $journal.steps | Where-Object { $_.kind -eq "move" -and $_.state -ne "done" }) {
        $from = Join-Path $GameFolder $s.from; $to = Join-Path $Root $s.to
        if ((Test-Path -LiteralPath $from) -and -not (Test-Path -LiteralPath $to)) {
            New-Item -ItemType Directory -Force (Split-Path $to -Parent) | Out-Null
            Move-Item -LiteralPath $from -Destination $to
        } elseif ((Test-Path -LiteralPath $from) -and (Test-Path -LiteralPath $to)) {
            throw "both $from and $to exist: left as they are"
        }
        $s.state = "done"
        Save-Journal $journal
    }
    # (the old cards came along inside data\game: renamed, so they cannot be taken for the cards in use)
    foreach ($card in "mc0", "mc1") {
        $old = Join-Path $Root "data\game\$card"
        if ((Test-Path -LiteralPath $old) -and (Test-Path -LiteralPath (Join-Path $Root "saves\$card")) -and -not (Test-Path -LiteralPath "$old.moved")) {
            Rename-Item -LiteralPath $old -NewName "$card.moved"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $Root "data\game")) {
        [IO.File]::WriteAllText((Join-Path $Root "data\game\README.txt"),
            "The game's files from your disc. Do not share this folder.`r`n" +
            "mc0.moved and mc1.moved are your memory cards as they were before the move: the game uses saves\mc0 and saves\mc1 now.`r`n")
    }

    # --- 6. Logs: the newest game logs copied into logs\ -----------------------------------------------------------
    $logs = Join-Path $Root "logs"
    $runs = Join-Path $GameFolder "runs"
    if (Test-Path -LiteralPath $runs) {
        Get-ChildItem -LiteralPath $runs -File -Filter "play_*" | Sort-Object LastWriteTime -Descending | Select-Object -First 20 |
            ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $logs -Force -ErrorAction SilentlyContinue }
    }
    [IO.File]::WriteAllText((Join-Path $Root "saves\README.txt"),
        "Your saves.`r`n`r`n" +
        "mc0 and mc1: your memory card saves (the same files a PlayStation 2 writes to a memory card).`r`n" +
        "states: save states (F6 / F9 and the named ones). A save state works only with the version of the game that made it.`r`n" +
        "backups, exports: what F1 > Saves makes.`r`n`r`n" +
        "Back up: copy this whole saves folder somewhere safe. Never post save files or save states in public: they contain game data.`r`n")

    $journal | Add-Member -Force finished (Get-Date -Format "yyyy-MM-dd_HHmmss")
    Save-Journal $journal
    Say "done: saves in $(Join-Path $Root 'saves'), settings in $(Join-Path $Root 'settings.ini'), the disc's files in $(Join-Path $Root 'data\game')"
    Say "the old saves and settings stay in the checkout as they were, and a backup is in $($journal.backup)"
} catch {
    # (the install plays on as it was: before layout.json nothing uses the new places; the journal carries on next time)
    Write-Host "  [layout] the folders were not moved: $($_.Exception.Message). The game plays on as before; the next update tries again." -ForegroundColor Yellow
}
