#!/usr/bin/env bash
# Brings your copy up to date with the repository and rebuilds what the new changes need (update.ps1's job on Linux).
#   1. Fetches the repository. If nothing is new it says so and stops (--force rebuilds anyway).
#   2. Pulls the new commits and updates the PS2Recomp submodule. Your own work in the copy (commits of your own,
#      changed files, new files that are not git-ignored) is first committed, all of it, to a branch of its own named
#      like my-changes-2026-11-01-1430; ignored files (game data, builds, settings.ini, saves, Mods/) are never touched.
#   3. Rebuilds the game with build.sh when the changes need it (it translates the game's code again only when the
#      recompiler or what it reads changed).
# The game's own "Update now" (F1 menu, About) runs this with --wait-pid (the game closes first) and --play (the game
# starts again afterwards, where it was). Everything it prints also goes to update.log.
#
#   ./update.sh
#   ./update.sh --check-only
set -uo pipefail

port=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$port state=$port/.setup.json states=$port/states result_file=$port/states/update_result.txt
force=0 check_only=0 wait_pid=0 play=0
while [ $# -gt 0 ]; do
    case "$1" in
    --force) force=1; shift ;;
    --check-only) check_only=1; shift ;;
    --wait-pid) wait_pid=$2; shift 2 ;;
    --play) play=1; shift ;;
    --stash-local-changes) shift ;; # (kept for callers that pass it: your own work is always kept on a branch)
    -h | --help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
exec > >(tee -a "$port/update.log") 2>&1
start_time=$SECONDS

# (TODO 310) Each step with its time to TF_STEP_LOG when the setup or the update set it, synced to the disk at once: it
# names the step even when the PC went off (tools/step_log.ps1's, on Windows).
steplog() { if [ -n "${TF_STEP_LOG:-}" ]; then printf '%s  %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >>"$TF_STEP_LOG" && sync "$TF_STEP_LOG" 2>/dev/null; fi; return 0; }
step() { printf '\n==> %s\n' "$*"; steplog "update: $*"; }
if [ -z "${TF_STEP_LOG:-}" ]; then export TF_STEP_LOG="$port/update_steps.log"; : >"$TF_STEP_LOG"; fi
g() { git -C "$repo" -c core.longpaths=true "$@"; }
result() {
    [ "$play" = 1 ] || return 0
    mkdir -p "$states" && printf '%s\n' "$1" >"$result_file"
}
start_game() {
    [ "$play" = 1 ] || return 0
    step "Starting the game"
    [ -x "$port/out/build/transformers/TransformersRecomp" ] || { echo "update: the game is missing ($port/out/build/transformers/TransformersRecomp)"; return 0; }
    setsid "$port/play.sh" >/dev/null 2>&1 </dev/null &
}
fail() {
    trap - ERR
    printf '\nupdate: %s\n' "$1"
    result "failed $1"
    start_game
    exit 1
}
kept_text() { echo "Your own changes were kept on the branch $1; nothing was lost."; }
# (anything unexpected ends like a failure: the result written, the game started again)
trap 'fail "unexpected error (line $LINENO)"' ERR

# Your own work kept on a branch (2. above), as update.ps1's Save-LocalWork: the backup commit is made in a temporary
# index first, so nothing in the folder moves until a branch holding all of it exists. Sets kept to the branch.
kept=""
save_local_work() {
    local branch=$1 theirs head on_branch commits changes base stamp name commit temp tree index path entry n
    theirs=$(g rev-parse --verify "origin/$branch") || fail "origin/$branch not found"
    head=$(g rev-parse HEAD)
    on_branch=0
    [ "$(g rev-parse --abbrev-ref HEAD)" = "$branch" ] && on_branch=1
    if g merge-base --is-ancestor "$theirs" HEAD; then return 0; fi # (nothing new to merge)
    commits=$(g rev-list --count "$theirs..HEAD")
    changes=$(g status --porcelain --ignore-submodules=dirty -- . ':(exclude)update.log') || fail "git status failed"
    if [ "$commits" = 0 ] && [ -z "$changes" ] && [ "$on_branch" = 1 ]; then return 0; fi
    base=$(g merge-base HEAD "$theirs") || fail "your copy shares no history with origin/$branch: cannot update it"
    while IFS= read -r path; do
        if [ -n "$path" ] && [ -e "$repo/$path" ] && g check-ignore -q --no-index "$path"; then
            fail "the update brings $path, and your copy has a file of its own there (an ignored one): move it away, then update again"
        fi
    done < <(g diff --name-only --diff-filter=A --no-renames "$head" "$theirs")
    stamp=my-changes-$(date +%Y-%m-%d-%H%M)
    name=$stamp
    for ((n = 2; ; n++)); do
        g rev-parse --verify --quiet "refs/heads/$name" >/dev/null || break
        name=$stamp-$n
    done
    echo "Your own work in this copy ($commits commit(s) of your own) is kept on the branch $name:"
    printf '%s\n' "$changes" | head -n 20
    commit=$head
    if [ -n "$changes" ]; then
        temp=$repo/$(g rev-parse --git-path tfa-keep.index)
        index=$repo/$(g rev-parse --git-path index)
        if [ -f "$index" ]; then cp "$index" "$temp"; fi
        if ! GIT_INDEX_FILE=$temp g add -A || ! GIT_INDEX_FILE=$temp g reset -q -- update.log ||
            ! tree=$(GIT_INDEX_FILE=$temp g write-tree); then
            rm -f "$temp"
            fail "your own changes could not be kept on a branch: nothing was changed"
        fi
        rm -f "$temp"
        local who=()
        [ -n "$(g config user.email)" ] || who=(-c user.name="Transformers setup" -c user.email=setup@transformers.local)
        commit=$(g "${who[@]}" commit-tree "$tree" -p "$head" -m "My own changes, kept before an update") ||
            fail "your own changes could not be kept on a branch (git commit-tree): nothing was changed"
    fi
    g branch "$name" "$commit" || fail "your own changes could not be kept on a branch (git branch): nothing was changed"
    # From here on all of it is on the branch: a step that fails stops the update, nothing is lost.
    g checkout -q -f -B "$branch" "$base" || fail "$(kept_text "$name") But the update could not go on (git checkout): it stops here"
    while IFS= read -r -d '' entry; do
        [ "${entry:0:3}" = "?? " ] || continue
        path=${entry:3}
        g cat-file -e "$commit:$path" || fail "$(kept_text "$name") But $path is not on that branch, so it is left as it is and the update stops here"
        rm -f -- "$repo/$path" || fail "$(kept_text "$name") But the update could not go on ($path): it stops here"
    done < <(g status --porcelain -z -uall --ignore-submodules=dirty -- . ':(exclude)update.log')
    kept_text "$name"
    mkdir -p "$states" && printf '%s\n' "$name" >"$states/update_kept.txt" # (the game says so at its next start)
    kept=$name
}

if [ "$play" = 1 ]; then export GIT_TERMINAL_PROMPT=0 GCM_INTERACTIVE=never; fi
if [ "$wait_pid" -gt 0 ] && kill -0 "$wait_pid" 2>/dev/null; then
    step "Waiting for the game to close"
    for _ in $(seq 1 1200); do
        kill -0 "$wait_pid" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$wait_pid" 2>/dev/null; then fail "the game did not close (process $wait_pid)"; fi
fi
[ -f "$state" ] || fail "run setup.sh first (this copy was never set up)"
built=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("SetUpAt",""))' "$state" 2>/dev/null || true)

