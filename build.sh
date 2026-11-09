#!/usr/bin/env bash
# Builds the Transformers (PS2) recompilation on Linux from your own disc image: build.ps1's steps.
#   1. Extracts the ISO into game/ (skipped if already extracted).
#   2. Builds PS2Recomp's recompiler and analyzer.
#   3. Finds the functions and writes the recompiler config (work/).
#   4. Translates the game's MIPS code to C++ (work/generated/), only when what it reads changed.
#   5. Builds vkd3d (Wine's Direct3D 12 on Vulkan, for the native renderer) once into out/vkd3d-<version>, when its
#      tools are installed; without it the game has the emulated renderer only.
#   6. Builds out/build/transformers/TransformersRecomp.
# Needs: CMake 3.21+, Ninja, a C/C++ compiler (CC/CXX are honoured), Python 3, git, pkg-config, and the development
# packages for X11/OpenGL (raylib's GLFW) and FFmpeg (libavcodec, libavformat, libavutil, libswresample, libswscale).
# The native renderer also needs curl, bison, flex, Perl's JSON module and the Vulkan and SPIR-V headers.
#
#   ./build.sh --iso "Transformers (USA).iso"
#   ./build.sh --skip-recompile      # only rebuild the executable after runtime or port changes
#   ./build.sh --patches-only        # only apply the PS2Recomp patch series (CI: no disc)
#   ./build.sh --no-vkd3d            # the emulated renderer only (no vkd3d build)
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
iso="" config=RelWithDebInfo skip_recompile=0 no_patches=0 patches_only=0 no_vkd3d=0
while [ $# -gt 0 ]; do
    case "$1" in
    --iso) iso=$2; shift 2 ;;
    --config) config=$2; shift 2 ;;
    --skip-recompile) skip_recompile=1; shift ;;
    --no-patches) no_patches=1; shift ;;
    --patches-only) patches_only=1; shift ;;
    --no-vkd3d) no_vkd3d=1; shift ;;
    -h | --help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

