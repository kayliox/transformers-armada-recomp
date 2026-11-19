#!/usr/bin/env bash
# (TODO 439) Frees disk space after the game is built: deletes the build's in-between files, which playing never needs.
# tools/clean_build.ps1's job on Linux.
#
# Kept always: the game itself, out/build/transformers/ (TransformersRecomp, the libraries bundled beside it) without
# its CMakeFiles/ folder and static libraries, and out/vkd3d-* (built once for the native renderer).
#   temp   work/staging: the recompiler's raw output, copied into work/generated and written again by every
#          translation. Nothing reads it afterwards: deleting it changes nothing.
#   build  the rest of out/build (the compiler's object files), out/tools (the recompiler's own build) and work/ (the
#          translated C++ code). Updates use them to rebuild only what changed: without them the NEXT update
#          rebuilds the whole game once (about as long as the first build) and leaves them behind again.
#
#   tools/clean_build.sh --report        deletes nothing; one line per kind: <kind>|<bytes>|<files> (game, temp, build)
#   tools/clean_build.sh --level temp    the temporary files only (no cost)
#   tools/clean_build.sh --level all     the temporary and the in-between files (the next update is a full rebuild)
# Never while the game runs from this folder. After --level all it leaves out/build-cleaned.txt (until the next build).
set -euo pipefail

port=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
level="" report=0
while [ $# -gt 0 ]; do
    case "$1" in
    --level) level=$2; shift 2 ;;
    --report) report=1; shift ;;
    --port) port=$(cd "$2" && pwd); shift 2 ;;
    -h | --help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
case "$level" in "" | temp | all) ;; *) echo "--level: temp or all" >&2; exit 2 ;; esac
out=$port/out build=$port/out/build tools=$port/out/tools work=$port/work game=$port/out/build/transformers

# The paths of each kind, one per line.
kind_game() { [ -x "$game/TransformersRecomp" ] && echo "$game" || true; }
kind_temp() { [ -d "$work/staging" ] && echo "$work/staging" || true; }
kind_build() {
    [ -d "$tools" ] && echo "$tools"
    if [ -d "$work" ]; then find "$work" -mindepth 1 -maxdepth 1 ! -path "$work/staging"; fi
    if [ -d "$build" ]; then
        if [ -x "$game/TransformersRecomp" ]; then
            find "$build" -mindepth 1 -maxdepth 1 ! -path "$game"
            find "$game" -mindepth 1 -maxdepth 1 \( -name CMakeFiles -o -name '*.a' \)
        else
            echo "$build"
        fi
    fi
    return 0
}
# The game's own files: its folder without what kind_build takes from it.
size_game() {
    [ -x "$game/TransformersRecomp" ] || { echo "0|0"; return; }
    find "$game" -path "$game/CMakeFiles" -prune -o -type f ! -name '*.a' -printf '%s\n' | awk '{ s += $1; n++ } END { printf "%d|%d\n", s, n }'
}
size_of() { # paths on stdin -> "<bytes>|<files>"
    local s=0 n=0 b
    while IFS= read -r p; do
        [ -n "$p" ] && [ -e "$p" ] || continue
        while IFS= read -r b; do s=$((s + b)); n=$((n + 1)); done < <(find "$p" -type f -printf '%s\n')
    done
    echo "$s|$n"
}

if [ "$report" = 1 ] || [ -z "$level" ]; then
    echo "game|$(size_game)"
    echo "temp|$(kind_temp | size_of)"
    echo "build|$(kind_build | size_of)"
    exit 0
fi

# Not while the game runs from here.
for e in /proc/[0-9]*/exe; do
    exe=$(readlink "$e" 2>/dev/null || true)
    case "$exe" in "$port"/*TransformersRecomp*) echo "clean_build: the game is running from this folder: close it first" >&2; exit 2 ;; esac
done

doomed=$(kind_temp; if [ "$level" = all ]; then kind_build; fi)
freed=$(printf '%s\n' "$doomed" | size_of)
failed=0
while IFS= read -r p; do
    [ -n "$p" ] && [ -e "$p" ] || continue
    rm -rf -- "$p" || failed=1
done <<<"$doomed"
if [ "$level" = all ]; then
    mkdir -p "$out"
    printf 'in-between files deleted %s; the next build rebuilds the whole game\n' "$(date '+%Y-%m-%dT%H:%M:%S')" >"$out/build-cleaned.txt"
fi
bytes=${freed%%|*} files=${freed##*|}
what="the build's temporary files"
[ "$level" = all ] && what="the build's temporary and in-between files"
printf 'Deleted %s: %s.%s GB freed (%s files).\n' "$what" $((bytes / 1073741824)) $(((bytes % 1073741824) * 10 / 1073741824)) "$files"
[ "$level" = all ] && echo "The game plays as before. The next update rebuilds the whole game once (about as long as the first build)."
exit $failed