step "Checking the repository for new changes"
g fetch --quiet origin || fail "git fetch failed (no network, or the repository cannot be reached)"
branch=$(g rev-parse --abbrev-ref HEAD)
old=$(g rev-parse HEAD)
new=$(g rev-parse "origin/$branch") || fail "origin/$branch not found"
if [ "$old" = "$new" ] && [ "$built" = "$old" ] && [ "$force" = 0 ]; then
    echo "Up to date (${old:0:7})."
    result "ok $old"
    start_game
    exit 0
fi
if [ "$old" != "$new" ]; then
    echo "New commits:"
    g log --oneline "$old..$new"
fi
if [ "$check_only" = 1 ]; then exit 0; fi
game_exe=$(readlink -f "$port/out/build/transformers/TransformersRecomp" || true)
for exe in /proc/[0-9]*/exe; do
    if [ -n "$game_exe" ] && [ "$(readlink "$exe" 2>/dev/null)" = "$game_exe" ]; then
        fail "the game is running: close it, then run update.sh again"
    fi
done

if [ "$old" != "$new" ]; then
    step "Pulling"
    save_local_work "$branch"
    g merge --ff-only --quiet "origin/$branch" || fail "the new changes could not be brought in (git merge failed)${kept:+. $(kept_text "$kept")}"
    g submodule update --init --recursive || fail "git submodule update failed"
fi

# What changed since the last build; documents, the Windows setup and these scripts need no rebuild.
since=${built:-$old}
need_build=$force
if [ "$force" = 0 ]; then
    while IFS= read -r path; do
        case "$path" in
        docs/* | *.md | installer/* | .github/* | .githooks/* | setup.ps1 | update.ps1 | Play.bat | setup.sh | update.sh | play.sh | \
            tools/setup_common.ps1 | tools/step_log.ps1 | tools/check_no_game_data.py | tools/check_publishable.py | LICENSE | .gitignore | .gitattributes) ;;
        *) need_build=1 ;;
        esac
    done < <(g diff --name-only "$since" HEAD)
fi
if [ "$need_build" = 1 ]; then
    # (TODO 310) "Go easy on my PC" (hd_textures/hd_options.json "gentle"): a build.sh from before it builds as it is
    build_args=()
    if grep -q -- '--gentle)' "$port/build.sh" &&
        python3 -c 'import json, sys; sys.exit(0 if json.load(open(sys.argv[1])).get("gentle") else 1)' "$port/hd_textures/hd_options.json" 2>/dev/null; then
        build_args+=(--gentle)
    fi
    step "Rebuilding the game (build.sh${build_args[*]+, going easy on the PC})"
    if [ "$force" = 1 ]; then rm -f "$port"/work/*-inputs.txt; fi # (the translation redone too)
    "$port/build.sh" ${build_args[@]+"${build_args[@]}"} || fail "the build failed (see above)"
else
    echo "Nothing to rebuild (documentation only)."
fi

now=$(g rev-parse HEAD)
python3 - "$state" "$now" <<'EOF'
import json, sys
data = json.load(open(sys.argv[1]))
data["SetUpAt"] = sys.argv[2]
json.dump(data, open(sys.argv[1], "w"), indent=2)
EOF
if [ -n "$kept" ]; then kept_text "$kept"; fi
printf '\nUp to date (%s), in %d s. Play with ./play.sh.\n' "${now:0:7}" "$((SECONDS - start_time))"
result "ok $now"
start_game
