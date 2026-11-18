// linux.rs - the Linux side of tfa-setup. main.rs uses it under winapi.rs's name (`winapi`): the same functions from
// /proc, sysfs and the desktop's own programs (a process tree Cancel can end, waiting for a process, the graphics
// cards, opening a folder). And what the setup needs only on Linux:
//   - the build container: SteamOS and Bazzite keep their system read-only and have no compiler, so the game is built
//     in a distrobox (Ubuntu 24.04, the README's own `tfa-build`), made and filled with the packages setup.sh lists.
//     The game it builds runs on the host (the build puts FFmpeg beside the executable). Its name is kept in the
//     checkout's .setup.json ("Container"), so updates build there too.
//   - the shortcuts: "Transformers Armada" beside tfa-setup in the install folder (the program to give to Steam or any
//     other launcher, as Transformers Armada.exe is on Windows), an entry in the app menu and on the desktop, and the
//     game added to Steam as a non-Steam game.
use super::*;
use std::sync::atomic::{AtomicBool, Ordering};

// ------------------------------------------------------------------------------------------------
// winapi.rs's functions

/// (Windows: lets an installer's permission prompt come forward. Nothing to do here.)
pub fn allow_foreground() {}

/// (Windows: Shift held while starting = the setup's window. Linux has no such check without a window: the app menu's
/// "Setup (updates, repair)" entry starts it with --setup instead.)
pub fn shift_held() -> bool {
    false
}

/// (Windows 11's Smart App Control - TODO 371. None here.)
pub fn smart_app_control_state() -> Option<u32> {
    None
}

/// A plain message (winapi.rs's message box, for a start that has no window of its own): the desktop's own dialog
/// (kdialog, zenity); without one the text is only on the error output, where the launcher already writes it.
pub fn message_box(caption: &str, text: &str) {
    let kde: [&str; 4] = ["--title", caption, "--sorry", text];
    let zenity: [&str; 6] = ["--warning", "--no-markup", "--title", caption, "--text", text];
    for (program, args) in [("kdialog", &kde[..]), ("zenity", &zenity[..])] {
        if Command::new(program).args(args).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok() {
            return;
        }
    }
}

/// The name of the variable that marks a step's processes: every process a step starts carries it (in the container
/// too, where they are not this program's children), so Cancel finds all of them.
pub const TREE_VAR: &str = "TFA_SETUP_TREE";

/// A new mark for a step's processes.
pub fn new_mark() -> String {
    static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    format!("{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst))
}

/// A step's processes: the child and its descendants, and the ones carrying its mark (TREE_VAR) - in the container
/// they are not this program's descendants. (The mark is given to the commands in the container only, never to the
/// host's `distrobox`/`podman`: podman's own helpers - conmon, the rootless pause process - would inherit it, and
/// ending them would take the container down.)
///
/// An id alone can name another process by the time it is used: Linux gives an id out again once its process has
/// ended and been collected. So, as winapi.rs opens a process and checks it is in the job before it waits (TODO 370),
/// a process is waited for and signalled here through a pidfd - a handle on that very process - opened first, then
/// checked: started no earlier than the tree's first process, and one of the tree (the first process itself by its id
/// and start time, a process whose parents lead to it, or one carrying the mark). The check counts only while the
/// process still runs after it, so it was about this process and not a later one with the same id.
pub struct ProcessTree {
    pid: u32,
    started: u64, // (the first process's start time: with its id it names it)
    mark: Option<String>,
    kill_on_close: bool,
}

impl ProcessTree {
    pub fn new(child: &std::process::Child, kill_on_close: bool) -> Self {
        let pid = child.id();
        // (this program's child, not collected yet: its id still names it, ended or not)
        let started = stat(pid).map_or(0, |s| s.started);
        Self { pid, started, mark: read_mark(pid), kill_on_close }
    }

    /// Whether the process `pid` (its /proc/<pid>/stat: `s`) is one of the tree.
    fn holds(&self, pid: u32, s: &Stat) -> bool {
        if s.started < self.started {
            return false; // (older than the tree's first process: never one it started)
        }
        if pid == self.pid {
            return s.started == self.started;
        }
        let wanted = self.mark.as_ref().map(|m| format!("{TREE_VAR}={m}"));
        self.descends(s.parent) || wanted.is_some_and(|w| environ(pid).is_some_and(|vars| vars.iter().any(|v| *v == w)))
    }

    /// A parent chain that leads to the tree's first process (the one with its start time, not a later one with its id).
    fn descends(&self, mut parent: u32) -> bool {
        for _ in 0..64 {
            if parent == self.pid {
                return stat(parent).is_some_and(|p| p.started == self.started);
            }
            if parent <= 1 {
                return false;
            }
            let Some(s) = stat(parent) else { return false };
            parent = s.parent;
        }
        false
    }

    /// The tree's processes that still run (zombies - ended, not collected yet - are not counted).
    pub fn processes(&self) -> Vec<u32> {
        let me = std::process::id();
        all_pids()
            .into_iter()
            .filter(|&pid| pid != me)
            .filter(|&pid| stat(pid).is_some_and(|s| !s.ended() && self.holds(pid, &s)))
            .collect()
    }

    /// The tree's process `pid`, opened and checked (module note above). None: gone, ended, or not one of the tree.
    fn open_member(&self, pid: u32) -> Option<Opened> {
        let p = Opened::open(pid)?;
        (self.holds(pid, &p.stat) && !p.ended()).then_some(p)
    }

    /// (TODO 370) Waits for the tree's process `pid` to end: its exit code when it is this program's child (only a parent
    /// can read one on Linux). None: not one of the tree (not waited for), gone, or ended with no code to read.
    pub fn wait_for_member(&self, pid: u32) -> Option<i32> {
        let p = self.open_member(pid)?;
        p.wait(None);
        p.exit_code()
    }

    /// Ends all of it: asked to stop, then (after 3 s) made to. Every process is opened and checked before any is
    /// signalled, and kept open: once a parent has ended, its children no longer lead to the first process.
    pub fn terminate(&self) {
        let mut opened = self.open_all();
        for p in &opened {
            p.signal(libc::SIGTERM);
        }
        for _ in 0..30 {
            if opened.iter().all(Opened::ended) && self.processes().is_empty() {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(100));
        }
        opened.extend(self.open_all());
        for p in opened.iter().filter(|p| !p.ended()) {
            p.signal(libc::SIGKILL);
        }
    }

    fn open_all(&self) -> Vec<Opened> {
        self.processes().into_iter().filter_map(|pid| self.open_member(pid)).collect()
    }
}

impl Drop for ProcessTree {
    fn drop(&mut self) {
        if self.kill_on_close {
            self.terminate();
        }
    }
}

/// The mark of a process just started: in its environment on this PC, in its command line ("env TFA_SETUP_TREE=...")
/// for a step in the container. Read again for a moment while the program is being put in place: std's spawn returns
/// as soon as the start cannot fail any more, and just then the environment reads as this program's or as empty (a test
/// found no mark so, now and then). One that is in place and has no mark has none.
fn read_mark(pid: u32) -> Option<String> {
    let prefix = format!("{TREE_VAR}=");
    let find = |vars: Vec<String>| vars.into_iter().find_map(|v| v.strip_prefix(&prefix).map(str::to_string));
    let mine = environ(std::process::id());
    for _ in 0..100 {
        let env = environ(pid)?;
        if let Some(mark) = find(env.clone()).or_else(|| cmdline(pid).and_then(find)) {
            return Some(mark);
        }
        if !env.is_empty() && Some(&env) != mine.as_ref() {
            return None;
        }
        std::thread::sleep(std::time::Duration::from_millis(2));
    }
    None
}

fn all_pids() -> Vec<u32> {
    std::fs::read_dir("/proc")
        .map(|d| d.flatten().filter_map(|e| e.file_name().to_str().and_then(|n| n.parse().ok())).collect())
        .unwrap_or_default()
}

/// A process's environment (only this user's processes can be read).
fn environ(pid: u32) -> Option<Vec<String>> {
    nul_list(&format!("/proc/{pid}/environ"))
}

/// A process's command line, word by word.
fn cmdline(pid: u32) -> Option<Vec<String>> {
    nul_list(&format!("/proc/{pid}/cmdline"))
}

fn nul_list(file: &str) -> Option<Vec<String>> {
    let raw = std::fs::read(file).ok()?;
    Some(raw.split(|&b| b == 0).filter(|v| !v.is_empty()).map(|v| String::from_utf8_lossy(v).into_owned()).collect())
}

/// /proc/<pid>/stat: the process's state, its parent and its start time (clock ticks after boot).
struct Stat {
    state: u8,
    parent: u32,
    started: u64,
}

impl Stat {
    /// A zombie (ended, its parent has not collected it yet) or dead.
    fn ended(&self) -> bool {
        matches!(self.state, b'Z' | b'X' | b'x')
    }
}

fn stat(pid: u32) -> Option<Stat> {
    let text = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    // (the fields after the program's name, which may hold spaces and brackets: [0] the state, [1] the parent, ...
    // [19] the start time)
    let fields: Vec<&str> = text[text.rfind(')')? + 1..].split_whitespace().collect();
    Some(Stat { state: *fields.first()?.as_bytes().first()?, parent: fields.get(1)?.parse().ok()?, started: fields.get(19)?.parse().ok()? })
}

/// A process opened to be waited for or signalled. `fd`: its pidfd (Linux 5.3 and later), which names that process
/// even once its id is given to another. None where pidfds are not allowed: then its id and start time name it.
/// `stat`: read after it was opened - its own while `ended()` is false afterwards.
struct Opened {
    pid: u32,
    fd: Option<std::os::fd::OwnedFd>,
    stat: Stat,
}

impl Opened {
    /// None: no such process.
    fn open(pid: u32) -> Option<Self> {
        use std::os::fd::FromRawFd;
        let n = unsafe { libc::syscall(libc::SYS_pidfd_open, pid as libc::pid_t, 0 as libc::c_uint) };
        let fd = if n >= 0 {
            Some(unsafe { std::os::fd::OwnedFd::from_raw_fd(n as i32) })
        } else if std::io::Error::last_os_error().raw_os_error() == Some(libc::ESRCH) {
            return None;
        } else {
            None // (ENOSYS, a kernel before 5.3; EPERM, a sandbox that forbids it)
        };
        Some(Self { pid, fd, stat: stat(pid)? })
    }

    /// It has ended (a zombie too) - or, without a pidfd, its id no longer names it.
    fn ended(&self) -> bool {
        match &self.fd {
            Some(fd) => pidfd_ended(fd, 0),
            None => stat(self.pid).is_none_or(|s| s.ended() || s.started != self.stat.started),
        }
    }

    /// Waits until it has ended, at most `timeout_ms` (None: no limit).
    fn wait(&self, timeout_ms: Option<u32>) {
        if let Some(fd) = &self.fd {
            pidfd_ended(fd, timeout_ms.map_or(-1, |t| t.min(i32::MAX as u32) as i32));
            return;
        }
        let end = timeout_ms.map(|t| Instant::now() + std::time::Duration::from_millis(t as u64));
        while !self.ended() && end.is_none_or(|e| Instant::now() < e) {
            std::thread::sleep(std::time::Duration::from_millis(100));
        }
    }

    /// Its exit code once it has ended, when it is this program's child (only the parent can read it). Read without
    /// collecting it, so std's Child::wait still gets it. None: not this program's child, ended by a signal, running.
    fn exit_code(&self) -> Option<i32> {
        use std::os::fd::AsRawFd;
        let fd = self.fd.as_ref()?;
        let mut info: libc::siginfo_t = unsafe { std::mem::zeroed() };
        let ok = unsafe { libc::waitid(libc::P_PIDFD, fd.as_raw_fd() as libc::id_t, &mut info, libc::WEXITED | libc::WNOWAIT | libc::WNOHANG) } == 0;
        (ok && unsafe { info.si_pid() } != 0 && info.si_code == libc::CLD_EXITED).then(|| unsafe { info.si_status() })
    }

    fn signal(&self, signal: i32) {
        use std::os::fd::AsRawFd;
        match &self.fd {
            Some(fd) => unsafe {
                libc::syscall(libc::SYS_pidfd_send_signal, fd.as_raw_fd(), signal, std::ptr::null::<libc::siginfo_t>(), 0 as libc::c_uint);
            },
            None if !self.ended() => unsafe {
                libc::kill(self.pid as libc::pid_t, signal);
            },
            None => {}
        }
    }
}

/// Waits up to `ms` (-1: no limit) for a pidfd's process to end: true when it has (a pidfd reads as ready then).
fn pidfd_ended(fd: &std::os::fd::OwnedFd, ms: i32) -> bool {
    use std::os::fd::AsRawFd;
    let mut p = libc::pollfd { fd: fd.as_raw_fd(), events: libc::POLLIN, revents: 0 };
    loop {
        match unsafe { libc::poll(&mut p, 1, ms) } {
            n if n >= 0 => return n > 0,
            _ if std::io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) => continue,
            _ => return true, // (no wait possible: not taken as running, so nothing waits for ever)
        }
    }
}

