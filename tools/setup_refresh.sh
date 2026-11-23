# (TODO 451) The setup program in the install folder (tfa-setup) brought up to date with the game: tools/setup_refresh.ps1's
# job on Linux, sourced by update.sh after a good update. The release of the setup made for the version the copy is at now
# (tag setup-v<VERSION>, or setup-v<VERSION>-<n>, the newest), its tfa-setup-linux checked against the size GitHub
# records and the SHA-256 in the release's SHA256SUMS.txt, then put in place by a rename (Linux lets a running program's
# file be replaced): the old one kept as tfa-setup.old. Nothing here can fail the update: every problem keeps the old one
# and says why in one line. TF_SETUP_REFRESH=0: off. TF_SETUP_REFRESH_SOURCE=<folder>: a stand-in release (tests).

tfa_update_setup_program() {
    local repo=$1 port=$2
    [ "${TF_SETUP_REFRESH:-1}" != 0 ] || return 0
    command -v python3 >/dev/null 2>&1 || return 0
    python3 - "$repo" "$port" <<'PYEOF' || true
import hashlib, json, os, re, subprocess, sys, urllib.request

repo, port = sys.argv[1], sys.argv[2]
root = os.path.dirname(os.path.abspath(repo))
cur = os.path.join(root, "tfa-setup")
say = lambda t: print("Setup program: " + t, flush=True)

def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()

def fetch(url, out=None, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "tfa-update", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read(65 * 1024 * 1024)
    if out is None:
        return data
    with open(out, "wb") as f:
        f.write(data)

try:
    if not os.path.isfile(cur):
        sys.exit(0)  # (a copy set up without the setup program)
    version = open(os.path.join(port, "VERSION")).read().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        sys.exit(0)
    source = os.environ.get("TF_SETUP_REFRESH_SOURCE", "")
    if source:
        j = json.load(open(os.path.join(source, "release.json")))
        releases = [{"tag_name": j["tag"], "draft": False, "prerelease": False,
                     "assets": [{"name": a["name"], "size": a["size"], "browser_download_url": os.path.join(source, a["name"])} for a in j["assets"]]}]
    else:
        url = subprocess.run(["git", "-C", repo, "remote", "get-url", "origin"], capture_output=True, text=True).stdout.strip()
        m = re.search(r"github\.com[/:]([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+?)(\.git)?/?$", url)
        if not m:
            sys.exit(0)
        releases = json.loads(fetch(f"https://api.github.com/repos/{m.group(1)}/{m.group(2)}/releases?per_page=40", timeout=30))
    best, best_n = None, 0
    for r in releases:
        mm = re.fullmatch(r"setup-v" + re.escape(version) + r"(-(\d+))?", r.get("tag_name", ""))
        if r.get("draft") or r.get("prerelease") or not mm:
            continue
        n = int(mm.group(2)) if mm.group(2) else 1
        if n > best_n:
            best, best_n = r, n
    if not best:
        say(f"no release of it for {version} yet: the one you have is kept")
        sys.exit(0)
    assets = {a["name"]: a for a in best.get("assets", [])}
    exe, sums = assets.get("tfa-setup-linux"), assets.get("SHA256SUMS.txt")
    if not exe or not sums:
        say(f"the release {best['tag_name']} has no checksums to check it with: the one you have is kept")
        sys.exit(0)
    size = int(exe["size"])
    if size <= 0 or size > 64 * 1024 * 1024:
        say(f"the release's file has an unexpected size ({size} bytes): the one you have is kept")
        sys.exit(0)
    get = (lambda a, out=None: (open(a["browser_download_url"], "rb").read() if out is None else
                                open(out, "wb").write(open(a["browser_download_url"], "rb").read()))) if source else \
          (lambda a, out=None: fetch(a["browser_download_url"], out, 120))
    want = None
    for line in get(sums).decode("ascii", "replace").splitlines():
        mm = re.fullmatch(r"([0-9A-Fa-f]{64})\s+\*?(.+)", line.strip())
        if mm and mm.group(2).strip() == "tfa-setup-linux":
            want = mm.group(1).lower()
    if not want:
        say("the release's checksums do not name tfa-setup-linux: the one you have is kept")
        sys.exit(0)
    if sha(cur) == want:
        sys.exit(0)  # (already that release's)
    tmp = os.path.join(root, "tfa-setup.download.tmp")
    try:
        get(exe, tmp)
        if os.path.getsize(tmp) != size:
            raise RuntimeError(f"the download is {os.path.getsize(tmp)} bytes, the release's file {size}")
        if sha(tmp) != want:
            raise RuntimeError("the download's SHA-256 is not the one the release publishes")
        with open(tmp, "rb") as f:
            if f.read(4) != b"\x7fELF":
                raise RuntimeError("the download is not a Linux program")
        os.chmod(tmp, 0o755)
        old = os.path.join(root, "tfa-setup.old")
        os.replace(cur, old)  # (a running program's file can be renamed; it goes on running from it)
        try:
            os.replace(tmp, cur)
        except OSError:
            os.replace(old, cur)
            raise
        say(f"updated to the one of {best['tag_name']} (the old one kept as tfa-setup.old)")
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
except SystemExit:
    raise
except Exception as e:
    say(f"not updated ({e}); the one you have is kept")
PYEOF
}
