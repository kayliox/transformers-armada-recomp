#!/usr/bin/env bash
# Makes HD textures and sharp fonts from your extracted disc into hd_textures/.
# Needs Python 3 with Pillow, numpy, curl, unzip and a Vulkan driver for upscaling.
#   ./tools/make_hd_textures.sh
#   ./tools/make_hd_textures.sh --skip-upscale      # extract, convert and make fonts
#   ./tools/make_hd_textures.sh --crisp-fonts-only  # extract, convert and make fonts
#   ./tools/make_hd_textures.sh --fonts-only        # font assets only, no GPU or downloads
#   ./tools/make_hd_textures.sh --verify            # check the set and write hd_textures/verify_report.txt, nothing changed
#   ./tools/make_hd_textures.sh --remake            # the last report's missing and broken images made again
# Set PYTHON to select a Python interpreter.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
game=$root/game out=$root/hd_textures py=$root/tools/hd_textures
python=${PYTHON:-python3}
skip_upscale=0 crisp_fonts_only=0 fonts_only=0 verify=0 remake=0
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-upscale) skip_upscale=1 ;;
        --crisp-fonts-only) crisp_fonts_only=1 ;;
        --fonts-only) fonts_only=1 ;;
        --verify) verify=1 ;;
        --remake) remake=1 ;;
        --python) shift; [ $# -gt 0 ] || { echo '--python needs a value' >&2; exit 2; }; python=$1 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
step() { printf '\n==> %s\n' "$*"; }
ensure_numpy() {
    if ! "$python" -c 'import numpy' 2>/dev/null; then
        step 'Installing numpy (Python), for the sharp fonts'
        "$python" -m pip install --user numpy || true
        "$python" -c 'import numpy' 2>/dev/null
    fi
}

if [ "$fonts_only" = 1 ]; then
    if ! ensure_numpy || ! "$python" "$py/make_fonts_only.py" --game "$game" --out "$out" --controls "$root/controls.txt"; then
        echo 'warning: the button prompts and sharp fonts are not made; continuing with the game’s own fonts.' >&2
    fi
    exit 0
fi

archives=$out/archives converted=$out/converted/textures
# (TODO 281) F1's "Verify HD textures": the set checked and reported, nothing changed (upscale_textures.py --verify).
# It needs the verifier and the set itself only: no disc archives, no extraction or upscaling, no Real-ESRGAN download.
if [ "$verify" = 1 ]; then
    step 'Verifying the HD textures (nothing is changed)'
    "$python" -c 'import PIL' 2>/dev/null || { echo "Python's Pillow is missing: pip install pillow" >&2; exit 1; }
    [ -d "$converted" ] || { echo "no converted textures in $converted: run ./tools/make_hd_textures.sh first" >&2; exit 1; }
    [ -d "$out/upscaled" ] || { echo "no HD textures in $out/upscaled: run ./tools/make_hd_textures.sh first" >&2; exit 1; }
    "$python" "$py/upscale_textures.py" "$converted" --out "$out/upscaled" --verify --hd-dir "$out" || exit $?
    exit 0
fi
zips=() inputs=()
for name in TFA TFA2 TA_XTRAS SOUNDE; do
    zips+=("$game/$name.ZIP")
    [ -f "$game/$name.ZIP" ] || { echo "$game/$name.ZIP not found: run build.sh --iso <your disc> first" >&2; exit 1; }
done
"$python" -c 'import PIL' 2>/dev/null || { echo "Python's Pillow is missing: pip install pillow" >&2; exit 1; }
if [ ! -d "$archives/TFA" ]; then
    step "Extracting the disc's archives into $archives"
    "$python" "$py/extract_pk_archive.py" "${zips[@]}" --out "$archives"
fi
if [ ! -d "$converted/TFA" ]; then
    step "Converting the textures to PNG into $converted"
    for name in TFA TFA2 TA_XTRAS SOUNDE; do
        [ ! -d "$archives/$name" ] || inputs+=("$archives/$name")
    done
    "$python" "$py/itf_to_png.py" "${inputs[@]}" --out "$converted" --all-palettes
fi
# (TODO 281) F1's "Remake N broken images": the last verify's missing and broken ones removed, then made again below.
if [ "$remake" = 1 ]; then
    step 'Removing the images the last verify found missing or broken (they are made again below)'
    "$python" "$py/upscale_textures.py" "$converted" --out "$out/upscaled" --forget-broken --hd-dir "$out"
fi
crisp_fonts() {
    if ! ensure_numpy; then
        echo "warning: Python's numpy could not be installed: sharp fonts are not made." >&2
        return
    fi
    step "Making the sharp fonts and button symbols into $out/crisp_fonts"
    local args=(--archives "$archives" --converted "$converted" --out "$out/crisp_fonts")
    [ ! -d "$out/upscaled" ] || args+=(--upscaled "$out/upscaled")
    [ ! -f "$root/controls.txt" ] || args+=(--controls "$root/controls.txt")
    "$python" "$py/make_crisp_fonts.py" "${args[@]}"
}
if [ "$crisp_fonts_only" = 1 ] || [ "$skip_upscale" = 1 ]; then
    crisp_fonts
    exit 0
fi

esr=$out/realesrgan exe=$out/realesrgan/realesrgan-ncnn-vulkan
if [ ! -f "$exe" ]; then
    step 'Downloading Real-ESRGAN ncnn-vulkan v0.2.5.0'
    mkdir -p "$esr"
    zip=$out/realesrgan.zip
    curl -fL 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip' -o "$zip"
    unzip -oq "$zip" -d "$esr"
    rm -f "$zip"
    [ -f "$exe" ] || { echo 'Real-ESRGAN download did not contain the executable' >&2; exit 1; }
    chmod +x "$exe"
fi
# A set made before the maker checked its images, or before its newer checks: every image checked once, broken ones
# removed and made again below (make_hd_textures.ps1's step; the game reads the same mark).
if [ "$remake" = 0 ] && [ -f "$out/upscaled/upscale_manifest.jsonl" ] && [ ! -f "$out/.images_checked_3" ]; then
    step 'Checking the HD textures made so far (a few minutes)'
    "$python" "$py/upscale_textures.py" "$converted" --out "$out/upscaled" --check-colours
    printf 'checked %s\n' "$(date +%Y-%m-%dT%H:%M:%S)" >"$out/.images_checked_3"
fi
# (0.7.0) The preset chosen (hd_options.json, as the setup and F1 > Graphics > HD texture preset write it): Fast,
# Balanced or Best; Balanced when none is chosen. Images another preset made larger are shrunk to it on the CPU.
quality=$("$python" - "$out/hd_options.json" <<'PY'
import json, sys
try:
    q = str(json.load(open(sys.argv[1], encoding="utf-8")).get("quality", ""))
except (OSError, ValueError):
    q = ""
print(q if q in ("fast", "balanced", "best") else "balanced")
PY
)
step "Upscaling into $out/upscaled (resumable, preset: $quality)"
log=$(mktemp "$out/upscale-log.XXXXXX")
trap 'rm -f "$log"' EXIT
set +e
"$python" "$py/upscale_textures.py" "$converted" --out "$out/upscaled" --work "$out/work" --exe "$exe" --quality "$quality" 2>&1 | tee "$log"
status=("${PIPESTATUS[@]}")
set -e
code=${status[0]}
[ "${status[1]}" -eq 0 ] || exit "${status[1]}"
# (0.7.0) Exit 5: the graphics driver was reset even with the smallest pieces, so this run stopped with images left.
# What is made is kept and used; the set is not marked done, and running this again carries on.
stopped_by_resets=0
if [ "$code" -eq 5 ]; then
    echo 'warning: the graphics driver was reset while the HD textures were made, even in the smallest pieces: this run stopped with some left to make; running this again carries on.' >&2
    stopped_by_resets=1
    code=0
fi
if [ "$code" -ne 0 ]; then
    if [ "$code" -eq 1 ] && grep -Eq '^upscaled [0-9]+/[0-9]+ uniques' "$log"; then
        echo 'warning: some textures could not be upscaled; running again retries them.' >&2
    else
        echo "upscale_textures.py failed with exit code $code" >&2
        exit "$code"
    fi
fi
if [ ! -f "$out/upscaled_precolour/colour_gains.json" ]; then
    step "Matching the HD textures' colours to the game's (one time, about 10 minutes)"
    [ ! -d "$out/upscaled_bc7_cache" ] || echo 'Changed textures will be compressed again on the next visit to each level.'
else
    step "Matching the upscaled images' colour to the game's textures"
fi
"$python" "$py/match_colour.py" --upscaled "$out/upscaled" --native "$converted"
crisp_fonts
if [ "$stopped_by_resets" = 1 ]; then
    printf '
HD textures made so far are in %s; run this again for the rest.
' "$out"
    exit 0
fi
# The upscaler stops at "upscaled"; only the whole pipeline can mark the set done.
"$python" - "$out/upscale_status.json" <<'PY'
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

path = Path(sys.argv[1])
status = json.loads(path.read_text(encoding="utf-8"))
status["state"] = "done"
status["updated"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
tmp = path.with_name(path.name + ".tmp")
tmp.write_text(json.dumps(status, indent=1), encoding="utf-8")
os.replace(tmp, path)
PY
printf '\nDone. HD textures and fonts are in %s.\n' "$out"