/// Waits for a process to end: its exit code when it is this program's child (the game a launcher started), else None
/// (only a parent can read it), as for one that is already gone. Through its pidfd: the process opened is the one
/// waited for, even when its id is given out again meanwhile.
pub fn wait_for_exit(pid: u32) -> Option<i32> {
    let p = Opened::open(pid)?;
    p.wait(None);
    p.exit_code()
}

/// wait_for_exit for a process named by its id and the start time written down for it (LeftStep): its pidfd opened,
/// then checked against that start time - the process waited for is the one written down, not a later one given its
/// id. None at once when it is not that one, or no start time was written down. (The start time is read after the
/// open: a process holding the id then with that start time held it from before the open, so the pidfd names it.)
pub fn wait_for_exit_checked(pid: u32, started: u64) -> Option<i32> {
    let p = Opened::open(pid)?;
    if started == 0 || p.stat.started != started {
        return None;
    }
    p.wait(None);
    p.exit_code()
}

/// (`--update --wait-pid`, the game that started this update) Waits up to `timeout_ms` for that process to end; true
/// when it still runs afterwards. Its pidfd opened, then checked: it must have started before this program - one that
/// started later has only been given the id since, and is not waited for (false at once). The check counts only while
/// the process still runs after it (then the start time read was its own).
pub fn wait_for_starter(pid: u32, timeout_ms: u32) -> bool {
    let (Some(p), Some(me)) = (Opened::open(pid), stat(std::process::id())) else { return false };
    if p.stat.started >= me.started || p.ended() {
        return false;
    }
    p.wait(Some(timeout_ms));
    !p.ended()
}

/// (Windows: a console window's host process. None here.)
pub fn is_console_host(_pid: u32) -> bool {
    false
}

/// (TODO 289) A running process's start time (clock ticks after boot): with its id it names the process, an id alone
/// can be given to another process once it has ended. None: not running (gone, or a zombie).
pub fn process_started(pid: u32) -> Option<u64> {
    let p = Opened::open(pid)?;
    (!p.stat.ended() && !p.ended()).then_some(p.stat.started)
}

/// Waits up to `timeout_ms` for a process to end (at once when it is not there).
pub fn wait_for_process(pid: u32, timeout_ms: u32) {
    if let Some(p) = Opened::open(pid) {
        p.wait(Some(timeout_ms));
    }
}

/// (Windows: PATH as the registry has it now. Here the environment's.)
pub fn registry_path() -> String {
    std::env::var("PATH").unwrap_or_default()
}

/// Opens a folder in the desktop's file manager.
pub fn open_folder(folder: &Path) {
    let _ = Command::new("xdg-open").arg(folder).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn();
}

/// The graphics cards: (name, video memory in bytes; 0 when not known). AMD: the name Mesa gives the card
/// (libdrm's amdgpu.ids: "AMD Radeon RX 9070 XT", the Steam Deck's "AMD Custom GPU 0405" - the names Windows shows)
/// and its own memory from the driver (an APU's is its small carve-out, as on Windows). NVIDIA: nvidia-smi.
/// Others (Intel): the name only (0: counted as built-in graphics, as an Arc card too - TODO).
pub fn graphics_cards() -> Vec<(String, u64)> {
    let mut cards = Vec::new();
    if let Ok(out) = Command::new("nvidia-smi").args(["--query-gpu=name,memory.total", "--format=csv,noheader,nounits"])
        .stdin(Stdio::null()).stderr(Stdio::null()).output()
    {
        for line in String::from_utf8_lossy(&out.stdout).lines() {
            if let Some((name, mib)) = line.rsplit_once(',') {
                cards.push((name.trim().to_string(), mib.trim().parse::<u64>().unwrap_or(0) << 20));
            }
        }
    }
    let Ok(dir) = std::fs::read_dir("/sys/class/drm") else { return cards };
    for e in dir.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        if !(name.starts_with("card") && name[4..].bytes().all(|b| b.is_ascii_digit()) && name.len() > 4) {
            continue;
        }
        let dev = e.path().join("device");
        let hex = |f: &str| std::fs::read_to_string(dev.join(f)).ok().and_then(|t| u32::from_str_radix(t.trim().trim_start_matches("0x"), 16).ok());
        let (Some(vendor), Some(device)) = (hex("vendor"), hex("device")) else { continue };
        match vendor {
            0x1002 => {
                let vram = std::fs::read_to_string(dev.join("mem_info_vram_total")).ok().and_then(|t| t.trim().parse().ok()).unwrap_or(0);
                let name = amdgpu_name(device, hex("revision").unwrap_or(0)).unwrap_or_else(|| "AMD Radeon Graphics".into());
                cards.push((name, vram));
            }
            0x10de => {} // (nvidia-smi's, above)
            0x8086 => cards.push(("Intel Graphics".into(), 0)),
            _ => {}
        }
    }
    cards
}

/// The marketing name of an AMD card, as Mesa finds it: libdrm's amdgpu.ids ("7550,\tC0,\tAMD Radeon RX 9070 XT").
fn amdgpu_name(device: u32, revision: u32) -> Option<String> {
    for file in ["/usr/share/libdrm/amdgpu.ids", "/usr/share/hwdata/amdgpu.ids"] {
        let Ok(text) = std::fs::read_to_string(file) else { continue };
        let mut first = None;
        for line in text.lines().filter(|l| !l.starts_with('#')) {
            let parts: Vec<&str> = line.split(',').map(str::trim).collect();
            if parts.len() < 3 || u32::from_str_radix(parts[0], 16).ok() != Some(device) {
                continue;
            }
            let name = parts[2..].join(",");
            if u32::from_str_radix(parts[1], 16).ok() == Some(revision) {
                return Some(name);
            }
            first.get_or_insert(name);
        }
        if first.is_some() {
            return first;
        }
    }
    None
}

/// The local time, for the branch a player's own changes are kept on (GetLocalTime on Windows).
pub fn local_time(t: &mut LocalTime) {
    unsafe {
        let now = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&now, &mut tm);
        t.year = (tm.tm_year + 1900) as u16;
        t.month = (tm.tm_mon + 1) as u16;
        t.day = tm.tm_mday as u16;
        t.day_of_week = tm.tm_wday as u16;
        t.hour = tm.tm_hour as u16;
        t.minute = tm.tm_min as u16;
        t.second = tm.tm_sec as u16;
    }
}

// ------------------------------------------------------------------------------------------------
// File dialogs: the desktop's own (KDE's through kdialog, else zenity: both on SteamOS and Bazzite).

fn dialog(kde: &[&str], zenity: &[&str]) -> Option<PathBuf> {
    let on_kde = std::env::var("XDG_CURRENT_DESKTOP").is_ok_and(|d| d.to_ascii_uppercase().contains("KDE"));
    let tries: [(&str, &[&str]); 2] = if on_kde { [("kdialog", kde), ("zenity", zenity)] } else { [("zenity", zenity), ("kdialog", kde)] };
    for (program, args) in tries {
        match Command::new(program).args(args).stdin(Stdio::null()).stderr(Stdio::null()).output() {
            Ok(out) => {
                let picked = String::from_utf8_lossy(&out.stdout).trim_end_matches(['\n', '\r']).to_string();
                return (out.status.success() && !picked.is_empty()).then(|| PathBuf::from(picked));
            }
            Err(_) => continue, // (not installed: the other one)
        }
    }
    None
}

pub fn pick_file(title: &str) -> Option<PathBuf> {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/".into());
    dialog(&["--title", title, "--getopenfilename", &home, "Disc image (*.iso *.ISO)"],
           &["--file-selection", "--title", title, "--file-filter=Disc image | *.iso *.ISO", "--file-filter=All files | *"])
}

pub fn pick_folder(title: &str) -> Option<PathBuf> {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/".into());
    dialog(&["--title", title, "--getexistingdirectory", &home], &["--file-selection", "--directory", "--title", title])
}

// ------------------------------------------------------------------------------------------------
// The build container

const CONTAINER_IMAGE: &str = "docker.io/library/ubuntu:24.04";
static CONTAINER: Mutex<Option<String>> = Mutex::new(None);

/// The container this install builds in (None: on this PC itself).
pub fn container() -> Option<String> {
    CONTAINER.lock().unwrap().clone()
}

