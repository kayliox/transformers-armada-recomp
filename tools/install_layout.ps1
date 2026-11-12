# install_layout.ps1 (TODO 279, docs/INSTALL_LAYOUT_PLAN.md) - the installed folder layout as the scripts see it.
# Dot-sourced by build.ps1, setup.ps1, update.ps1, tools\make_hd_textures.ps1 and tools\move_layout.ps1.
#
# An install is the folder the player picked (tfa-setup.exe in it) holding the checkout. Layout 1 (every install up
# to now): everything is in the checkout. Layout 2 (<install>\layout.json says {"layout": 2}): the player's things are
# in the install folder - settings.ini, saves\, logs\, data\game (the disc's files), data\movies_hd - and the checkout
# keeps the program, its build, the HD textures and the mods. Nothing here writes an absolute path anywhere: every
# place is found from where the checkout is (a copied install works on another drive or PC, section 2.4).

# The install folder above a game folder (the checkout, or its port\ in a repository that holds it), or "" when the
# game folder is not in an install (a developer's checkout): the folder above with tfa-setup.exe, layout.json or the
# setup's .setup_unfinished mark in it.
function Get-InstallRoot([string]$GameFolder) {
    $here = (Resolve-Path -LiteralPath $GameFolder).Path.TrimEnd('\')
    $above = @(Split-Path $here -Parent)
    if ((Split-Path $here -Leaf) -eq "port") { $above += Split-Path $above[0] -Parent }
    # (.setup_unfinished: the setup's mark while it installs - it copies itself into the folder only at the end, so a
    # fresh install's first build would otherwise not see an install: no launcher, and no move until the first update)
    foreach ($d in $above) {
        if ($d -and ((Test-Path -LiteralPath (Join-Path $d "tfa-setup.exe")) -or (Test-Path -LiteralPath (Join-Path $d "layout.json")) -or
                (Test-Path -LiteralPath (Join-Path $d ".setup_unfinished")))) { return $d }
    }
    return ""
}

# The layout number layout.json holds (1 without it), as the game reads it (install_layout.cpp: the number after "layout").
function Get-LayoutVersion([string]$Root) {
    if (-not $Root) { return 1 }
    $f = Join-Path $Root "layout.json"
    if (-not (Test-Path -LiteralPath $f)) { return 1 }
    $m = [regex]::Match((Get-Content -LiteralPath $f -Raw), '"layout"\s*:\s*(\d+)')
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return 1
}

# The disc's extracted files: <install>\data\game in layout 2 when its executable is there, else <game folder>\game.
function Get-GameDataDir([string]$GameFolder) {
    $root = Get-InstallRoot $GameFolder
    if ((Get-LayoutVersion $root) -eq 2) {
        $moved = Join-Path $root "data\game"
        if ((Test-Path -LiteralPath (Join-Path $moved "SLUS_206.68")) -or (Test-Path -LiteralPath (Join-Path $moved "SLES_523.88"))) { return $moved }
    }
    return (Join-Path $GameFolder "game")
}

# Where the scripts' own logs go: <install>\logs in layout 2 when it is there, else "" (the caller's old place).
function Get-LayoutLogsDir([string]$GameFolder) {
    $root = Get-InstallRoot $GameFolder
    if ((Get-LayoutVersion $root) -eq 2 -and (Test-Path -LiteralPath (Join-Path $root "logs"))) { return (Join-Path $root "logs") }
    return ""
}
