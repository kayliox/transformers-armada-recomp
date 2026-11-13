#!/usr/bin/env bash
# Builds vkd3d (Wine's Direct3D 12 on Vulkan: the native renderer on Linux) once, statically, into out/vkd3d-<version>
# (build.sh step 5, and the Linux CI). A pinned release, checked by its SHA-256. Prints the folder when it is there;
# prints nothing (and says why) when its tools are missing or it does not build: the game then has the emulated
# renderer only.
#   tools/build_vkd3d.sh
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
version=2.1 sha=7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a
prefix=$root/out/vkd3d-$version
say() { printf '\n== %s\n' "$*" >&2; }

if [ ! -f "$prefix/lib/libvkd3d-utils.a" ]; then
    missing=()
    for tool in curl bison flex perl; do command -v "$tool" >/dev/null || missing+=("$tool"); done
    perl -MJSON -e 1 2>/dev/null || missing+=("Perl's JSON module")
    printf '#include <vulkan/vulkan.h>\n#include <spirv/unified1/spirv.h>\n' | "${CC:-cc}" -E -x c - >/dev/null 2>&1 ||
        missing+=("the Vulkan and SPIR-V headers")
    if [ "${#missing[@]}" -gt 0 ]; then
        list=$(printf '%s, ' "${missing[@]}")
        say "No native renderer: vkd3d needs ${list%, } (the emulated renderer only; build.sh --no-vkd3d: no notice)"
        exit 0
    fi
    say "Building vkd3d $version (the native renderer's Direct3D 12 on Vulkan)"
    src=$root/out/vkd3d-src
    rm -rf "$src" && mkdir -p "$src"
    # (-fno-lto: GCC's LTO objects cannot be read by every linker - zig's lld among them)
    if (curl -fsSL "https://dl.winehq.org/vkd3d/source/vkd3d-$version.tar.xz" -o "$src/vkd3d.tar.xz" &&
        [ "$(sha256sum "$src/vkd3d.tar.xz" | cut -d' ' -f1)" = "$sha" ] &&
        tar -xJf "$src/vkd3d.tar.xz" -C "$src" --strip-components=1 &&
        cd "$src" && CFLAGS="-O2 -fPIC -fno-lto" SONAME_LIBVULKAN=libvulkan.so.1 ./configure --prefix="$prefix" \
            --libdir="$prefix/lib" --enable-static --disable-shared --disable-tests --disable-demos --disable-doxygen-doc \
            --without-opengl --without-xcb --without-ncurses >configure.log 2>&1 &&
        make -j"$(nproc)" >make.log 2>&1 && make install >>make.log 2>&1); then
        rm -rf "$src"
    else
        say "vkd3d did not build (logs in $src): the emulated renderer only"
        exit 0
    fi
fi
echo "$prefix"