fn set_container(name: Option<String>) {
    *CONTAINER.lock().unwrap() = name;
}

/// The container's name for a new install: the README's tfa-build (one a player made by hand is used as it is), or
/// TFA_CONTAINER's.
fn default_container() -> String {
    std::env::var("TFA_CONTAINER").ok().filter(|v| !v.trim().is_empty()).unwrap_or_else(|| "tfa-build".into())
}

/// A program on PATH.
fn host_has(exe: &str) -> bool {
    std::env::var("PATH").unwrap_or_default().split(':').any(|d| !d.is_empty() && Path::new(d).join(exe).is_file())
}

/// /etc/os-release's value for a key ("" without it).
fn os_release(key: &str) -> String {
    let text = std::fs::read_to_string("/etc/os-release").unwrap_or_default();
    text.lines()
        .find_map(|l| l.strip_prefix(&format!("{key}=")))
        .map(|v| v.trim().trim_matches('"').to_string())
        .unwrap_or_default()
}

/// A system that is kept read-only (SteamOS; Bazzite and the other Fedora Atomic systems: ostree).
pub fn immutable_os() -> bool {
    Path::new("/run/ostree-booted").exists() || os_release("ID") == "steamos" || os_release("ID_LIKE").split(' ').any(|i| i == "steamos")
}

/// The build's tools on this PC itself: setup.sh's own check, roughly (it says exactly what is missing).
fn host_has_build_tools() -> bool {
    let tools = ["git", "cmake", "ninja", "pkg-config", "python3"].iter().all(|t| host_has(t))
        && ["c++", "g++", "clang++"].iter().any(|t| host_has(t));
    tools && Command::new("pkg-config").args(["--exists", "x11", "gl", "libavcodec", "libavformat", "libavutil", "libswresample", "libswscale"])
        .stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|s| s.success())
}

/// Where a new install builds: a container on a read-only system, or on a PC without the build tools when distrobox is
/// there; else this PC (setup.sh then lists what is missing, with the command that installs it).
fn wanted_container() -> Option<String> {
    let need = immutable_os() || !host_has_build_tools();
    (need && host_has("distrobox")).then(default_container)
}

/// The form's line under the options (Windows: winget installs the missing tools).
pub fn tools_note() -> String {
    match wanted_container() {
        Some(name) if immutable_os() => format!(
            "This system is read-only: the build tools go into a container of their own (distrobox \"{name}\", Ubuntu 24.04, about 1.5 GB). Nothing on the system changes; the game runs on it. Stay in Desktop Mode until it says Ready to play."),
        Some(name) => format!("Missing build tools go into a container of their own (distrobox \"{name}\", Ubuntu 24.04); nothing on the system changes."),
        None if host_has_build_tools() => "The game is built with the tools installed on this PC.".into(),
        None => "Missing build tools: the setup lists them, with the command that installs them on your distribution (or install distrobox: the setup then uses a container).".into(),
    }
}

/// The container's name a checkout records (.setup.json "Container"), if any.
fn recorded_container(repo: &Path) -> Option<String> {
    let text = std::fs::read_to_string(repo.join(".setup.json")).ok()?;
    let at = text.find("\"Container\"")?;
    let rest = text[at + 11..].trim_start().strip_prefix(':')?.trim_start().strip_prefix('"')?;
    let name = rest.split('"').next()?.trim();
    (!name.is_empty()).then(|| name.to_string())
}

/// .setup.json's "Container" set (the other keys - setup.sh's Iso and SetUpAt - kept).
fn record_container(repo: &Path, name: &str) {
    let path = repo.join(".setup.json");
    let text = std::fs::read_to_string(&path).unwrap_or_default();
    let body = text.trim();
    let entry = format!("\"Container\": \"{name}\"");
    let new = if recorded_container(repo).is_some() {
        let at = text.find("\"Container\"").unwrap();
        let end = text[at..].find(':').map(|c| at + c).and_then(|c| text[c..].find('"').map(|q| c + q))
            .and_then(|q| text[q + 1..].find('"').map(|e| q + 1 + e + 1)).unwrap_or(text.len());
        format!("{}{entry}{}", &text[..at], &text[end..])
    } else if body.starts_with('{') && body.len() > 2 {
        format!("{{\n  {entry},{}", &body[1..])
    } else {
        format!("{{\n  {entry}\n}}\n")
    };
    let _ = std::fs::write(path, new);
}

fn container_exists(name: &str) -> bool {
    Command::new("distrobox").args(["list", "--no-color"]).stdin(Stdio::null()).stderr(Stdio::null()).output()
        .map(|o| String::from_utf8_lossy(&o.stdout).lines().any(|l| l.split('|').nth(1).is_some_and(|n| n.trim() == name)))
        .unwrap_or(false)
}

/// A command run in the container when the install builds there: the scripts (bash), and Git when this PC has none.
/// The step's variables are given to it explicitly (distrobox passes most of the environment, not all of it); a
/// `distrobox enter <name> -- ...` of the setup's own gets them the same way.
pub fn wrap(exe: &str, args: &[&str], vars: &[(&str, String)]) -> (String, Vec<String>) {
    let plain = || (exe.to_string(), args.iter().map(|a| a.to_string()).collect());
    let env = || std::iter::once("env".to_string()).chain(vars.iter().map(|(k, v)| format!("{k}={v}")));
    if exe == "distrobox" && args.first() == Some(&"enter") {
        if let Some(at) = args.iter().position(|a| *a == "--") {
            let mut all: Vec<String> = args[..=at].iter().map(|a| a.to_string()).collect();
            all.extend(env());
            all.extend(args[at + 1..].iter().map(|a| a.to_string()));
            return ("distrobox".into(), all);
        }
    }
    let Some(name) = container() else { return plain() };
    if exe != "bash" && !(exe == "git" && !host_has("git")) {
        return plain();
    }
    let mut all: Vec<String> = vec!["enter".into(), name, "--".into()];
    all.extend(env());
    all.push(exe.into());
    all.extend(args.iter().map(|a| a.to_string()));
    ("distrobox".into(), all)
}

/// Git for check_updates and keep_local_work: this PC's, else the container's.
pub fn git_command(vars: &[(&str, &std::ffi::OsStr)]) -> Command {
    match container() {
        Some(name) if !host_has("git") => {
            let mut c = Command::new("distrobox");
            c.args(["enter", &name, "--", "env"]);
            for (k, v) in vars {
                c.arg(format!("{k}={}", v.to_string_lossy()));
            }
            c.arg("git");
            c
        }
        _ => Command::new("git"),
    }
}

/// The Ubuntu packages the checkout's own setup.sh names (`setup.sh --packages ubuntu`): one list for both. A checkout
/// from before that option: read from its missing-tools message ("Ubuntu / Debian:  sudo apt install ..."). Plus Pillow
/// and numpy, always: the sharp fonts and button prompts are made without the HD textures too (a container has no pip -
/// on the Legion's SteamOS test they were skipped without them); and for the HD textures unzip, curl and the Vulkan driver.
fn ubuntu_packages(repo: &Path, hd: bool) -> Result<Vec<String>, String> {
    let setup = game_folder(repo).join("setup.sh");
    // (--packages prints the list and does nothing else; an older setup.sh stops at the unknown option, before it
    // writes anything)
    let listed = Command::new("bash").arg(&setup).args(["--packages", "ubuntu"]).stdin(Stdio::null()).stderr(Stdio::null()).output().ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).split_whitespace().map(str::to_string).collect::<Vec<_>>())
        .filter(|p| !p.is_empty());
    let mut pkgs = match listed {
        Some(p) => p,
        None => {
            let text = std::fs::read_to_string(&setup).map_err(|e| format!("setup.sh cannot be read: {e}"))?;
            let start = text.find("Ubuntu / Debian:").ok_or("setup.sh lists no Ubuntu packages")?;
            let block = &text[start..];
            let block = &block[..block.find("Fedora:").unwrap_or(block.len())];
            let joined = block.replace("\\\n", " ");
            let line = joined.lines().find(|l| l.contains("apt install")).ok_or("setup.sh lists no Ubuntu packages")?;
            line[line.find("apt install").unwrap() + 11..].split_whitespace().map(str::to_string).collect()
        }
    };
    pkgs.extend(["python3-pil", "python3-numpy"].map(String::from));
    if hd {
        pkgs.extend(["unzip", "curl", "mesa-vulkan-drivers", "libvulkan1"].map(String::from));
    }
    let mut seen = std::collections::HashSet::new();
    pkgs.retain(|p| seen.insert(p.clone())); // (once each: curl is in setup.sh's list too)
    Ok(pkgs)
}

