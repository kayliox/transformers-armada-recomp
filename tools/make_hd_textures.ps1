<#
.SYNOPSIS
    Makes the HD texture set from your own disc: every texture of the game upscaled (4K characters,
    2K levels, alpha kept), in hd_textures\ next to this folder's parent, where the native renderer
    finds it on its own (TF_HIRES_DIR overrides).

.DESCRIPTION
    1. Extracts the disc's archives (game\TFA.ZIP, TFA2.ZIP, TA_XTRAS.ZIP, SOUNDE.ZIP) into
       hd_textures\archives (tools\hd_textures\extract_pk_archive.py).
    2. Converts every .ITF texture to PNG, palette variants included, into
       hd_textures\converted\textures (itf_to_png.py). The renderer compares these with the
       textures the game holds to pick the right image for each.
    3. Downloads Real-ESRGAN ncnn-vulkan v0.2.5.0 (MIT licence, runs on any Vulkan GPU: AMD,
       NVIDIA, Intel) into hd_textures\realesrgan, unless it is there already.
    4. Upscales into hd_textures\upscaled (upscale_textures.py: tiers and models in tier_rules.py).
       Resumable: run it again after a stop and it carries on. Then matches each image's colour to
       the game's own texture (match_colour.py: off by more than 5%, one tone curve per channel).
    5. Makes the font atlases again sharp, with PS5 button symbols, into hd_textures\crisp_fonts
       (make_crisp_fonts.py; TODO 119) - the Display settings "Sharp text" and "Button symbols".

    Needs: build.ps1 run once (it extracts the disc into game\), Python 3.11+ with Pillow
    (pip install pillow). Takes from tens of minutes to a few hours, depending on the GPU.
    Everything it writes is game data made from your disc: hd_textures\ is git-ignored, never
    commit or share it.

    Your set will be the same as kayliox's apart from a few hand-made fixes he made later (a
    handful of re-made upscales and redrawn faction insignia), which live in his own copy only.

.EXAMPLE
    .\tools\make_hd_textures.ps1
.EXAMPLE
    .\tools\make_hd_textures.ps1 -SkipUpscale     # steps 1-2 only (and 5)
.EXAMPLE
    .\tools\make_hd_textures.ps1 -CrispFontsOnly  # step 5 only
.EXAMPLE
    .\tools\make_hd_textures.ps1 -FontsOnly      # font assets only, no GPU or downloads
#>
param([switch]$SkipUpscale, [switch]$CrispFontsOnly, [switch]$FontsOnly, [string]$Python = "python",
      [ValidateSet("", "best", "fast")][string]$Quality = "",
      [switch]$Resume, [int]$GamePid = 0)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root "game"
$Out = Join-Path $Root "hd_textures"
$Py = Join-Path $PSScriptRoot "hd_textures"

# (TODO 219) Started by the game (-Resume): an HD set left unfinished is carried on in a small window of its own,
# which waits while the game runs and works once it closes. Python is looked for as the setup does.
if ($Resume) {
    try { $Host.UI.RawUI.WindowTitle = "Making the HD textures (you can close this; it carries on next time)" } catch {}
    . (Join-Path $PSScriptRoot "setup_common.ps1")
    if ($Python -eq "python") { $found = Find-Python; if ($found) { $Python = $found } }
    if ($GamePid -gt 0) {
        # (TODO 267, the user's choice: the game's own window makes them only while the game is open - beside it,
        # gently; it stops when the game closes (the game ends it), and the next start carries on)
        Write-Host "Making the rest of the HD textures while the game is open. Closing the game stops this;"
        Write-Host "what is made is kept and the next start of the game carries on."
    }
    else {
        Write-Host "Making the rest of the HD textures. It waits while the game runs and carries on when you close it."
        Write-Host "You can close this window at any time: it carries on the next time you start the game."
    }
}
# (TODO 267) Beside the game (-GamePid): never waiting for it; the upscale stops when it ends.
$withGame = if ($GamePid -gt 0) { @("--game-pid", "$GamePid") } else { @() }
# (in that window a stop is shown for a minute before it closes; otherwise an error ends the script as always)
trap {
    if ($Resume) {
        Write-Host "`nMaking the HD textures stopped: $_" -ForegroundColor Red
        Write-Host "It is tried again the next time you start the game. This window closes in a minute."
        Start-Sleep -Seconds 60
    }
    break
}

