#!/usr/bin/env bash
# Sets the game up on Linux from your own disc image (setup.ps1's job):
#   1. Checks the tools the build needs and says how to install the missing ones.
#   2. Builds the game (build.sh): extracts your disc, translates its code, compiles the executable.
#   3. Remembers your disc path in .setup.json (git-ignored) for update.sh.
# Everything made from your disc stays in this folder and is git-ignored. Never commit or share it.
#
#   ./setup.sh --iso "/path/to/Transformers (USA).iso"
#   ./setup.sh            # again later: reuses the path it remembered
#   ./setup.sh --gentle on   # (TODO 310) easy on the PC: the build on half the processor threads at a lower priority
#                            # (kept in hd_textures/hd_options.json, so update.sh goes easy too; --gentle off undoes it)
#   ./setup.sh --packages ubuntu   # the packages the build needs (ubuntu, debian, fedora or arch), on one line: for a
#                                  # setup program or a container to install, nothing else done
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
state=$root/.setup.json
iso="" gentle="" packages=""
while [ $# -gt 0 ]; do
    case "$1" in
    --iso) iso=$2; shift 2 ;;
    --gentle) gentle=$2; shift 2 ;;
    --packages) packages=$2; shift 2 ;;
    -h | --help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
# The build's packages on each distribution: the message below lists them, --packages prints one list (the Linux
# tfa-setup installs it in its build container on SteamOS and Bazzite). curl, bison, flex, Perl's JSON, the Vulkan and
# SPIR-V headers are for the native renderer, built through vkd3d.
ubuntu_packages="git cmake ninja-build g++ pkg-config python3 libgl-dev libx11-dev libxrandr-dev libxinerama-dev
    libxcursor-dev libxi-dev libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev
    curl bison flex libjson-perl libvulkan-dev spirv-headers"
fedora_packages="git cmake ninja-build gcc-c++ pkgconf python3 mesa-libGL-devel libX11-devel libXrandr-devel
    libXinerama-devel libXcursor-devel libXi-devel ffmpeg-free-devel curl bison flex perl-JSON vulkan-headers
    spirv-headers-devel"
arch_packages="git cmake ninja gcc pkgconf python mesa libx11 libxrandr libxinerama libxcursor libxi ffmpeg
    curl bison flex perl-json vulkan-headers spirv-headers"
if [ -n "$packages" ]; then
    case "$packages" in
    ubuntu | debian) list=$ubuntu_packages ;;
    fedora) list=$fedora_packages ;;
    arch) list=$arch_packages ;;
    *) echo "--packages: ubuntu, debian, fedora or arch" >&2; exit 2 ;;
    esac
    echo $list # (unquoted: one line, one space between)
    exit 0
fi
# (TODO 310) Each step with its time to TF_STEP_LOG when the setup or the update set it, synced to the disk at once: it
# names the step even when the PC went off (tools/step_log.ps1's, on Windows).
steplog() { if [ -n "${TF_STEP_LOG:-}" ]; then printf '%s  %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >>"$TF_STEP_LOG" && sync "$TF_STEP_LOG" 2>/dev/null; fi; return 0; }
step() { printf '\n==> %s\n' "$*"; steplog "setup: $*"; }
if [ -z "${TF_STEP_LOG:-}" ]; then export TF_STEP_LOG="$root/setup_steps.log"; : >"$TF_STEP_LOG"; fi
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
    # (each list as it is written above: its lines end in " \" here, for copying)
    shown() { printf '%s\n' "$1" | sed -e '$!s/$/ \\/'; }
    cat <<EOF
Ubuntu / Debian:
  sudo apt install $(shown "$ubuntu_packages")
Fedora:
  sudo dnf install $(shown "$fedora_packages")
Arch:
  sudo pacman -S $(shown "$arch_packages")
(curl, bison, flex, Perl's JSON, the Vulkan and SPIR-V headers: for the native renderer, built through vkd3d.)
Then run ./setup.sh again.
EOF
    exit 1
fi

# The check that refuses commits with game data (tools/check_no_game_data.py), for anyone who commits.
[ -d "$root/.githooks" ] && git -C "$root" config core.hooksPath .githooks || true

# (TODO 310) "Go easy on my PC", kept in hd_textures/hd_options.json beside the HD textures' choices (the update reads it)
gentle_on=$(python3 - "$root/hd_textures/hd_options.json" "$gentle" <<'EOF'
import json, os, sys
path, want = sys.argv[1:]
data = {}
try:
    data = json.load(open(path))
except Exception:
    pass
if want in ("on", "off"):
    data["gentle"] = want == "on"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump(data, open(path, "w"), indent=4)
print(1 if data.get("gentle") else 0)
EOF
)
build_args=()
[ "$gentle_on" = 1 ] && build_args+=(--gentle)
step "Building the game (build.sh)"
if [ -f "$root/game/SLUS_206.68" ]; then "$root/build.sh" ${build_args[@]+"${build_args[@]}"}; else "$root/build.sh" --iso "$iso" ${build_args[@]+"${build_args[@]}"}; fi

for example in "$root/overlay/settings.example.ini" "$root/settings.example.ini"; do
    if [ ! -f "$root/settings.ini" ] && [ -f "$example" ]; then cp "$example" "$root/settings.ini"; fi
done
json "SetUpAt=$(git -C "$root" rev-parse HEAD)"
steplog "setup: ready"
printf '\nReady. Play with ./play.sh in %s (for Steam: add play.sh as a non-Steam game). Settings: F1 in the game.\n' "$root"