impl Job {
    /// The setup and the update on Linux (Job::run's Windows steps, with setup.sh / update.sh).
    pub(super) fn run_linux(&self, tx: &Sender<Msg>, log: &Arc<Mutex<Option<std::fs::File>>>, log_path: &Path,
                            path_env: &str) -> Result<(), String> {
        let full = |e: String| format!("{e}\n\nThe full log: {}", log_path.display());
        let repo = checkout_in(&self.folder, &self.repo_url);
        let r = repo.to_string_lossy().to_string();
        set_container(recorded_container(&repo));

        if self.update {
            // update.sh does it all (and, for the game's own update, starts it again: --play).
            let _ = tx.send(Msg::Step(STEP_DOWNLOAD));
            // (the game that asked for the update closes first: its folder is about to change. Only the process that
            // started this one is waited for, checked through its pidfd; update.sh is told to wait for it only while
            // it still runs - it fails then, "the game did not close", as on Windows)
            let mut still_running = None;
            if self.wait_pid != 0 {
                let _ = tx.send(Msg::Detail(STEP_DOWNLOAD, "waiting for the game to close".into()));
                still_running = wait_for_starter(self.wait_pid, 120_000).then_some(self.wait_pid);
            }
            self.keep_local_work(&repo, tx, log).map_err(full)?;
            let script = repo.join("update.sh").to_string_lossy().to_string();
            let mut args: Vec<String> = vec![script, "--stash-local-changes".into()];
            if let Some(pid) = still_running {
                args.extend(["--wait-pid".into(), pid.to_string()]);
            }
            // (the game started again on this system, never in the build container: its libraries are the system's)
            let play_here = self.play && container().is_some();
            if self.play && !play_here {
                args.push("--play".into());
            }
            let refs: Vec<&str> = args.iter().map(String::as_str).collect();
            let result = self.exec("bash", &refs, Some(&repo), path_env, tx, STEP_DOWNLOAD, log).map_err(full);
            // (TODO 426) unless "Start the game when the update is done" was unticked (read now: it can change meanwhile)
            if play_here && crate::start_game_after_update(&repo) {
                let _ = start_game(&repo);
            }
            result?;
            copy_self(&self.folder);
            let _ = std::fs::remove_file(self.folder.join(UNFINISHED_MARK));
            desktop_shortcut_once(&self.folder, &repo);
            return Ok(());
        }

        // Tools: the container on SteamOS / Bazzite (or a PC without the build tools), and Git.
        let _ = tx.send(Msg::Step(STEP_TOOLS));
        if container().is_none() {
            set_container(wanted_container());
        }
        if let Some(name) = container() {
            if !container_exists(&name) {
                let _ = tx.send(Msg::Detail(STEP_TOOLS, format!("making the build container \"{name}\" (distrobox, Ubuntu 24.04)")));
                let mut args = vec!["create", "--yes", "--name", name.as_str(), "--image", CONTAINER_IMAGE];
                if Path::new("/proc/driver/nvidia").exists() {
                    args.push("--nvidia"); // (the HD textures are made on the graphics card, from inside it)
                }
                self.exec("distrobox", &args, None, path_env, tx, STEP_TOOLS, log).map_err(full)?;
            }
            let _ = tx.send(Msg::Detail(STEP_TOOLS, format!("starting the build container \"{name}\" (the first time takes a few minutes)")));
            self.exec("distrobox", &["enter", name.as_str(), "--", "true"], None, path_env, tx, STEP_TOOLS, log).map_err(full)?;
            if !host_has("git") {
                let _ = tx.send(Msg::Detail(STEP_TOOLS, "installing Git in the container".into()));
                self.exec("distrobox", &["enter", name.as_str(), "--", "sh", "-c", "command -v git >/dev/null || (sudo apt-get update && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y git)"],
                          None, path_env, tx, STEP_TOOLS, log).map_err(full)?;
            }
        } else if !host_has("git") {
            return Err("Git is not on this PC: install git from your distribution's packages, then run the setup again.".into());
        }
        let _ = tx.send(Msg::Progress(0.03));

        // Download: as on Windows.
        let _ = tx.send(Msg::Step(STEP_DOWNLOAD));
        if repo.join(".git").exists() {
            let _ = tx.send(Msg::Detail(STEP_DOWNLOAD, "already there: the newest changes pulled".into()));
            self.keep_local_work(&repo, tx, log)?;
            self.exec("git", &["-C", &r, "pull", "--ff-only"], None, path_env, tx, STEP_DOWNLOAD, log)?;
            self.exec("git", &["-C", &r, "submodule", "update", "--init", "--recursive"], None, path_env, tx, STEP_DOWNLOAD, log)?;
        } else {
            self.exec("git", &["clone", "--recursive", &self.repo_url, &r], None, path_env, tx, STEP_DOWNLOAD, log)
                .map_err(|e| format!("{e}\n\nThe repository could not be downloaded from {} (no network? the address right?).", self.repo_url))?;
        }
        let _ = tx.send(Msg::Progress(0.06));

        // Tools again: the packages setup.sh lists, into the container (setup.sh checks them next).
        if let Some(name) = container() {
            record_container(&repo, &name);
            let _ = tx.send(Msg::Step(STEP_TOOLS));
            let pkgs = ubuntu_packages(&repo, self.hd).map_err(full)?;
            let list = pkgs.join(" ");
            let script = format!("if dpkg -s {list} >/dev/null 2>&1; then echo 'the build tools are there'; else \
                                  sudo apt-get update && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y {list}; fi");
            let _ = tx.send(Msg::Detail(STEP_TOOLS, format!("installing the build tools in the container \"{name}\" (apt, about 1.5 GB the first time)")));
            self.exec("distrobox", &["enter", name.as_str(), "--", "sh", "-c", script.as_str()], None, path_env, tx, STEP_TOOLS, log).map_err(full)?;
        }

        // The HD choices where make_hd_textures.sh reads them (hd_options.json, as setup.ps1 writes it).
        let hd_dir = game_folder(&repo).join("hd_textures");
        let _ = std::fs::create_dir_all(&hd_dir);
        let _ = std::fs::write(hd_dir.join("hd_options.json"), format!("{{\n    \"quality\":  \"{}\",\n    \"while_playing\":  {},\n    \"gentle\":  {}\n}}",
                                                                         self.hd_quality.arg(), self.hd_while_playing, self.hd_gentle));

        // setup.sh: the tools checked, the disc, the game.
        let script = game_folder(&repo).join("setup.sh").to_string_lossy().to_string();
        let gentle = if self.hd_gentle { "on" } else { "off" };
        self.exec("bash", &[script.as_str(), "--iso", self.retail.as_str(), "--gentle", gentle], Some(&repo), path_env, tx, STEP_TOOLS, log).map_err(full)?;

        // The HD textures (make_hd_textures.sh); without them the sharp fonts and button prompts only, as on Windows.
        let _ = tx.send(Msg::Step(STEP_HD));
        let hd_script = game_folder(&repo).join("tools").join("make_hd_textures.sh").to_string_lossy().to_string();
        if self.hd {
            let _ = tx.send(Msg::Progress(0.62));
            if let Err(e) = self.exec("bash", &[hd_script.as_str()], Some(&game_folder(&repo)), path_env, tx, STEP_HD, log) {
                // (the game plays without them: the setup carries on, the log says why)
                let last = e.lines().last().unwrap_or("").to_string();
                let _ = tx.send(Msg::Detail(STEP_HD, format!("stopped ({last}): the game shows its original textures; the log says why")));
            }
        } else {
            let _ = self.exec("bash", &[hd_script.as_str(), "--fonts-only"], Some(&game_folder(&repo)), path_env, tx, STEP_HD, log);
        }

        copy_self(&self.folder);
        if !self.managed {
            remember_folder(&self.folder);
        }
        let _ = std::fs::remove_file(self.folder.join(UNFINISHED_MARK));
        if !self.managed {
            desktop_shortcut_once(&self.folder, &repo);
        }
        Ok(())
    }
}

/// (PR #20) The HD step for a new preset, in a terminal window of its own when there is one (else in the background,
/// its output in hd_textures.log in the install folder).
pub fn start_hd_maker(repo: &Path) -> Result<(), String> {
    let game = game_folder(repo);
    let script = game.join("tools").join("make_hd_textures.sh");
    if !script.is_file() {
        return Err(format!("{} is missing", script.display()));
    }
    set_container(recorded_container(repo));
    let s = script.to_string_lossy().to_string();
    let (exe, args) = wrap("bash", &[s.as_str()], &[]);
    for (term, opts) in [("konsole", &["--hold", "-e"][..]), ("kgx", &["--"][..]), ("gnome-terminal", &["--"][..]), ("xterm", &["-hold", "-e"][..])] {
        if host_has(term) {
            return Command::new(term).args(opts).arg(&exe).args(&args).current_dir(&game).spawn().map(|_| ())
                .map_err(|e| format!("could not start the HD textures window: {e}"));
        }
    }
    let out = std::fs::File::create(repo.parent().unwrap_or(repo).join("hd_textures.log")).map_err(|e| e.to_string())?;
    Command::new(&exe).args(&args).current_dir(&game).stdout(Stdio::from(out.try_clone().map_err(|e| e.to_string())?))
        .stderr(Stdio::from(out)).stdin(Stdio::null()).spawn().map(|_| ()).map_err(|e| format!("could not start the HD textures: {e}"))
}

// ------------------------------------------------------------------------------------------------
// Shortcuts: "Transformers Armada" in the install folder, the app menu, the desktop, Steam

/// The form's "Add it to Steam" (a non-Steam game), on by default.
pub static ADD_TO_STEAM: AtomicBool = AtomicBool::new(true);
static STEAM_RESULT: Mutex<Option<(bool, String)>> = Mutex::new(None);

/// What adding the game to Steam did, for the last screen.
pub fn steam_result() -> Option<(bool, String)> {
    STEAM_RESULT.lock().unwrap().clone()
}

pub fn steam_checkbox(ui: &mut egui::Ui) {
    if steam_dir().is_none() {
        return;
    }
    let mut on = ADD_TO_STEAM.load(Ordering::SeqCst);
    ui.checkbox(&mut on, "Add it to Steam (as a non-Steam game)").on_hover_text(
        "\"Transformers Armada\" in your Steam library: start it from Gaming Mode like any other game. If Steam is running it adds it itself; if not, the setup adds it to your account's list of non-Steam games (backed up first), or says how to add it when it cannot be sure.");
    ADD_TO_STEAM.store(on, Ordering::SeqCst);
}

/// The launcher in the install folder: what Steam and the shortcuts start (Windows: Transformers Armada.exe).
pub fn launcher_path(folder: &Path) -> PathBuf {
    folder.join("Transformers Armada")
}

/// `Transformers Armada` beside tfa-setup: starts the game (the checkout's play.sh), found from where it is, so the
/// install folder can be moved or copied. Written again each time (a checkout's name may change).
fn write_launcher(folder: &Path, repo: &Path) {
    let name = repo.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
    let text = format!("#!/bin/sh\n\
        # Transformers Armada (PC version): starts the game. The program to give to Steam or any other launcher.\n\
        # Made by tfa-setup; it starts {name}/play.sh in the folder it is in.\n\
        here=$(dirname \"$(readlink -f \"$0\")\")\n\
        exec \"$here/{name}/play.sh\" \"$@\"\n");
    let path = launcher_path(folder);
    if std::fs::read_to_string(&path).ok().as_deref() != Some(text.as_str()) {
        let _ = std::fs::write(&path, text);
    }
    use std::os::unix::fs::PermissionsExt;
    let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755));
}