game=$root/game work=$root/work build=$root/out/build tools_build=$root/out/tools tools=$root/transformers/tools
ps2recomp=$root/PS2Recomp elf=SLUS_206.68
python=${PYTHON:-python3}
step() { printf '\n== %s\n' "$*"; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

[ -f "$ps2recomp/ps2xRuntime/CMakeLists.txt" ] || git -C "$root" submodule update --init --recursive

# PS2Recomp patch series, as build.ps1 applies it: as a whole, recorded in out/ps2recomp-patches.txt, the submodule
# reset to its commit when the series changed. Files the new series leaves as they were keep their times, so one new
# runtime patch does not recompile the whole game.
if [ "$no_patches" = 0 ]; then
    mapfile -t patches < <(find "$root/patches/ps2recomp" -maxdepth 1 -name '*.patch' | LC_ALL=C sort)
    record=$root/out/ps2recomp-patches.txt
    series=$(for p in "${patches[@]}"; do printf '%s %s\n' "$(basename "$p")" "$(sha "$p" | tr a-f A-F)"; done)
    if [ "${#patches[@]}" -gt 0 ] && [ "$series" != "$(cat "$record" 2>/dev/null)" ]; then
        step "Applying the PS2Recomp patch series (${#patches[@]} patches) to a clean submodule"
        keep=$(mktemp -d)
        trap 'rm -rf "$keep"' EXIT
        git -C "$ps2recomp" ls-files -z -m -o --exclude-standard | while IFS= read -r -d '' rel; do
            [ -f "$ps2recomp/$rel" ] || continue
            mkdir -p "$keep/$(dirname "$rel")"
            cp -p "$ps2recomp/$rel" "$keep/$rel"
        done
        git -C "$ps2recomp" checkout -q -- .
        git -C "$ps2recomp" clean -fdq
        for p in "${patches[@]}"; do
            # (some patches were written on Windows with CRLF lines; the submodule's files are LF here)
            tr -d '\r' <"$p" | git -C "$ps2recomp" apply --whitespace=nowarn -
        done
        (cd "$keep" && find . -type f -print0) | while IFS= read -r -d '' rel; do
            if cmp -s "$keep/$rel" "$ps2recomp/$rel"; then touch -r "$keep/$rel" "$ps2recomp/$rel"; fi
        done
        mkdir -p "$root/out"
        printf '%s' "$series" >"$record"
    fi
fi
[ "$patches_only" = 1 ] && exit 0

# 1. Game data
if [ ! -f "$game/$elf" ]; then
    if [ -z "$iso" ]; then
        iso=$(find "$root" -maxdepth 1 -iname '*.iso' | head -n 1)
        [ -n "$iso" ] || { echo "No game data. Pass your disc image: ./build.sh --iso \"Transformers (USA).iso\"" >&2; exit 1; }
    fi
    step "Extracting $iso"
    "$python" "$tools/extract_iso.py" "$iso" "$game"
fi

generator=(-G Ninja -DCMAKE_BUILD_TYPE="$config")

# 2-4. Tools, then the translation when its inputs changed (work/translation-inputs.txt, as build.ps1's)
if [ "$skip_recompile" = 0 ]; then
    step "Building recompiler tools"
    cmake -S "$ps2recomp" -B "$tools_build" "${generator[@]}" \
        -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF >/dev/null
    cmake --build "$tools_build" --target ps2_recomp ps2_analyzer
    analyzer=$tools_build/ps2xAnalyzer/ps2_analyzer recomp=$tools_build/ps2xRecomp/ps2_recomp
    inputs=$(
        echo "scheme linux-1 $config"
        for f in "$analyzer" "$recomp" "$game/$elf" "$tools"/*.py; do echo "$(basename "$f") $(sha "$f")"; done
        find "$root/transformers/config" -type f | LC_ALL=C sort | while read -r f; do echo "config/${f#"$root"/transformers/config/} $(sha "$f")"; done
        env | grep -E '^(PS2X_CODEGEN|PS2RECOMP_)' | LC_ALL=C sort | sed 's/^/env /' || true
    )
    if [ -f "$work/generated/register_functions.cpp" ] && [ "$inputs" = "$(cat "$work/translation-inputs.txt" 2>/dev/null)" ]; then
        step "The game's code is translated as before"
    else
        rm -f "$work/translation-inputs.txt"
        mkdir -p "$work"
        (
            cd "$work"
            cp "$game/$elf" .
            step "Scanning for SDK functions"
            "$analyzer" "$elf" analyzer.toml >analyzer.log 2>&1
            step "Recovering function boundaries"
            "$python" "$tools/find_functions.py" "$elf" functions.csv --names analyzer.toml --jump-tables jump_tables.json
            "$python" "$tools/make_config.py" "$work" transformers.toml
            step "Translating MIPS to C++"
            rm -rf staging
            "$recomp" transformers.toml >recomp.log 2>&1
            step "Updating generated sources"
            "$python" "$tools/sync_generated.py" staging generated batches
        )
        printf '%s' "$inputs" >"$work/translation-inputs.txt"
    fi
fi

# 5. vkd3d for the native renderer: a pinned release (checked by its SHA-256), static, built once.
vkd3d_version=2.1 vkd3d_sha=7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a
vkd3d=$root/out/vkd3d-$vkd3d_version
vkd3d_arg=-DTF_VKD3D_DIR=
if [ "$no_vkd3d" = 0 ]; then
    if [ ! -f "$vkd3d/lib/libvkd3d-utils.a" ]; then
        missing=()
        for tool in curl bison flex perl; do command -v "$tool" >/dev/null || missing+=("$tool"); done
        perl -MJSON -e 1 2>/dev/null || missing+=("Perl's JSON module")
        printf '#include <vulkan/vulkan.h>\n#include <spirv/unified1/spirv.h>\n' | "${CC:-cc}" -E -x c - >/dev/null 2>&1 ||
            missing+=("the Vulkan and SPIR-V headers")
        if [ "${#missing[@]}" -gt 0 ]; then
            list=$(printf '%s, ' "${missing[@]}")
            step "No native renderer: vkd3d needs ${list%, } (the emulated renderer only; --no-vkd3d: no notice)"
        else
            step "Building vkd3d $vkd3d_version (the native renderer's Direct3D 12 on Vulkan)"
            src=$root/out/vkd3d-src
            rm -rf "$src" && mkdir -p "$src"
            if (curl -fsSL "https://dl.winehq.org/vkd3d/source/vkd3d-$vkd3d_version.tar.xz" -o "$src/vkd3d.tar.xz" &&
                [ "$(sha "$src/vkd3d.tar.xz")" = "$vkd3d_sha" ] && tar -xJf "$src/vkd3d.tar.xz" -C "$src" --strip-components=1 &&
                cd "$src" && CFLAGS="-O2 -fPIC -fno-lto" SONAME_LIBVULKAN=libvulkan.so.1 ./configure --prefix="$vkd3d" --libdir="$vkd3d/lib" \
                    --enable-static --disable-shared --disable-tests --disable-demos --disable-doxygen-doc \
                    --without-opengl --without-xcb --without-ncurses >configure.log 2>&1 &&
                make -j"$(nproc)" >make.log 2>&1 && make install >>make.log 2>&1); then
                rm -rf "$src"
            else
                step "vkd3d did not build (logs in $src): the emulated renderer only"
            fi
        fi
    fi
    [ -f "$vkd3d/lib/libvkd3d-utils.a" ] && vkd3d_arg=-DTF_VKD3D_DIR=$vkd3d
fi

# 6. The game
step "Configuring game build"
# (TF_CMAKE_ARGS: more configure arguments, word-split - e.g. a packager's library paths)
# shellcheck disable=SC2086
cmake -S "$root" -B "$build" "${generator[@]}" "$vkd3d_arg" ${TF_CMAKE_ARGS:-} >/dev/null
step "Building TransformersRecomp ($config) - the first build compiles ~22,000 generated functions"
cmake --build "$build" --target TransformersRecomp
step "Built $build/transformers/TransformersRecomp"
