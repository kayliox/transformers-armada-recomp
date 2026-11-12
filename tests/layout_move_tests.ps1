# layout_move_tests.ps1 - tools\move_layout.ps1 and tools\install_layout.ps1 (TODO 279) on made-up installs in the temp
# folder: the move itself, a second run (nothing more happens), a move stopped half-way and carried on, an install with
# no saves yet, a European disc, a developer's checkout (not an install: never moved), the old TF_LAYOUT_MOVE=0
# switch ignored (no way to stay on the old layout), a settings.ini already in the install folder (RecompLauncher's:
# merged), the mods' repository (the game folder is its port\), and a finished install copied to another folder
# (every place still found: no absolute path written anywhere).
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests\layout_move_tests.ps1   (exit code 0: all passed)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failed++ }
}
. (Join-Path $Root "tools\install_layout.ps1")
$move = Join-Path $Root "tools\move_layout.ps1"
$base = Join-Path ([IO.Path]::GetTempPath()) ("tfa_layout_" + [guid]::NewGuid().ToString("N"))

# An install as the setup leaves it: <install>\tfa-setup.exe and the checkout with the player's things in it.
function New-Install([string]$Name, [switch]$NoSaves, [string]$Elf = "SLUS_206.68", [switch]$NoSetupExe, [switch]$ModsRepo) {
    $inst = Join-Path $base $Name
    $co = if ($ModsRepo) { Join-Path $inst "transformers-armada-ps2\port" } else { Join-Path $inst "transformers-armada-recomp" }
    New-Item -ItemType Directory -Force "$co\game\FMV", "$co\states", "$co\runs" | Out-Null
    if (-not $NoSetupExe) { Set-Content "$inst\tfa-setup.exe" "setup" }
    Set-Content "$co\game\$Elf" "elf"; Set-Content "$co\game\TFA.ZIP" ("x" * 5000); Set-Content "$co\game\FMV\A.PSS" "pss"
    Set-Content "$co\settings.example.ini" "# example"
    if (-not $NoSaves) {
        New-Item -ItemType Directory -Force "$co\game\mc0\BASLUS-20668", "$co\game\mc1", "$co\Saves\Backups", "$co\Saves\Exports", "$co\movies_hd" | Out-Null
        Set-Content "$co\game\mc0\BASLUS-20668\save.bin" "my save"; Set-Content "$co\game\mc1\x.bin" "card 2"
        # (a real card folder holds several files, with spaces in their names: BASLUS-20668 TRANSFM, Icon.ico, icon.sys)
        New-Item -ItemType Directory -Force "$co\game\mc0\BASLUS-20668 TRANSFM" | Out-Null
        Set-Content "$co\game\mc0\BASLUS-20668 TRANSFM\BASLUS-20668 TRANSFM" "save data"; Set-Content "$co\game\mc0\BASLUS-20668 TRANSFM\Icon.ico" "icon"
        Set-Content "$co\settings.ini" "TF_FOV=1.4"; Set-Content "$co\quick.ps2state" "quick"
        Set-Content "$co\states\named.ps2state" "named"; Set-Content "$co\states\update_result.txt" "ok"
        Set-Content "$co\Saves\Backups\b.bin" "backup"; Set-Content "$co\Saves\Exports\e.psu" "export"; Set-Content "$co\Saves\README.txt" "old readme"
        Set-Content "$co\movies_hd\N1.mp4" "movie"; Set-Content "$co\runs\play_2026-11-11_1200.err" "log"
    }
    return @{ Inst = $inst; Co = $co }
}
function Read([string]$p) { if (Test-Path -LiteralPath $p) { (Get-Content -LiteralPath $p -Raw).Trim() } else { "<missing>" } }