/// The app menu's entry and the desktop's (Windows: the Start menu's and the desktop's shortcuts), each made once:
/// they start tfa-setup in the install folder, which starts the game when the install is finished and carries on the
/// setup when it is not; "Setup (updates, repair)" opens the setup's window. Then the game added to Steam, once.
pub fn desktop_shortcut_once(folder: &Path, repo: &Path) {
    write_launcher(folder, repo);
    let setup = folder.join(SETUP_EXE);
    let quote = |p: &Path| format!("\"{}\"", p.to_string_lossy().replace('\\', "\\\\").replace('"', "\\\""));
    let icon = icon_path(repo).map(|p| p.to_string_lossy().to_string()).unwrap_or_else(|| "applications-games".into());
    let entry = format!("[Desktop Entry]\nType=Application\nName=Transformers Armada\nComment=Transformers Armada (PC version)\n\
        Exec={} %u\nPath={}\nIcon={icon}\nTerminal=false\nCategories=Game;\nActions=Setup;\n\n\
        [Desktop Action Setup]\nName=Setup (updates, repair)\nExec={} --setup\n",
        quote(&setup), folder.to_string_lossy(), quote(&setup));
    let data = data_home();
    let flags = remembered_folder_file().and_then(|f| f.parent().map(Path::to_path_buf));
    let once = |flag: &str, write: &dyn Fn() -> bool| {
        let Some(dir) = &flags else { return };
        let f = dir.join(flag);
        if !f.exists() && write() {
            let _ = std::fs::create_dir_all(dir);
            let _ = std::fs::write(f, b"");
        }
    };
    once("start_menu_shortcut_made", &|| {
        let dir = data.join("applications");
        std::fs::create_dir_all(&dir).is_ok() && std::fs::write(dir.join("transformers-armada.desktop"), &entry).is_ok()
    });
    once("desktop_shortcut_made", &|| {
        let Some(desktop) = desktop_dir() else { return false };
        let f = desktop.join("Transformers Armada.desktop");
        // (KDE starts a desktop file there only when it is executable: trusted)
        use std::os::unix::fs::PermissionsExt;
        std::fs::write(&f, &entry).is_ok() && std::fs::set_permissions(&f, std::fs::Permissions::from_mode(0o755)).is_ok()
    });
    if ADD_TO_STEAM.load(Ordering::SeqCst) {
        once("steam_shortcut_made", &|| {
            let (ok, text) = add_to_steam(&launcher_path(folder), folder, icon_path(repo).as_deref());
            *STEAM_RESULT.lock().unwrap() = Some((ok, text));
            ok
        });
    }
}

/// `tfa-setup --add-to-steam`: the game added to Steam now, and what was done printed (the setup does it once, after
/// the first install; this again - Steam installed since, or its entry deleted). Exit code 0 when it is in Steam.
pub fn add_to_steam_cli(folder: &Path, repo: &Path) -> i32 {
    if !repo.join(".git").exists() {
        eprintln!("the game is not installed in {}", folder.display());
        return 1;
    }
    write_launcher(folder, repo);
    let (ok, text) = add_to_steam(&launcher_path(folder), folder, icon_path(repo).as_deref());
    println!("{text}");
    if let (true, Some(dir)) = (ok, remembered_folder_file().and_then(|f| f.parent().map(Path::to_path_buf))) {
        let _ = std::fs::create_dir_all(&dir);
        let _ = std::fs::write(dir.join("steam_shortcut_made"), b"");
    }
    if ok { 0 } else { 1 }
}

/// The game's icon, when the build made one (none is shipped: it would come from the disc).
fn icon_path(repo: &Path) -> Option<PathBuf> {
    let game = game_folder(repo);
    ["out/build/transformers/icon.png", "out/build/transformers/TransformersRecomp.png"].iter().map(|p| game.join(p)).find(|p| p.is_file())
}

fn data_home() -> PathBuf {
    std::env::var("XDG_DATA_HOME").ok().filter(|v| !v.is_empty()).map(PathBuf::from)
        .unwrap_or_else(|| Path::new(&std::env::var("HOME").unwrap_or_default()).join(".local").join("share"))
}

/// The desktop folder (xdg-user-dir DESKTOP), when there is one.
fn desktop_dir() -> Option<PathBuf> {
    let out = Command::new("xdg-user-dir").arg("DESKTOP").stdin(Stdio::null()).stderr(Stdio::null()).output().ok()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string()).filter(|d| !d.is_empty());
    let home = std::env::var("HOME").unwrap_or_default();
    let dir = PathBuf::from(out.unwrap_or_else(|| format!("{home}/Desktop")));
    (dir.is_dir() && dir != Path::new(&home)).then_some(dir)
}

/// Steam's own folder (native, or the Flatpak's).
fn steam_dir() -> Option<PathBuf> {
    let home = PathBuf::from(std::env::var("HOME").ok()?);
    [".local/share/Steam", ".steam/steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam"]
        .iter().map(|p| home.join(p)).find(|p| p.join("userdata").is_dir())
}

fn steam_running() -> bool {
    all_pids().into_iter().any(|pid| std::fs::read_to_string(format!("/proc/{pid}/comm")).is_ok_and(|c| c.trim() == "steam"))
}

/// The shortcut lists Steam keeps, one per account that signed in on this PC (userdata/<id>/config/shortcuts.vdf).
fn shortcut_files(steam: &Path) -> Vec<PathBuf> {
    let Ok(dir) = std::fs::read_dir(steam.join("userdata")) else { return Vec::new() };
    dir.flatten()
        .filter(|e| e.file_name().to_str().is_some_and(|n| n != "0" && n.bytes().all(|b| b.is_ascii_digit())))
        .map(|e| e.path().join("config"))
        .filter(|c| c.is_dir())
        .map(|c| c.join("shortcuts.vdf"))
        .collect()
}

/// Already in the library: in the list of the account that signed in last - or, when which account that is is not
/// certain (`signed_in_list`), in every account's list. An entry in another account's list alone does not count.
fn already_added(steam: &Path, exe: &Path) -> bool {
    match signed_in_list(steam) {
        Ok(list) => entries_in(&list, exe) > 0,
        Err(_) => {
            let lists = shortcut_files(steam);
            !lists.is_empty() && lists.iter().all(|l| entries_in(l, exe) > 0)
        }
    }
}

/// How many entries of a shortcut list start this program (0: none, or the list is not there or not read).
fn entries_in(list: &Path, exe: &Path) -> usize {
    std::fs::read(list).ok().and_then(|b| vdf::parse(&b)).map_or(0, |v| vdf::count_exe(&v, exe))
}

/// Each account's list with its entries for this program, before Steam is asked to add it.
fn entries(steam: &Path, exe: &Path) -> Vec<(PathBuf, usize)> {
    shortcut_files(steam).into_iter().map(|l| {
        let n = entries_in(&l, exe);
        (l, n)
    }).collect()
}

/// Steam added it: a list has an entry for it more than `before`.
fn added_since(before: &[(PathBuf, usize)], exe: &Path) -> bool {
    before.iter().any(|(l, n)| entries_in(l, exe) > *n)
}

/// What to do by hand when the setup cannot add the game itself.
const BY_HAND: &str = "In Steam: Games > Add a Non-Steam Game to My Library > Browse, file type All Files, pick \"Transformers Armada\" in the install folder.";

/// The game added to Steam as a non-Steam game: by Steam itself while it runs (steam://addnonsteamgame, as SteamOS's
/// "Add to Steam" does), else into the shortcut list of the account that signed in last, only when nothing about it is
/// in doubt (`add_while_closed`). (true, what was done) - or (false, why not and what to do by hand).
///
/// Known limit (for a player report of the game listed twice in Steam): Steam running, several Steam accounts on the
/// PC, none marked as the last to sign in (current Steam on Bazzite marks none), and the game already in some of their
/// lists but not all. Which account is signed in cannot be told then (loginusers.vdf marks none, registry.vdf names no
/// active user), so `already_added` does not count it as added and Steam is asked to add it: the signed-in account
/// gets a second entry if it had one already. Removing one in Steam (Manage > Remove non-Steam game from your library)
/// is all it takes.
fn add_to_steam(exe: &Path, dir: &Path, icon: Option<&Path>) -> (bool, String) {
    let Some(steam) = steam_dir() else { return (false, "Steam is not installed: add \"Transformers Armada\" from the install folder to your launcher.".into()) };
    if already_added(&steam, exe) {
        return (true, "Already in your Steam library as Transformers Armada.".into());
    }
    if steam_running() {
        // Steam's own way, as SteamOS's "Add to Steam" does it - but not through its script, steamos-add-to-steam: it
        // asks KDE for the file's type and accepts a shell script only as "application/x-shellscript", and newer
        // systems (Bazzite 45, 14 Nov 2026) call it "text/x-shellscript", so it refuses every script. Steam adds the
        // game only while /tmp/addnonsteamgamefile is there; it names it after the file ("Transformers Armada").
        let real = std::fs::canonicalize(exe).unwrap_or_else(|_| exe.to_path_buf());
        let url = format!("steam://addnonsteamgame/{}", percent_encode(&real.to_string_lossy()));
        let before = entries(&steam, exe);
        let _ = std::fs::write("/tmp/addnonsteamgamefile", b"");
        let flatpak = steam.to_string_lossy().contains("/.var/app/com.valvesoftware.Steam/");
        let mut ask = if flatpak && host_has("flatpak") {
            let mut c = Command::new("flatpak");
            c.args(["run", "com.valvesoftware.Steam"]);
            c
        } else if host_has("steam") {
            Command::new("steam")
        } else {
            Command::new("xdg-open")
        };
        // (with Steam running, `steam <url>` hands the address to it and ends at once)
        let asked = ask.arg(&url).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status();
        if !asked.is_ok_and(|s| s.success()) {
            return (false, format!("Steam could not be asked to add it. {BY_HAND}"));
        }
        // (Steam writes the signed-in account's list at once: a list with one entry more for it is looked for, a few
        // seconds - whichever account that is, so it is found when Steam names none as the last to sign in too)
        for _ in 0..30 {
            std::thread::sleep(std::time::Duration::from_millis(500));
            if added_since(&before, exe) {
                return (true, "Added to your Steam library as Transformers Armada.".into());
            }
        }
        return (false, format!("Steam did not add it. {BY_HAND}"));
    }
    match add_while_closed(&steam, exe, dir, icon, &steam_running) {
        Ok(()) => (true, "Added to your Steam library as Transformers Armada (it shows when Steam starts).".into()),
        Err(why) => (false, format!("Steam's list of non-Steam games was left as it is: {why}. With Steam running, \
                                     \"tfa-setup --add-to-steam\" in the install folder adds the game. {BY_HAND}")),
    }
}

