#!/usr/bin/env bash
# Sets the game up on Linux from your own disc image (setup.ps1's job):
#   1. Checks the tools the build needs and says how to install the missing ones.
#   2. Builds the game (build.sh): extracts your disc, translates its code, compiles the executable.
#   3. Remembers your disc path in .setup.json (git-ignored) for update.sh.
# Everything made from your disc stays in this folder and is git-ignored. Never commit or share it.
#
#   ./setup.sh --iso "/path/to/Transformers (USA).iso"
#   ./setup.sh            # again later: reuses the path it remembered
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
state=$root/.setup.json
iso=""
while [ $# -gt 0 ]; do
    case "$1" in
    --iso) iso=$2; shift 2 ;;
    -h | --help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
step() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nsetup: %s\n' "$*" >&2; exit 1; }
usa_size=2900951040 # Transformers (USA), SLUS-20668: the only disc the build accepts (it checks the executable inside)

command -v python3 >/dev/null || fail "Python 3 is missing: install python3 from your distribution's packages"
# .setup.json: { "Iso": ..., "SetUpAt": ... } as setup.ps1 writes it (the game and transformers/version.cmake read it)
json() { python3 - "$state" "$@" <<'EOF'
import json, sys
path, *pairs = sys.argv[1:]
try:
    data = json.load(open(path))
except Exception:
    data = {}
if not pairs:
    print(data.get("Iso", ""))
for pair in pairs:
    key, value = pair.split("=", 1)
    data[key] = value
if pairs:
    json.dump(data, open(path, "w"), indent=2)
EOF
}
if [ -n "$iso" ]; then
    [ -f "$iso" ] || fail "no such file: $iso"
    json "Iso=$(readlink -f "$iso")"
fi
iso=$(json)
if [ ! -f "$root/game/SLUS_206.68" ]; then
    [ -n "$iso" ] || fail "first setup: give your disc image, e.g.  ./setup.sh --iso \"/path/to/Transformers (USA).iso\""
    [ -f "$iso" ] || fail "the disc image is gone: $iso (give it again with --iso)"
    size=$(stat -c %s "$iso")
    [ "$size" = "$usa_size" ] || echo "warning: $iso is not the size of the USA release (SLUS-20668, $usa_size bytes); the build checks the disc's executable."
fi

step "Checking what the build needs"
missing=()
for tool in git cmake ninja pkg-config; do command -v "$tool" >/dev/null || missing+=("$tool"); done
command -v "${CXX:-c++}" >/dev/null || command -v g++ >/dev/null || command -v clang++ >/dev/null || missing+=("a C++ compiler")
if command -v pkg-config >/dev/null; then
    for lib in x11 gl libavcodec libavformat libavutil libswresample libswscale; do
        pkg-config --exists "$lib" || missing+=("$lib (development package)")
    done
fi
if command -v cmake >/dev/null; then
    version=$(cmake --version | awk 'NR==1 {print $3}')
    [ "$(printf '%s\n3.21\n' "$version" | sort -V | head -n 1)" = 3.21 ] || missing+=("CMake 3.21 or newer (this one is $version)")
fi
if [ "${#missing[@]}" -gt 0 ]; then
    printf 'missing:\n'
    printf '  - %s\n' "${missing[@]}"
    if [ -f /etc/os-release ] && grep -q '^ID=steamos' /etc/os-release; then
        cat <<'EOF'
SteamOS keeps its system read-only. Build inside a container instead, e.g.:
  distrobox create -n tfa-build -i ubuntu:24.04 && distrobox enter tfa-build
then install the packages below in it and run ./setup.sh there (the game itself runs on SteamOS).
EOF
    fi
    cat <<'EOF'
Ubuntu / Debian:
  sudo apt install git cmake ninja-build g++ pkg-config python3 libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
    libxcursor-dev libxi-dev libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev \
    curl bison flex libjson-perl libvulkan-dev spirv-headers
Fedora:
  sudo dnf install git cmake ninja-build gcc-c++ pkgconf python3 mesa-libGL-devel libX11-devel libXrandr-devel \
    libXinerama-devel libXcursor-devel libXi-devel ffmpeg-free-devel curl bison flex perl-JSON vulkan-headers \
    spirv-headers-devel
Arch:
  sudo pacman -S git cmake ninja gcc pkgconf python mesa libx11 libxrandr libxinerama libxcursor libxi ffmpeg \
    curl bison flex perl-json vulkan-headers spirv-headers
(curl, bison, flex, Perl's JSON, the Vulkan and SPIR-V headers: for the native renderer, built through vkd3d.)
Then run ./setup.sh again.
EOF
    exit 1
fi

# The check that refuses commits with game data (tools/check_no_game_data.py), for anyone who commits.
[ -d "$root/.githooks" ] && git -C "$root" config core.hooksPath .githooks || true

step "Building the game (build.sh)"
if [ -f "$root/game/SLUS_206.68" ]; then "$root/build.sh"; else "$root/build.sh" --iso "$iso"; fi

for example in "$root/overlay/settings.example.ini" "$root/settings.example.ini"; do
    if [ ! -f "$root/settings.ini" ] && [ -f "$example" ]; then cp "$example" "$root/settings.ini"; fi
done
json "SetUpAt=$(git -C "$root" rev-parse HEAD)"
printf '\nReady. Play with ./play.sh in %s (for Steam: add play.sh as a non-Steam game). Settings: F1 in the game.\n' "$root"