# (TODO 219) The choices (the setup writes them): Best or Fast, and whether to keep making them while the game runs.
$OptionsFile = Join-Path $Out "hd_options.json"
$StatusFile = Join-Path $Out "upscale_status.json"
if (-not $Quality) {
    $Quality = "best"
    if (Test-Path $OptionsFile) {
        try { $o = Get-Content $OptionsFile -Raw | ConvertFrom-Json; if ($o.quality -eq "fast") { $Quality = "fast" } } catch {}
    }
}
function SetStatus([string]$State) {
    # (the upscaler keeps the counts in it; this marks the whole set's state: "done" once everything is made)
    $st = [ordered]@{ state = $State; done = 0; total = 0; quality = $Quality }
    if (Test-Path $StatusFile) {
        try {
            $old = Get-Content $StatusFile -Raw | ConvertFrom-Json; $st.done = $old.done; $st.total = $old.total
            if ($old.gpu) { $st.gpu = $old.gpu }
        } catch {}
    }
    $st.updated = (Get-Date).ToUniversalTime().ToString("s")
    [System.IO.File]::WriteAllText($StatusFile, ($st | ConvertTo-Json))  # (UTF-8 without a BOM: the game reads it)
}
# One maker at a time: the setup's, Play.bat's or the game's. hd_textures\.upscaling holds its process id while it runs;
# Windows deletes the file when it ends (a closed window too).
$script:WorkerLock = $null
function TakeWorkerLock {
    $path = Join-Path $Out ".upscaling"
    New-Item -ItemType Directory -Force $Out | Out-Null
    for ($i = 0; $i -lt 2; $i++) {
        try {
            $script:WorkerLock = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write,
                ([System.IO.FileShare]::Read -bor [System.IO.FileShare]::Delete), 4096, [System.IO.FileOptions]::DeleteOnClose)
            $b = [System.Text.Encoding]::ASCII.GetBytes("$PID")
            $script:WorkerLock.Write($b, 0, $b.Length); $script:WorkerLock.Flush()
            return $true
        } catch { Start-Sleep -Milliseconds 300 }
    }
    return $false
}

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Run([string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = "Continue"
    & $Exe @Arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

# (TODO 186) numpy, which the sharp fonts need, installed when it is missing; $LASTEXITCODE 0 when it is there.
function EnsureNumpy {
    & $Python -c "import numpy" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Step "Installing numpy (Python), for the sharp fonts"
        & $Python -m pip install --user numpy 2>&1 | ForEach-Object { "$_" }
        & $Python -c "import numpy" 2>$null
    }
}

# Only FONTS from TFA2.ZIP: no other archives, downloads or GPU upscale.
if ($FontsOnly) {
    try {
        $ErrorActionPreference = "Continue"
        EnsureNumpy
        $ErrorActionPreference = "Stop"
        if ($LASTEXITCODE -ne 0) { throw "Python's numpy could not be installed (pip install numpy)" }
        Run $Python @((Join-Path $Py "make_fonts_only.py"), "--game", $Game, "--out", $Out, "--controls", (Join-Path $Root "controls.txt"))
    } catch {
        Write-Host "`nwarning: $($_.Exception.Message): the button prompts and sharp fonts are not made; continuing with the game's own fonts." -ForegroundColor Yellow
    }
    return
}

if (-not $CrispFontsOnly -and -not (TakeWorkerLock)) {
    if ($Resume) { Write-Host "The HD textures are being made already (another window)."; return }
    # (the setup or an update while the game's own window makes them: it waits for that one, then carries on)
    Write-Host "The HD textures are being made in another window: waiting for it (close it to carry on here)."
    while (-not (TakeWorkerLock)) { Start-Sleep -Seconds 5 }
}
$zips = "TFA", "TFA2", "TA_XTRAS", "SOUNDE" | ForEach-Object { Join-Path $Game "$_.ZIP" }
foreach ($z in $zips) { if (-not (Test-Path $z)) { throw "$z not found: run build.ps1 -Iso <your disc> first" } }
& $Python -c "import PIL" 2>$null
if ($LASTEXITCODE -ne 0) { throw "Python's Pillow is missing: pip install pillow" }

$archives = Join-Path $Out "archives"
if (-not (Test-Path (Join-Path $archives "TFA"))) {
    Step "Extracting the disc's archives into $archives"
    Run $Python (@((Join-Path $Py "extract_pk_archive.py")) + $zips + @("--out", $archives))
}

$converted = Join-Path $Out "converted\textures"
if (-not (Test-Path (Join-Path $converted "TFA"))) {
    Step "Converting the textures to PNG into $converted"
    $inputs = "TFA", "TFA2", "TA_XTRAS", "SOUNDE" | ForEach-Object { Join-Path $archives $_ } | Where-Object { Test-Path $_ }
    Run $Python (@((Join-Path $Py "itf_to_png.py")) + $inputs + @("--out", $converted, "--all-palettes"))
}

function CrispFonts {
    # 5. (TODO 119) The font atlases made again sharp at 8x - the menu font from TeX Gyre Adventor (GUST Font
    #    License, tools\hd_textures\typeface), the HUD's and HQ's rebuilt with sharp edges - with the button
    #    symbols drawn in the DualSense's style, into hd_textures\crisp_fonts (make_crisp_fonts.py). The
    #    game's Display settings "Sharp text" / "Button symbols" choose among them. Needs numpy; a minute.
    # (TODO 186: numpy installed here when it is missing - setup.ps1 never installed it, so an install made
    # without it never had the sharp text: the game showed the old letters)
    $ErrorActionPreference = "Continue"
    EnsureNumpy
    $ErrorActionPreference = "Stop"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nwarning: Python's numpy could not be installed (pip install numpy): the sharp text and PS5 button symbols are not made." -ForegroundColor Yellow
        return
    }
    Step "Making the sharp fonts and PS5 button symbols into $(Join-Path $Out 'crisp_fonts')"
    $a = @((Join-Path $Py "make_crisp_fonts.py"), "--archives", $archives, "--converted", $converted, "--out", (Join-Path $Out "crisp_fonts"))
    $up = Join-Path $Out "upscaled"
    if (Test-Path $up) { $a += @("--upscaled", $up) }
    $controls = Join-Path $Root "controls.txt"  # (the keyboard layout: the key prompts' keys)
    if (Test-Path $controls) { $a += @("--controls", $controls) }
    Run $Python $a
}