/// Steam closed: the game added to the shortcut list of the account that signed in last (Steam reads it when it
/// starts). The list is changed only when all of this holds, and is otherwise left as it is (Err: why):
/// - which account signed in last is certain (`signed_in_list`); no other account's list is touched;
/// - the list is there (none is made: Steam makes it the first time it adds a non-Steam game itself), and it is read
///   exactly: parsed and written back, it gives the same bytes;
/// - a backup of it (shortcuts.vdf.tfa-backup beside it) is written and reads back the same, before anything else;
/// - Steam has not started meanwhile (it would write its own list over this one when it closes).
/// The new list is written beside it (shortcuts.vdf.tfa-new, the old one's permissions) and renamed over it, then read
/// back: not the bytes written, the old list is put back the same way. The .tfa-new file never stays behind.
fn add_while_closed(steam: &Path, exe: &Path, dir: &Path, icon: Option<&Path>, steam_started: &dyn Fn() -> bool) -> Result<(), String> {
    use std::io::Write;
    let list = signed_in_list(steam)?;
    if !list.is_file() {
        return Err("your Steam account has no list of non-Steam games yet, and the setup does not make one".into());
    }
    let old = std::fs::read(&list).map_err(|e| format!("it could not be read ({e})"))?;
    let mut v = vdf::parse(&old).filter(|v| vdf::write(v) == old).ok_or("it is not in a form the setup reads")?;
    vdf::add_shortcut(&mut v, "Transformers Armada", exe, dir, icon);
    let new = vdf::write(&v);
    if !vdf::parse(&new).is_some_and(|w| vdf::has_exe(&w, exe)) {
        return Err("the new list did not check out".into());
    }
    let backup = list.with_file_name("shortcuts.vdf.tfa-backup");
    if std::fs::write(&backup, &old).is_err() || std::fs::read(&backup).ok().as_deref() != Some(&old[..]) {
        let _ = std::fs::remove_file(&backup);
        return Err("a backup of it could not be made".into());
    }
    let permissions = std::fs::metadata(&list).map_err(|e| format!("it could not be read ({e})"))?.permissions();
    let tmp = list.with_file_name("shortcuts.vdf.tfa-new");
    let write_tmp = |bytes: &[u8]| -> std::io::Result<()> {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
        std::fs::set_permissions(&tmp, permissions.clone())
    };
    let given_up = |why: String| {
        let _ = std::fs::remove_file(&tmp);
        Err(why)
    };
    if let Err(e) = write_tmp(&new) {
        return given_up(format!("the new list could not be written ({e})"));
    }
    if steam_started() {
        return given_up("Steam was started meanwhile".into());
    }
    if let Err(e) = std::fs::rename(&tmp, &list) {
        return given_up(format!("it could not be replaced ({e})"));
    }
    if std::fs::read(&list).ok().as_deref() != Some(&new[..]) {
        let restored = write_tmp(&old).and_then(|_| std::fs::rename(&tmp, &list)).is_ok()
            && std::fs::read(&list).ok().as_deref() == Some(&old[..]);
        let _ = std::fs::remove_file(&tmp);
        return Err(if restored {
            "it did not read back as written, so the old list was put back".into()
        } else {
            "it did not read back as written, and the old list could not be put back: it is in shortcuts.vdf.tfa-backup beside it".into()
        });
    }
    Ok(())
}

/// The shortcut list of the Steam account that signed in last on this PC (userdata/<id>/config/shortcuts.vdf; the file
/// may not exist yet): the account config/loginusers.vdf marks "MostRecent" "1" - its folder's name is its SteamID64
/// less 76561197960265728 - or, when that file marks none (or is not there), the only account folder there is.
/// Err when that is not certain.
fn signed_in_list(steam: &Path) -> Result<PathBuf, String> {
    const STEAMID64_BASE: u64 = 76_561_197_960_265_728;
    let recent = std::fs::read_to_string(steam.join("config").join("loginusers.vdf")).map(|t| most_recent_accounts(&t)).unwrap_or_default();
    match recent.as_slice() {
        [id] => id.checked_sub(STEAMID64_BASE)
            .map(|n| steam.join("userdata").join(n.to_string()).join("config"))
            .filter(|config| config.is_dir())
            .map(|config| config.join("shortcuts.vdf"))
            .ok_or_else(|| "the Steam account that signed in last has no folder of its own here".into()),
        [] => match shortcut_files(steam).as_slice() {
            [only] => Ok(only.clone()),
            [] => Err("no Steam account has signed in on this PC yet".into()),
            _ => Err("more than one Steam account has signed in on this PC, and Steam does not say which was last".into()),
        },
        _ => Err("Steam marks more than one account as the last to sign in".into()),
    }
}

/// The accounts Steam's config/loginusers.vdf (its text format: "users" { "<SteamID64>" { "MostRecent" "1" ... } })
/// marks as the last to sign in.
fn most_recent_accounts(text: &str) -> Vec<u64> {
    let mut out = Vec::new();
    let mut open: Vec<String> = Vec::new(); // (the keys of the maps the reading is in)
    let mut key: Option<String> = None;
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '"' => {
                let mut s = String::new();
                while let Some(d) = chars.next() {
                    match d {
                        '"' => break,
                        '\\' => s.extend(chars.next()),
                        _ => s.push(d),
                    }
                }
                match key.take() {
                    None => key = Some(s),
                    Some(k) => {
                        if open.len() == 2 && open[0].eq_ignore_ascii_case("users") && k.eq_ignore_ascii_case("MostRecent") && s == "1" {
                            out.extend(open[1].parse::<u64>().ok());
                        }
                    }
                }
            }
            '{' => open.push(key.take().unwrap_or_default()),
            '}' => {
                open.pop();
                key = None;
            }
            '/' if chars.peek() == Some(&'/') => {
                for d in chars.by_ref() {
                    if d == '\n' {
                        break;
                    }
                }
            }
            _ => {}
        }
    }
    out
}

fn percent_encode(s: &str) -> String {
    s.bytes().map(|b| if b.is_ascii_alphanumeric() || b"-_.~".contains(&b) { (b as char).to_string() } else { format!("%{b:02X}") }).collect()
}

/// Steam's binary VDF (shortcuts.vdf): maps of strings, 32-bit numbers and maps.
pub mod vdf {
    use std::path::Path;

    #[derive(Clone, Debug, PartialEq)]
    pub enum Value {
        Map(Vec<(Vec<u8>, Value)>),
        Str(Vec<u8>),
        Int(u32),
        Float(u32),
        U64(u64),
    }

    #[cfg(test)]
    pub fn empty() -> Value {
        Value::Map(vec![(b"shortcuts".to_vec(), Value::Map(Vec::new()))])
    }

    /// The whole file (None: not a file this reads exactly - left alone).
    pub fn parse(bytes: &[u8]) -> Option<Value> {
        let mut at = 0;
        let v = map(bytes, &mut at)?;
        // (the file ends with the top map's end mark; nothing may follow)
        (at == bytes.len()).then_some(v)
    }

    fn cstr(b: &[u8], at: &mut usize) -> Option<Vec<u8>> {
        let end = b[*at..].iter().position(|&c| c == 0)? + *at;
        let s = b[*at..end].to_vec();
        *at = end + 1;
        Some(s)
    }

    fn map(b: &[u8], at: &mut usize) -> Option<Value> {
        let mut items = Vec::new();
        loop {
            let kind = *b.get(*at)?;
            *at += 1;
            if kind == 8 {
                return Some(Value::Map(items));
            }
            let key = cstr(b, at)?;
            let value = match kind {
                0 => map(b, at)?,
                1 => Value::Str(cstr(b, at)?),
                2 | 3 => {
                    let n = u32::from_le_bytes(b.get(*at..*at + 4)?.try_into().ok()?);
                    *at += 4;
                    if kind == 2 { Value::Int(n) } else { Value::Float(n) }
                }
                7 => {
                    let n = u64::from_le_bytes(b.get(*at..*at + 8)?.try_into().ok()?);
                    *at += 8;
                    Value::U64(n)
                }
                _ => return None,
            };
            items.push((key, value));
        }
    }

    pub fn write(v: &Value) -> Vec<u8> {
        let mut out = Vec::new();
        if let Value::Map(items) = v {
            write_items(items, &mut out);
        }
        out
    }

    fn write_items(items: &[(Vec<u8>, Value)], out: &mut Vec<u8>) {
        for (k, v) in items {
            let kind = match v {
                Value::Map(_) => 0,
                Value::Str(_) => 1,
                Value::Int(_) => 2,
                Value::Float(_) => 3,
                Value::U64(_) => 7,
            };
            out.push(kind);
            out.extend_from_slice(k);
            out.push(0);
            match v {
                Value::Map(m) => write_items(m, out),
                Value::Str(s) => {
                    out.extend_from_slice(s);
                    out.push(0);
                }
                Value::Int(n) | Value::Float(n) => out.extend_from_slice(&n.to_le_bytes()),
                Value::U64(n) => out.extend_from_slice(&n.to_le_bytes()),
            }
        }
        out.push(8);
    }

    fn shortcuts(v: &Value) -> &[(Vec<u8>, Value)] {
        if let Value::Map(top) = v {
            if let Some((_, Value::Map(list))) = top.iter().find(|(k, _)| k.eq_ignore_ascii_case(b"shortcuts")) {
                return list;
            }
        }
        &[]
    }