try {
    Write-Host "1. a full move"
    $i = New-Install "full install"   # (a space in the name, as the game's own folder may have)
    & $move -GameFolder $i.Co -Root $i.Inst *> $null
    $r = $i.Inst
    Check ((Get-LayoutVersion $r) -eq 2) "layout.json says 2"
    Check ((Read "$r\layout.json") -eq '{"layout": 2}') "layout.json holds only the number (no path)"
    Check ((Read "$r\saves\mc0\BASLUS-20668\save.bin") -eq "my save" -and (Read "$r\saves\mc1\x.bin") -eq "card 2") "memory cards in saves\mc0, saves\mc1"
    Check ((Read "$r\settings.ini") -eq "TF_FOV=1.4") "settings.ini in the install folder"
    Check ((Read "$r\saves\mc0\BASLUS-20668 TRANSFM\BASLUS-20668 TRANSFM") -eq "save data" -and (Read "$r\saves\mc0\BASLUS-20668 TRANSFM\Icon.ico") -eq "icon") "a card folder with several files and spaces in its names"
    Check ((Read "$r\saves\states\quick.ps2state") -eq "quick" -and (Read "$r\saves\states\named.ps2state") -eq "named") "save states in saves\states"
    Check ((Read "$($i.Co)\states\update_result.txt") -eq "ok" -and -not (Test-Path "$r\saves\states\update_result.txt")) "the updater's own files stay in the checkout"
    Check ((Read "$r\saves\backups\b.bin") -eq "backup" -and (Read "$r\saves\exports\e.psu") -eq "export") "F1 > Saves' folders in saves\"
    Check ((Read "$r\data\game\SLUS_206.68") -eq "elf" -and -not (Test-Path "$($i.Co)\game")) "the disc's files moved to data\game"
    Check ((Read "$r\data\game\mc0.moved\BASLUS-20668\save.bin") -eq "my save" -and -not (Test-Path "$r\data\game\mc0")) "old cards renamed mc0.moved inside data\game"
    Check ((Read "$r\data\movies_hd\N1.mp4") -eq "movie") "HD movies moved to data\movies_hd"
    Check ((Read "$r\logs\play_2026-11-11_1200.err") -eq "log") "game logs copied to logs\"
    $bk = @(Get-ChildItem "$r\saves\backups" -Directory -Filter "before_layout2_*")
    Check ($bk.Count -eq 1 -and (Read "$($bk[0].FullName)\game\mc0\BASLUS-20668\save.bin") -eq "my save" -and (Read "$($bk[0].FullName)\settings.ini") -eq "TF_FOV=1.4") "the backup holds the cards and the settings"
    Check ((Read "$($i.Co)\settings.ini") -eq "TF_FOV=1.4" -and (Read "$($i.Co)\quick.ps2state") -eq "quick") "the originals in the checkout are left as they were"
    Check ((Get-GameDataDir $i.Co) -eq "$r\data\game") "the scripts find the disc's files (Get-GameDataDir)"
    Check ((Get-LayoutLogsDir $i.Co) -eq "$r\logs") "the scripts' logs go to logs\"
    $j = Get-Content "$r\layout_move.json" -Raw | ConvertFrom-Json
    Check ([bool]$j.finished -and -not ((Get-Content "$r\layout_move.json" -Raw) -match [regex]::Escape($r))) "the journal is finished and holds no absolute path"

    Write-Host "2. run again: nothing more happens"
    Set-Content "$r\saves\mc0\BASLUS-20668\save.bin" "newer save"
    & $move -GameFolder $i.Co -Root $i.Inst *> $null
    Check ((Read "$r\saves\mc0\BASLUS-20668\save.bin") -eq "newer save" -and @(Get-ChildItem "$r\saves\backups" -Directory -Filter "before_layout2_*").Count -eq 1) "a second run changes nothing"

    Write-Host "3. stopped half-way (the disc's folder could not be moved), then carried on"
    $i = New-Install "stopped"
    $r = $i.Inst
    New-Item -ItemType Directory -Force "$r\data\game" | Out-Null; Set-Content "$r\data\game\blocker" "x"   # (a folder in the way: the move stops there)
    & $move -GameFolder $i.Co -Root $i.Inst *> $null
    Check ((Get-LayoutVersion $r) -eq 2 -and (Read "$r\saves\mc0\BASLUS-20668\save.bin") -eq "my save") "saves and settings in place, layout.json written"
    Check ((Get-GameDataDir $i.Co) -eq (Join-Path $i.Co "game")) "the disc's files still found in the checkout (each place used only once it is there)"
    Remove-Item -Recurse -Force "$r\data\game"
    & $move -GameFolder $i.Co -Root $i.Inst *> $null
    Check ((Read "$r\data\game\SLUS_206.68") -eq "elf" -and (Get-GameDataDir $i.Co) -eq "$r\data\game") "the next run finishes the move"
    Check ([bool]((Get-Content "$r\layout_move.json" -Raw | ConvertFrom-Json).finished)) "the journal is finished"

    Write-Host "4. other installs"
    $i = New-Install "no saves yet" -NoSaves
    & $move -GameFolder $i.Co -Root $i.Inst *> $null
    Check ((Get-LayoutVersion $i.Inst) -eq 2 -and (Test-Path "$($i.Inst)\saves\mc0") -and (Read "$($i.Inst)\data\game\SLUS_206.68") -eq "elf") "a fresh install: saves\mc0 made, so new saves land there"
    Check ((Read "$($i.Inst)\settings.ini") -eq "# example" -and -not (Test-Path "$($i.Co)\settings.ini")) "a fresh install: settings.ini made from the example in the install folder (none in the checkout)"
    $i = New-Install "setup running" -NoSetupExe -NoSaves
    Set-Content "$($i.Inst)\.setup_unfinished" ""
    Check ((Get-InstallRoot $i.Co) -eq $i.Inst) "the setup's first build (only its .setup_unfinished mark there yet): an install, moved at once"
    $i = New-Install "pal" -Elf "SLES_523.88"
    & $move -GameFolder $i.Co -Root $i.Inst *> $null
    Check ((Get-GameDataDir $i.Co) -eq "$($i.Inst)\data\game") "the European disc: data\game found by its own executable"
    $i = New-Install "dev checkout" -NoSetupExe
    Check (-not (Get-InstallRoot $i.Co)) "a developer's checkout (no tfa-setup.exe above it) is not an install"
    $i = New-Install "old switch"
    $env:TF_LAYOUT_MOVE = "0"
    & $move -GameFolder $i.Co -Root $i.Inst *> $null
    Remove-Item Env:TF_LAYOUT_MOVE
    Check ((Get-LayoutVersion $i.Inst) -eq 2 -and (Read "$($i.Inst)\saves\mc0\BASLUS-20668\save.bin") -eq "my save") "the old TF_LAYOUT_MOVE=0 is ignored: no way to stay on the old layout"

    Write-Host "5. a settings.ini already in the install folder (RecompLauncher writes its settings there): merged"
    $i = New-Install "launcher settings"
    Set-Content "$($i.Co)\settings.ini" "# my settings`r`nTF_FOV=1.4`r`nTF_DRAW_DISTANCE=3`r`nPS2X_FULLSCREEN=0"
    Set-Content "$($i.Inst)\settings.ini" "PS2X_FULLSCREEN=1`r`nTF_FPS_CAP=120"
    & $move -GameFolder $i.Co -Root $i.Inst *> $null
    $merged = @(Get-Content "$($i.Inst)\settings.ini")
    Check (($merged -contains "TF_FOV=1.4") -and ($merged -contains "TF_DRAW_DISTANCE=3") -and ($merged -contains "# my settings")) "the player's own settings and comments kept"
    Check (($merged -contains "PS2X_FULLSCREEN=1") -and -not ($merged -contains "PS2X_FULLSCREEN=0") -and ($merged -contains "TF_FPS_CAP=120")) "the install folder's values win, its new keys added"
    $bk = @(Get-ChildItem "$($i.Inst)\saves\backups" -Directory -Filter "before_layout2_*")[0].FullName
    Check ((Read "$bk\settings_install_folder.ini") -match "PS2X_FULLSCREEN=1" -and (Read "$bk\settings.ini") -match "TF_DRAW_DISTANCE=3") "both files as they were in the backup"

    Write-Host "6. the mods' repository: the game folder is its port\"
    $i = New-Install "mods install" -ModsRepo
    New-Item -ItemType Directory -Force "$($i.Co)\Mods\JetBike" | Out-Null; Set-Content "$($i.Co)\Mods\JetBike\disabled" "off"
    & $move -GameFolder $i.Co -Root $i.Inst *> $null
    Check ((Get-InstallRoot $i.Co) -eq $i.Inst -and (Get-LayoutVersion $i.Inst) -eq 2) "the install found above the repository"
    Check ((Read "$($i.Inst)\saves\mc0\BASLUS-20668\save.bin") -eq "my save" -and (Get-GameDataDir $i.Co) -eq "$($i.Inst)\data\game") "cards in saves\, the disc's files in data\game"
    Check ((Read "$($i.Co)\Mods\JetBike\disabled") -eq "off") "Mods\ and the mods' on/off choices stay in port\"
    $bk = @(Get-ChildItem "$($i.Inst)\saves\backups" -Directory -Filter "before_layout2_*")[0].FullName
    Check ((Read "$bk\README.txt") -match [regex]::Escape("transformers-armada-ps2\port")) "the backup's note names the real folder"

    Write-Host "7. a moved install copied whole to another folder (another drive, a Steam Deck)"
    $copy = Join-Path $base "copied elsewhere"
    Copy-Item -Recurse (Join-Path $base "full install") $copy
    $co = Join-Path $copy "transformers-armada-recomp"
    Check ((Get-InstallRoot $co) -eq $copy -and (Get-GameDataDir $co) -eq "$copy\data\game") "every place found from the copy's own folder"
    $text = (Get-ChildItem $copy -Recurse -File -Include *.json, *.ini, *.txt | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
    Check (-not ($text -match [regex]::Escape((Join-Path $base "full install")))) "no file of the copy names the old folder"
} finally {
    Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "`n$failed check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nall checks passed" -ForegroundColor Green
exit 0