if ($CrispFontsOnly) { CrispFonts; return }
if ($SkipUpscale) { CrispFonts; return }

# (TODO 219) The sharp fonts first, when there are none yet: a minute, and the game started while the rest is made
# (the setup's "Play now", Play.bat) has sharp menus from the start. Made again at the end, with the upscaled set.
if (-not (Test-Path (Join-Path $Out "crisp_fonts\manifest.json"))) { CrispFonts }

$esr = Join-Path $Out "realesrgan"
$exe = Join-Path $esr "realesrgan-ncnn-vulkan.exe"
if (-not (Test-Path $exe)) {
    Step "Downloading Real-ESRGAN ncnn-vulkan v0.2.5.0"
    $zip = Join-Path $Out "realesrgan.zip"
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip" -OutFile $zip
    Expand-Archive $zip $esr -Force
    Remove-Item $zip
    if (-not (Test-Path $exe)) { throw "the Real-ESRGAN download did not contain realesrgan-ncnn-vulkan.exe" }
}

# (TODO 241) A set made before the maker checked its images: every image checked once (a graphics card that gives
# black, junk-striped or red-and-blue-exchanged images - a GTX 1660 SUPER did): exchanged ones put right, broken ones
# removed and made again below. Again whenever "Check HD textures" (F1) runs this on a finished set.
$checkedMark = Join-Path $Out ".images_checked_3"
$finished = $false
if (Test-Path $StatusFile) { try { $finished = ((Get-Content $StatusFile -Raw | ConvertFrom-Json).state -in @("done", "card_broken")) } catch {} }
if ((Test-Path (Join-Path $Out "upscaled\upscale_manifest.jsonl")) -and (-not (Test-Path $checkedMark) -or ($Resume -and $finished))) {
    Step "Checking the HD textures made so far (a few minutes)"
    Run $Python (@((Join-Path $Py "upscale_textures.py"), $converted, "--out", (Join-Path $Out "upscaled"), "--check-colours") +
        $(if ($GamePid -gt 0) { @("--no-wait-for-game") } else { @() }))
    Set-Content -LiteralPath $checkedMark -Value "checked $(Get-Date -Format s)"
}