    fn field<'a>(entry: &'a Value, name: &[u8]) -> Option<&'a Value> {
        if let Value::Map(m) = entry { m.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v) } else { None }
    }

    /// An entry that starts this program (its path, with or without quotes; the same file by another path too -
    /// SteamOS's "Add to Steam" records the resolved one: /var/home/... on Bazzite, where $HOME is /home/...).
    pub fn has_exe(v: &Value, exe: &Path) -> bool {
        count_exe(v, exe) > 0
    }

    /// How many entries start this program (has_exe's comparison).
    pub fn count_exe(v: &Value, exe: &Path) -> usize {
        let real = |p: &Path| std::fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf());
        let want = real(exe);
        shortcuts(v).iter().filter(|(_, e)| match field(e, b"Exe") {
            Some(Value::Str(s)) => {
                let got = String::from_utf8_lossy(s);
                let got = Path::new(got.trim().trim_matches('"'));
                got == exe || real(got) == want
            }
            _ => false,
        }).count()
    }

    /// Steam's id for a non-Steam game: CRC-32 of the quoted program and the name, top bit set (what Steam computes).
    pub fn app_id(exe_quoted: &str, name: &str) -> u32 {
        let mut crc = 0xFFFF_FFFFu32;
        for b in exe_quoted.bytes().chain(name.bytes()) {
            crc ^= b as u32;
            for _ in 0..8 {
                crc = if crc & 1 != 0 { (crc >> 1) ^ 0xEDB8_8320 } else { crc >> 1 };
            }
        }
        !crc | 0x8000_0000
    }

    /// A non-Steam game added at the end of the list (the next number as its key).
    pub fn add_shortcut(v: &mut Value, name: &str, exe: &Path, dir: &Path, icon: Option<&Path>) {
        let Value::Map(top) = v else { return };
        if !top.iter().any(|(k, _)| k.eq_ignore_ascii_case(b"shortcuts")) {
            top.push((b"shortcuts".to_vec(), Value::Map(Vec::new())));
        }
        let Some((_, Value::Map(list))) = top.iter_mut().find(|(k, _)| k.eq_ignore_ascii_case(b"shortcuts")) else { return };
        let next = list.iter().filter_map(|(k, _)| std::str::from_utf8(k).ok()?.parse::<u32>().ok()).max().map_or(0, |n| n + 1);
        let exe_q = format!("\"{}\"", exe.to_string_lossy());
        let dir_q = format!("\"{}/\"", dir.to_string_lossy().trim_end_matches('/'));
        let s = |t: &str| Value::Str(t.as_bytes().to_vec());
        let entry = vec![
            (b"appid".to_vec(), Value::Int(app_id(&exe_q, name))),
            (b"AppName".to_vec(), s(name)),
            (b"Exe".to_vec(), s(&exe_q)),
            (b"StartDir".to_vec(), s(&dir_q)),
            (b"icon".to_vec(), s(&icon.map(|p| p.to_string_lossy().to_string()).unwrap_or_default())),
            (b"ShortcutPath".to_vec(), s("")),
            (b"LaunchOptions".to_vec(), s("")),
            (b"IsHidden".to_vec(), Value::Int(0)),
            (b"AllowDesktopConfig".to_vec(), Value::Int(1)),
            (b"AllowOverlay".to_vec(), Value::Int(1)),
            (b"OpenVR".to_vec(), Value::Int(0)),
            (b"Devkit".to_vec(), Value::Int(0)),
            (b"DevkitGameID".to_vec(), s("")),
            (b"DevkitOverrideAppID".to_vec(), Value::Int(0)),
            (b"LastPlayTime".to_vec(), Value::Int(0)),
            (b"FlatpakAppID".to_vec(), s("")),
            (b"tags".to_vec(), Value::Map(Vec::new())),
        ];
        list.push((next.to_string().into_bytes(), Value::Map(entry)));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vdf_round_trip_and_add() {
        let mut v = vdf::empty();
        vdf::add_shortcut(&mut v, "Other", Path::new("/opt/other"), Path::new("/opt"), None);
        let bytes = vdf::write(&v);
        assert_eq!(vdf::parse(&bytes).as_ref(), Some(&v));
        let mut w = vdf::parse(&bytes).unwrap();
        let exe = Path::new("/home/deck/Games/TFA/Transformers Armada");
        assert!(!vdf::has_exe(&w, exe));
        vdf::add_shortcut(&mut w, "Transformers Armada", exe, Path::new("/home/deck/Games/TFA"), None);
        let again = vdf::parse(&vdf::write(&w)).unwrap();
        assert!(vdf::has_exe(&again, exe));
        assert!(vdf::has_exe(&again, Path::new("/opt/other")));
        // (a real list, when one is given: read and written back byte for byte)
        if let Ok(path) = std::env::var("TFA_VDF_SAMPLE") {
            let real = std::fs::read(path).unwrap();
            let parsed = vdf::parse(&real).expect("the real shortcuts.vdf is read");
            assert_eq!(vdf::write(&parsed), real);
            let mut more = parsed.clone();
            vdf::add_shortcut(&mut more, "Transformers Armada", exe, Path::new("/home/deck/Games/TFA"), None);
            assert!(vdf::has_exe(&vdf::parse(&vdf::write(&more)).unwrap(), exe));
        }
    }

    // (A Steam folder of the test's own in the temp folder - never the real one: userdata/<id>/config for each account,
    // with a list holding one other game when asked.)
    fn fake_steam(name: &str, accounts: &[(u64, bool)]) -> PathBuf {
        let steam = std::env::temp_dir().join(format!("tfa_fake_steam_{}_{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&steam);
        for &(id, with_list) in accounts {
            let config = steam.join("userdata").join(id.to_string()).join("config");
            std::fs::create_dir_all(&config).unwrap();
            if with_list {
                let mut v = vdf::empty();
                vdf::add_shortcut(&mut v, "Other", Path::new("/opt/other"), Path::new("/opt"), None);
                std::fs::write(config.join("shortcuts.vdf"), vdf::write(&v)).unwrap();
            }
        }
        steam
    }

    // (config/loginusers.vdf as Steam writes it, one entry per account folder id: (id, "MostRecent"))
    fn login_users(steam: &Path, accounts: &[(u64, bool)]) {
        let mut text = String::from("\"users\"\n{\n");
        for &(id, last) in accounts {
            text += &format!("\t\"{}\"\n\t{{\n\t\t\"AccountName\"\t\t\"a{id}\"\n\t\t\"MostRecent\"\t\t\"{}\"\n\t}}\n",
                             id + 76_561_197_960_265_728, u8::from(last));
        }
        text += "}\n";
        std::fs::create_dir_all(steam.join("config")).unwrap();
        std::fs::write(steam.join("config").join("loginusers.vdf"), text).unwrap();
    }

    fn list_of(steam: &Path, id: u64) -> PathBuf {
        steam.join("userdata").join(id.to_string()).join("config").join("shortcuts.vdf")
    }

    // (the files in an account's config folder: nothing beside the list may appear when it is left alone)
    fn config_files(steam: &Path, id: u64) -> Vec<String> {
        let mut names: Vec<String> = std::fs::read_dir(list_of(steam, id).parent().unwrap()).unwrap().flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        names.sort();
        names
    }

    const GAME: &str = "/home/deck/Games/TFA/Transformers Armada";

    fn add_closed(steam: &Path, started: bool) -> Result<(), String> {
        add_while_closed(steam, Path::new(GAME), Path::new("/home/deck/Games/TFA"), None, &|| started)
    }

    #[test]
    fn steam_closed_adds_to_the_last_signed_in_account_only() {
        use std::os::unix::fs::PermissionsExt;
        let steam = fake_steam("two", &[(111, true), (222, true)]);
        login_users(&steam, &[(111, false), (222, true)]);
        let (first, second) = (std::fs::read(list_of(&steam, 111)).unwrap(), std::fs::read(list_of(&steam, 222)).unwrap());
        std::fs::set_permissions(list_of(&steam, 222), std::fs::Permissions::from_mode(0o755)).unwrap();
        add_closed(&steam, false).unwrap();
        assert_eq!(std::fs::read(list_of(&steam, 111)).unwrap(), first, "the other account's list is untouched");
        assert_eq!(config_files(&steam, 111), ["shortcuts.vdf"]);
        let now = vdf::parse(&std::fs::read(list_of(&steam, 222)).unwrap()).expect("the new list reads back");
        assert!(vdf::has_exe(&now, Path::new(GAME)) && vdf::has_exe(&now, Path::new("/opt/other")), "added, the other game kept");
        let backup = list_of(&steam, 222).with_file_name("shortcuts.vdf.tfa-backup");
        assert_eq!(std::fs::read(backup).unwrap(), second, "the backup is the list as it was");
        assert_eq!(config_files(&steam, 222), ["shortcuts.vdf", "shortcuts.vdf.tfa-backup"], "no .tfa-new left");
        assert_eq!(std::fs::metadata(list_of(&steam, 222)).unwrap().permissions().mode() & 0o777, 0o755, "permissions kept");
        let _ = std::fs::remove_dir_all(&steam);
    }

    #[test]
    fn steam_closed_leaves_a_missing_or_unreadable_list_alone() {
        use std::os::unix::fs::PermissionsExt;
        let steam = fake_steam("missing", &[(111, false)]);
        let why = add_closed(&steam, false).unwrap_err();
        assert!(why.contains("no list of non-Steam games yet"), "{why}");
        assert!(config_files(&steam, 111).is_empty(), "none made");
        // (a list in a form the setup does not read)
        std::fs::write(list_of(&steam, 111), b"not a list").unwrap();
        let why = add_closed(&steam, false).unwrap_err();
        assert!(why.contains("not in a form the setup reads"), "{why}");
        assert_eq!(std::fs::read(list_of(&steam, 111)).unwrap(), b"not a list");
        assert_eq!(config_files(&steam, 111), ["shortcuts.vdf"], "no backup, no .tfa-new");
        // (a list that cannot be read - not for root, who reads anything)
        if unsafe { libc::geteuid() } != 0 {
            let mut v = vdf::empty();
            vdf::add_shortcut(&mut v, "Other", Path::new("/opt/other"), Path::new("/opt"), None);
            std::fs::write(list_of(&steam, 111), vdf::write(&v)).unwrap();
            std::fs::set_permissions(list_of(&steam, 111), std::fs::Permissions::from_mode(0o000)).unwrap();
            let why = add_closed(&steam, false).unwrap_err();
            std::fs::set_permissions(list_of(&steam, 111), std::fs::Permissions::from_mode(0o644)).unwrap();
            assert!(why.contains("could not be read"), "{why}");
            assert_eq!(std::fs::read(list_of(&steam, 111)).unwrap(), vdf::write(&v));
            assert_eq!(config_files(&steam, 111), ["shortcuts.vdf"]);
        }
        let _ = std::fs::remove_dir_all(&steam);
    }

    #[test]
    fn steam_closed_changes_nothing_when_the_account_is_not_certain() {
        let steam = fake_steam("unsure", &[(111, true), (222, true)]);
        let (first, second) = (std::fs::read(list_of(&steam, 111)).unwrap(), std::fs::read(list_of(&steam, 222)).unwrap());
        // (no loginusers.vdf, two accounts; none marked; two marked; the marked one without a folder here)
        let cases: [(&[(u64, bool)], &str); 4] = [
            (&[], "more than one Steam account"),
            (&[(111, false), (222, false)], "more than one Steam account"),
            (&[(111, true), (222, true)], "more than one account as the last"),
            (&[(111, false), (333, true)], "no folder of its own"),
        ];
        for (users, expected) in cases {
            if !users.is_empty() {
                login_users(&steam, users);
            }
            let why = add_closed(&steam, false).unwrap_err();
            assert!(why.contains(expected), "{why}");
        }
        assert_eq!(std::fs::read(list_of(&steam, 111)).unwrap(), first);
        assert_eq!(std::fs::read(list_of(&steam, 222)).unwrap(), second);
        assert_eq!((config_files(&steam, 111), config_files(&steam, 222)), (vec!["shortcuts.vdf".to_string()], vec!["shortcuts.vdf".to_string()]));
        let _ = std::fs::remove_dir_all(&steam);
        // (one account folder and nothing saying otherwise: no doubt, that one)
        let one = fake_steam("one", &[(111, true)]);
        add_closed(&one, false).unwrap();
        assert!(vdf::has_exe(&vdf::parse(&std::fs::read(list_of(&one, 111)).unwrap()).unwrap(), Path::new(GAME)));
        let _ = std::fs::remove_dir_all(&one);
    }

    #[test]
    fn steam_started_meanwhile_changes_nothing() {
        let steam = fake_steam("started", &[(111, true)]);
        let before = std::fs::read(list_of(&steam, 111)).unwrap();
        let why = add_closed(&steam, true).unwrap_err();
        assert!(why.contains("Steam was started meanwhile"), "{why}");
        assert_eq!(std::fs::read(list_of(&steam, 111)).unwrap(), before);
        assert!(!list_of(&steam, 111).with_file_name("shortcuts.vdf.tfa-new").exists());
        let _ = std::fs::remove_dir_all(&steam);
    }

    #[test]
    fn already_added_means_in_this_accounts_list() {
        let steam = fake_steam("already", &[(111, true), (222, true)]);
        let exe = Path::new(GAME);
        let add_to = |id: u64| {
            let mut v = vdf::parse(&std::fs::read(list_of(&steam, id)).unwrap()).unwrap();
            vdf::add_shortcut(&mut v, "Transformers Armada", exe, Path::new("/home/deck/Games/TFA"), None);
            std::fs::write(list_of(&steam, id), vdf::write(&v)).unwrap();
        };
        add_to(111);
        login_users(&steam, &[(111, false), (222, true)]);
        assert!(!already_added(&steam, exe), "another account's entry is not this one's");
        // (which account is not certain: only an entry in every account's list counts)
        std::fs::remove_file(steam.join("config").join("loginusers.vdf")).unwrap();
        assert!(!already_added(&steam, exe));
        add_to(222);
        assert!(already_added(&steam, exe));
        login_users(&steam, &[(111, false), (222, true)]);
        assert!(already_added(&steam, exe));
        // (Steam's own add, while it runs: seen as one entry more in a list - a second one too)
        let before = entries(&steam, exe);
        assert_eq!(before.iter().map(|(_, n)| n).sum::<usize>(), 2);
        assert!(!added_since(&before, exe));
        add_to(222);
        assert!(added_since(&before, exe));
        let _ = std::fs::remove_dir_all(&steam);
    }

    #[test]
    fn last_signed_in_account_read_from_loginusers() {
        // (Steam's text format: escaped quotes, a comment, the key in another case)
        let text = "// written by Steam\n\"users\"\n{\n\t\"76561197960265839\"\n\t{\n\t\t\"PersonaName\"\t\t\"a \\\"quoted\\\" {name}\"\n\t\t\"MostRecent\"\t\t\"0\"\n\t}\n\
                    \t\"76561197960265950\"\n\t{\n\t\t\"mostrecent\"\t\t\"1\"\n\t}\n}\n";
        assert_eq!(most_recent_accounts(text), [76_561_197_960_265_950]);
        assert!(most_recent_accounts("\"other\" { \"1\" { \"MostRecent\" \"1\" } }").is_empty());
    }

    #[test]
    fn shortcut_id_is_steams() {
        // (the id Steam Rom Manager and Steam compute for "\"/usr/bin/true\"" + "True")
        assert_eq!(vdf::app_id("\"/usr/bin/true\"", "True") & 0x8000_0000, 0x8000_0000);
    }

    #[test]
    fn packages_read_from_setup_sh() {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
        let pkgs = ubuntu_packages(&repo, false).unwrap();
        assert!(pkgs.iter().any(|p| p == "cmake"));
        assert!(pkgs.iter().any(|p| p == "libavcodec-dev"));
        assert!(pkgs.iter().all(|p| !p.contains('\\') && !p.starts_with("sudo")));
        // (the fonts' Python packages with or without the HD textures; the Vulkan driver only with them; each once)
        assert!(pkgs.iter().any(|p| p == "python3-numpy") && pkgs.iter().any(|p| p == "python3-pil"));
        assert!(!pkgs.iter().any(|p| p == "mesa-vulkan-drivers"));
        let with_hd = ubuntu_packages(&repo, true).unwrap();
        assert!(with_hd.iter().any(|p| p == "mesa-vulkan-drivers"));
        assert_eq!(with_hd.iter().filter(|p| *p == "curl").count(), 1);
        // (a checkout from before --packages: its setup.sh stops at the unknown option; the list is read from its message)
        let old = std::env::temp_dir().join(format!("tfa_old_setup_{}", std::process::id()));
        std::fs::create_dir_all(&old).unwrap();
        std::fs::write(old.join("setup.sh"), "case \"$1\" in *) echo \"unknown option: $1\" >&2; exit 2 ;; esac\n\
            cat <<'EOF'\nUbuntu / Debian:\n  sudo apt install git cmake \\\n    libavcodec-dev\nFedora:\n  sudo dnf install x\nEOF\n").unwrap();
        assert_eq!(ubuntu_packages(&old, false).unwrap(), ["git", "cmake", "libavcodec-dev", "python3-pil", "python3-numpy"]);
        let _ = std::fs::remove_dir_all(&old);
    }

    #[test]
    fn container_kept_in_setup_json() {
        let dir = std::env::temp_dir().join(format!("tfa_container_test_{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        assert_eq!(recorded_container(&dir), None);
        record_container(&dir, "tfa-build");
        assert_eq!(recorded_container(&dir).as_deref(), Some("tfa-build"));
        std::fs::write(dir.join(".setup.json"), "{\n  \"Iso\": \"/x.iso\",\n  \"SetUpAt\": \"abc\"\n}").unwrap();
        record_container(&dir, "tfa-build");
        let text = std::fs::read_to_string(dir.join(".setup.json")).unwrap();
        assert!(text.contains("\"Iso\": \"/x.iso\"") && text.contains("\"SetUpAt\": \"abc\""));
        assert_eq!(recorded_container(&dir).as_deref(), Some("tfa-build"));
        record_container(&dir, "other");
        assert_eq!(recorded_container(&dir).as_deref(), Some("other"));
        assert!(std::fs::read_to_string(dir.join(".setup.json")).unwrap().contains("\"SetUpAt\": \"abc\""));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn own_child_exit_code_is_read_and_left_for_std() {
        let mut child = Command::new("sh").args(["-c", "exit 7"]).spawn().unwrap();
        assert_eq!(wait_for_exit(child.id()), Some(7));
        assert_eq!(process_started(child.id()), None, "ended: no start time");
        assert_eq!(child.wait().unwrap().code(), Some(7), "not collected: std's wait still gets it");
    }

    #[test]
    fn checked_waits_wait_only_for_the_process_named() {
        // (wait_for_exit_checked, LeftStep's: the start time written down must be the process's own)
        let mut child = Command::new("sh").args(["-c", "sleep 1; exit 5"]).spawn().unwrap();
        let started = process_started(child.id()).unwrap();
        let t = Instant::now();
        assert_eq!(wait_for_exit_checked(child.id(), started + 1), None, "another start time: another process");
        assert_eq!(wait_for_exit_checked(child.id(), 0), None, "no start time written down");
        assert!(t.elapsed() < std::time::Duration::from_millis(500) && child.try_wait().unwrap().is_none(), "not waited for");
        assert_eq!(wait_for_exit_checked(child.id(), started), Some(5));
        assert_eq!(child.wait().unwrap().code(), Some(5));
        // (wait_for_starter, --wait-pid's: a process that started after this program is not the one that started it)
        let mut later = Command::new("sleep").arg("30").stdin(Stdio::null()).spawn().unwrap();
        let t = Instant::now();
        assert!(!wait_for_starter(later.id(), 5_000));
        assert!(t.elapsed() < std::time::Duration::from_millis(500), "not waited for");
        let _ = later.kill();
        let _ = later.wait();
        assert!(!wait_for_starter(u32::MAX >> 1, 5_000), "no such process");
        // (the program that started this one: waited for up to the time-out, and still running after it)
        let parent = stat(std::process::id()).unwrap().parent;
        let t = Instant::now();
        assert!(wait_for_starter(parent, 300));
        assert!(t.elapsed() >= std::time::Duration::from_millis(300));
    }

    #[test]
    fn a_tree_waits_for_and_ends_its_own_processes_only() {
        // (TODO 370, as launcher.rs's Windows test: the first process starts the next and ends; the next carries the
        // tree's mark and runs until the test creates a file. A second's wait first: the mark is read while the first
        // runs, as for a step. A stranger, not of the tree, runs beside it.)
        let release = std::env::temp_dir().join(format!("tfa-tree-release-{}", std::process::id()));
        let _ = std::fs::remove_file(&release);
        let script = format!("sleep 1; (i=0; while [ ! -e '{}' ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i+1)); done) &",
                             release.display());
        let quiet = |c: &mut Command| c.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn().unwrap();
        let mut first = quiet(Command::new("sh").args(["-c", &script]).env(TREE_VAR, new_mark()));
        let tree = ProcessTree::new(&first, false);
        assert!(first.wait().unwrap().success());
        let mut stranger = quiet(Command::new("sleep").arg("30"));
        assert_eq!(tree.wait_for_member(stranger.id()), None, "a stranger is not waited for");
        let left = tree.processes();
        assert!(!left.is_empty(), "the started-again process is still in the tree");
        assert!(!left.contains(&stranger.id()) && !left.contains(&first.id()));
        let released = Arc::new(AtomicBool::new(false));
        let releaser = {
            let (released, release) = (released.clone(), release.clone());
            std::thread::spawn(move || {
                std::thread::sleep(std::time::Duration::from_millis(500));
                released.store(true, Ordering::SeqCst);
                std::fs::write(&release, b"release").unwrap();
            })
        };
        // (launcher.rs's wait_for_rest, as it runs on Linux: no exit code from a process that is not this program's child)
        while let Some(&pid) = tree.processes().first() {
            if tree.wait_for_member(pid).is_none() {
                std::thread::sleep(std::time::Duration::from_millis(20));
            }
        }
        assert!(released.load(Ordering::SeqCst), "waited for the started-again process");
        releaser.join().unwrap();
        let _ = std::fs::remove_file(&release);
        // (Cancel: a running tree ended through its pidfds, the stranger left running)
        let mut step = quiet(Command::new("sh").args(["-c", "sleep 30 & wait"]));
        let step_tree = ProcessTree::new(&step, false);
        std::thread::sleep(std::time::Duration::from_millis(300));
        assert_eq!(step_tree.processes().len(), 2, "sh and its sleep");
        step_tree.terminate();
        assert!(step.wait().unwrap().code().is_none(), "ended by a signal");
        assert!(step_tree.processes().is_empty());
        assert!(stranger.try_wait().unwrap().is_none(), "the stranger still runs");
        let _ = stranger.kill();
        let _ = stranger.wait();
    }

    #[test]
    fn amd_names_as_mesa_gives_them() {
        // (only the cards the distribution's list knows: an older one - Ubuntu 22.04's - predates the RX 9070)
        let ids = std::fs::read_to_string("/usr/share/libdrm/amdgpu.ids").unwrap_or_default();
        if ids.contains("7550,\tC0,") {
            assert_eq!(amdgpu_name(0x7550, 0xC0).as_deref(), Some("AMD Radeon RX 9070 XT"));
        }
        if ids.contains("163F,\tAE,") {
            assert_eq!(amdgpu_name(0x163F, 0xAE).as_deref(), Some("AMD Custom GPU 0405"));
        }
    }
}