Step "Upscaling into $(Join-Path $Out 'upscaled') (resumable)"
# A few textures failing is not a failed setup: the game keeps the original for each texture without
# an upscale, and a run again tries only those. (Exit 1 with its "upscaled N/M" summary = some files
# failed; anything else - no summary, exit 2 - is a real failure.)
$ErrorActionPreference = "Continue"
# (TODO 219) In batches, the textures seen first made first, each colour-matched as it is made; it waits while the
# game runs (unless hd_options.json says to carry on) and says "HD textures: N of M made" after each batch.
& $Python (Join-Path $Py "upscale_textures.py") $converted --out (Join-Path $Out "upscaled") `
    --work (Join-Path $Out "work") --exe $exe --quality $Quality --match-colour @withGame 2>&1 | ForEach-Object { "$_" } | Tee-Object -Variable lines | Out-Host
$code = $LASTEXITCODE
# (TODO 267) Exit 4: the game closed and the upscale stopped with it - the rest waits for the next start.
if ($code -eq 4) { exit 0 }
$ErrorActionPreference = "Stop"
$summary = $lines | Where-Object { $_ -match '^upscaled \d+/\d+ uniques' } | Select-Object -Last 1
# (TODO 242) Exit 3: this graphics card's upscales come out broken in every way the upscaler has (a GTX 1660 SUPER's
# were all black). Nothing is made on it - the game draws its own textures - and the next run tries the card again.
$cardBroken = ($code -eq 3)
if ($cardBroken) {
    Write-Host "`nwarning: this graphics card's HD images come out broken (black), in every way the upscaler can make them." -ForegroundColor Yellow
    Write-Host "No HD textures are made on it; the game draws its own. A newer graphics driver may fix it:" -ForegroundColor Yellow
    Write-Host "then F1 > Graphics > Check HD textures tries again." -ForegroundColor Yellow
}
elseif ($code -ne 0) {
    if ($code -eq 1 -and $summary) {
        Write-Host "`nwarning: some textures could not be upscaled ($summary); they keep their original look." -ForegroundColor Yellow
        Write-Host "Running make_hd_textures.ps1 (or setup.ps1) again retries only those." -ForegroundColor Yellow
    }
    else { throw "upscale_textures.py failed with exit code $code" }
}
# 4b. (TODO 124) Each upscale's colour matched to the game's own texture: the upscaler darkens the foliage
#     (10-37%), so the trees and the ground looked unlike the game. An image whose average over its opaque
#     parts is off by more than 5% gets one tone curve per channel (the detail stays as it is); the original is
#     kept in hd_textures\upscaled_precolour (match_colour.py --restore puts it back). Again: no change.
# (Said here, not only by the updater: an update runs the update.ps1 it had before the pull, this script as pulled.)
if (-not (Test-Path (Join-Path $Out "upscaled_precolour\colour_gains.json"))) {
    Step "Matching the HD textures' colours to the game's (one time, about 10 minutes)"
    if (Test-Path (Join-Path $Out "upscaled_bc7_cache")) {  # (a set already played with: its compressed copies are made again)
        Write-Host "Afterwards the first visit to each level loads a little slower, once: its changed textures are compressed again."
    }
}
else { Step "Matching the upscaled images' colour to the game's textures" }
# (--only-new: the images matched as they were made are not measured again)
Run $Python @((Join-Path $Py "match_colour.py"), "--upscaled", (Join-Path $Out "upscaled"), "--native", $converted, "--only-new")
CrispFonts
SetStatus $(if ($cardBroken) { "card_broken" } else { "done" })
Write-Host "`nDone. Start the game from this folder (Play.bat): the log says '[native render] upscaled textures: N images'." -ForegroundColor Green
