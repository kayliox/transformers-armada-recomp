// (TODO 439) What the setup installs and makes, said before anything runs, and the space the build can give back after.
//
// The plan: each program the build needs (Windows: Git, Python, Visual Studio's Build Tools), why, its download and
// disk size, whether it is already there; then the game built from the disc, the HD textures, and the space and time
// in all. The numbers were measured on a fresh install (the Testbench: Ryzen 5 5600, NVMe, GTX 1080, 2026-11-19; the
// README's "Install size"); keep them in step with tools\setup_common.ps1 Get-SetupSizes.
//
// The clean-up: tools\clean_build.ps1 (Linux: tools/clean_build.sh) -Report says how big the build's leftovers are,
// -Level temp / all deletes them. "temp" costs nothing; "all" makes the next update rebuild the whole game once.

use std::path::Path;
use std::process::{Command, Stdio};

use crate::NoWindow;

/// A program the build needs (Windows).
pub struct Tool {
    pub key: &'static str,
    pub name: &'static str,
    pub why: &'static str,
    pub download_mb: u32,
    pub disk_gb: f32,
    pub install_min: u32,
}

pub const TOOLS: [Tool; 3] = [
    Tool { key: "git", name: "Git", why: "gets the game's build recipe and its updates", download_mb: 67, disk_gb: 0.4, install_min: 1 },
    Tool { key: "python", name: "Python 3.12", why: "runs the build's tools and makes the HD textures", download_mb: 27, disk_gb: 0.2, install_min: 1 },
    Tool {
        key: "vs",
        name: "Visual Studio 2022 Build Tools (C++)",
        why: "Microsoft's free compiler: the game is built on this PC from your disc, since no game code may be shared",
        download_mb: 1900,
        disk_gb: 6.1,
        install_min: 10,
    },
];

/// Pillow, numpy and CMake for that Python (pip), GB.
pub const PY_PARTS_GB: f32 = 0.3;
/// The disc's files, extracted from the disc image, GB.
pub const DISC_GB: f32 = 2.7;
/// The build: the game itself and its in-between files, GB (most of it can be freed afterwards).
pub const BUILD_GB: f32 = 2.3;
/// The first build, minutes (a 6-core processor took the low end).
pub const BUILD_MIN: (u32, u32) = (12, 25);

/// What is already installed (None: not known yet, or not this platform's question).
#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct Found {
    pub git: Option<bool>,
    pub python: Option<bool>,
    pub vs: Option<bool>,
}

impl Found {
    pub fn get(&self, key: &str) -> Option<bool> {
        match key {
            "git" => self.git,
            "python" => self.python,
            _ => self.vs,
        }
    }
}

/// One line of the plan: its text and whether it says "already installed" (shown in green).
pub struct Line {
    pub text: String,
    pub present: bool,
    pub tool: bool,
}

fn download_text(mb: u32) -> String {
    if mb >= 1000 { format!("{:.1} GB", mb as f32 / 1000.0) } else { format!("{mb} MB") }
}

/// The plan's lines, and the space (GB) and time (minutes, low and high) in all. hd: the HD preset's (GB, minutes), or
/// None when they are left out. with_tools: list the programs (Windows; Linux says its own tools line).
pub fn lines(found: &Found, hd: Option<(f32, u32)>, with_tools: bool) -> (Vec<Line>, f32, (u32, u32)) {
    let mut out = Vec::new();
    let mut disk = PY_PARTS_GB + DISC_GB + BUILD_GB;
    let (mut lo, mut hi) = BUILD_MIN;
    if with_tools {
        for t in &TOOLS {
            let present = found.get(t.key) == Some(true);
            let state = match found.get(t.key) {
                Some(true) => "already installed".to_string(),
                Some(false) => format!("to install: {} download, {:.1} GB", download_text(t.download_mb), t.disk_gb),
                None => format!("{} download, {:.1} GB, if missing", download_text(t.download_mb), t.disk_gb),
            };
            if !present {
                disk += t.disk_gb;
                lo += t.install_min / 2;
                hi += t.install_min;
            }
            out.push(Line { text: format!("{}: {state}. It {}.", t.name, t.why), present, tool: true });
        }
    }
    out.push(Line {
        text: format!("The game, built from your disc: {DISC_GB:.1} GB of disc files and {BUILD_GB:.1} GB of build, about {} to {} min.", BUILD_MIN.0, BUILD_MIN.1),
        present: false,
        tool: false,
    });
    match hd {
        Some((gb, min)) => {
            disk += gb;
            lo += min;
            hi += min * 2;
            out.push(Line { text: format!("HD textures (optional): about {gb:.1} GB, about {min} min or more. You can play meanwhile."), present: false, tool: false });
        }
        None => out.push(Line { text: "HD textures: left out (the game shows its original textures).".into(), present: false, tool: false }),
    }
    (out, disk, (lo, hi))
}

/// The totals line under the plan.
pub fn total_text(disk: f32, minutes: (u32, u32)) -> String {
    format!("In all: about {} GB of free space, about {} to {} min. Nothing else is installed; afterwards most of the build's {BUILD_GB:.1} GB can be freed.",
            disk.ceil() as u32, round5(minutes.0), round5(minutes.1).max(round5(minutes.0) + 5))
}

fn round5(m: u32) -> u32 {
    m.div_ceil(5) * 5
}

/// What is installed on this PC (as tools\setup_common.ps1 Get-MissingPrerequisites decides, roughly: the setup checks
/// again itself). Slow parts (vswhere, a python started) run on the caller's thread: call it from one of its own.
pub fn detect() -> Found {
    let mut f = detect_here();
    // (a testing aid, as TFA_LOW_END: TFA_PLAN_FOUND="git=0,python=1,vs=0" stands in for what is installed - pictures of
    // the plan as a PC without the tools sees it)
    if let Ok(v) = std::env::var("TFA_PLAN_FOUND") {
        apply_found(&mut f, &v);
    }
    f
}

/// TFA_PLAN_FOUND's "name=0|1,..." over what was found.
pub fn apply_found(f: &mut Found, spec: &str) {
    for part in spec.split(',') {
        let Some((k, v)) = part.split_once('=') else { continue };
        let v = Some(v.trim() == "1");
        match k.trim() {
            "git" => f.git = v,
            "python" => f.python = v,
            "vs" => f.vs = v,
            _ => {}
        }
    }
}

#[cfg(windows)]
fn detect_here() -> Found {
    let path = std::env::var("PATH").unwrap_or_default();
    let git = crate::has(&path, "git") || crate::with_git_dir(&path) != path;
    Found { git: Some(git), python: Some(has_python(&path)), vs: Some(has_vs()) }
}

#[cfg(not(windows))]
fn detect_here() -> Found {
    Found::default()
}

/// A Python 3.11 or newer: the usual install folders (by their names), or one on PATH that says its version (not
/// Windows' Microsoft Store shortcut, WindowsApps\python.exe).
#[cfg(windows)]
fn has_python(path: &str) -> bool {
    let mut dirs = Vec::new();
    if let Ok(v) = std::env::var("LOCALAPPDATA") {
        dirs.push(Path::new(&v).join("Programs").join("Python"));
    }
    for var in ["ProgramFiles", "ProgramFiles(x86)"] {
        if let Ok(v) = std::env::var(var) {
            dirs.push(Path::new(&v).to_path_buf());
        }
    }
    for d in dirs {
        let Ok(entries) = std::fs::read_dir(&d) else { continue };
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().to_ascii_lowercase();
            if let Some(minor) = name.strip_prefix("python3").and_then(|m| m.parse::<u32>().ok()) {
                if minor >= 11 && e.path().join("python.exe").is_file() {
                    return true;
                }
            }
        }
    }
    for dir in path.split(';').filter(|d| !d.is_empty() && !d.to_ascii_lowercase().contains("windowsapps")) {
        let exe = Path::new(dir).join("python.exe");
        if !exe.is_file() {
            continue;
        }
        let out = Command::new(&exe).args(["-c", "import sys; print(sys.version_info[1])"]).stdin(Stdio::null()).stderr(Stdio::null())
            .creation_flags_no_window().output();
        if let Ok(o) = out {
            if String::from_utf8_lossy(&o.stdout).trim().parse::<u32>().is_ok_and(|m| m >= 11) {
                return true;
            }
        }
    }
    false
}

/// Visual Studio (any edition, the Build Tools too) with the C++ tools, new enough for the game (2019 version 16.11 or
/// newer, build.ps1's $MinVsVersion: an older one, Visual Studio 2017, gets the Build Tools installed beside it), and a
/// Windows SDK.
#[cfg(windows)]
fn has_vs() -> bool {
    let pf86 = std::env::var("ProgramFiles(x86)").unwrap_or_else(|_| r"C:\Program Files (x86)".into());
    let vswhere = Path::new(&pf86).join("Microsoft Visual Studio").join("Installer").join("vswhere.exe");
    let cpp = vswhere.is_file()
        && Command::new(&vswhere)
            .args(["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-version", "[16.11,", "-property", "installationPath"])
            .stdin(Stdio::null()).stderr(Stdio::null()).creation_flags_no_window().output()
            .is_ok_and(|o| !String::from_utf8_lossy(&o.stdout).trim().is_empty());
    cpp && has_windows_sdk(&pf86)
}

#[cfg(windows)]
fn has_windows_sdk(pf86: &str) -> bool {
    let mut roots = vec![Path::new(pf86).join("Windows Kits").join("10")];
    if let Ok(pf) = std::env::var("ProgramFiles") {
        roots.push(Path::new(&pf).join("Windows Kits").join("10"));
    }
    roots.iter().any(|r| {
        std::fs::read_dir(r.join("Include")).into_iter().flatten().flatten().any(|v| {
            let name = v.file_name();
            name.to_string_lossy().starts_with("10.")
                && v.path().join("um").join("windows.h").is_file()
                && r.join("Lib").join(&name).join("um").join("x64").join("kernel32.lib").is_file()
        })
    })
}

/// The build's leftovers (tools\clean_build.ps1 -Report), bytes.
#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct Leftovers {
    pub game: u64,
    pub temp: u64,
    pub build: u64,
}

/// "<kind>|<bytes>|<files>" lines into the sizes (lines it does not know are skipped).
pub fn parse_report(text: &str) -> Option<Leftovers> {
    let mut l = Leftovers::default();
    let mut any = false;
    for line in text.lines() {
        let mut parts = line.trim().split('|');
        let (Some(kind), Some(bytes)) = (parts.next(), parts.next()) else { continue };
        let Ok(bytes) = bytes.trim().parse::<u64>() else { continue };
        match kind {
            "game" => l.game = bytes,
            "temp" => l.temp = bytes,
            "build" => l.build = bytes,
            _ => continue,
        }
        any = true;
    }
    any.then_some(l)
}

pub fn gb(bytes: u64) -> f32 {
    bytes as f32 / (1u64 << 30) as f32
}

/// The clean-up script of a game folder, and how to start it.
fn clean_command(game: &Path) -> Option<Command> {
    #[cfg(windows)]
    {
        let script = game.join("tools").join("clean_build.ps1");
        if !script.is_file() {
            return None;
        }
        let mut c = Command::new(crate::program("powershell"));
        c.args(["-NoProfile", "-ExecutionPolicy", "RemoteSigned", "-File"]).arg(script);
        Some(c)
    }
    #[cfg(not(windows))]
    {
        let script = game.join("tools").join("clean_build.sh");
        if !script.is_file() {
            return None;
        }
        let mut c = Command::new("bash");
        c.arg(script);
        Some(c)
    }
}

/// The leftovers' sizes now (None: the install has no clean-up script yet, or it could not say).
pub fn report(game: &Path) -> Option<Leftovers> {
    let mut c = clean_command(game)?;
    c.arg(if cfg!(windows) { "-Report" } else { "--report" });
    let out = c.current_dir(game).stdin(Stdio::null()).stderr(Stdio::null()).creation_flags_no_window().output().ok()?;
    parse_report(&String::from_utf8_lossy(&out.stdout))
}

/// Deletes the leftovers: "temp" or "all". Ok: what the script said it freed.
pub fn clean(game: &Path, level: &str) -> Result<String, String> {
    let mut c = clean_command(game).ok_or_else(|| "this install has no clean-up script yet: update it first".to_string())?;
    if cfg!(windows) {
        c.args(["-Level", level]);
    } else {
        c.args(["--level", level]);
    }
    let out = c.current_dir(game).stdin(Stdio::null()).creation_flags_no_window().output().map_err(|e| format!("the clean-up did not start: {e}"))?;
    let text = String::from_utf8_lossy(&out.stdout).trim().to_string();
    let err = String::from_utf8_lossy(&out.stderr).trim().to_string();
    if out.status.success() {
        Ok(text.lines().next().unwrap_or("Done.").to_string())
    } else {
        Err(if text.is_empty() { err } else { text })
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn the_plan_found_can_be_stood_in_for() {
        let mut f = Found { git: Some(true), python: Some(true), vs: Some(true) };
        super::apply_found(&mut f, "git=0, vs=0,bogus=1");
        assert_eq!((f.git, f.python, f.vs), (Some(false), Some(true), Some(false)));
    }

    use super::*;

    #[test]
    fn the_report_is_read() {
        let l = parse_report("game|500000000|12\r\ntemp|300000000|20000\nbuild|4000000000|30000\nnoise\n").unwrap();
        assert_eq!(l, Leftovers { game: 500_000_000, temp: 300_000_000, build: 4_000_000_000 });
        assert!(parse_report("").is_none());
        assert!(parse_report("clean_build: the game is running").is_none());
    }

    #[test]
    fn the_plan_counts_only_what_is_missing() {
        let all = Found { git: Some(true), python: Some(true), vs: Some(true) };
        let none = Found { git: Some(false), python: Some(false), vs: Some(false) };
        let (l1, d1, t1) = lines(&all, Some((1.4, 25)), true);
        let (l2, d2, t2) = lines(&none, Some((1.4, 25)), true);
        assert!(l1.iter().filter(|l| l.tool).all(|l| l.present && l.text.contains("already installed")));
        assert!(l2.iter().any(|l| l.text.starts_with("Visual Studio 2022 Build Tools (C++): to install: 1.9 GB download, 6.1 GB")));
        assert!((d2 - d1 - 6.7).abs() < 0.01, "{d1} {d2}");
        assert!(t2.0 > t1.0 && t2.1 > t1.1);
        let (l3, d3, _) = lines(&all, None, true);
        assert!((d1 - d3 - 1.4).abs() < 0.01);
        assert!(l3.iter().any(|l| l.text.starts_with("HD textures: left out")));
        // (Linux: no program lines, the game's line first)
        let (l4, _, _) = lines(&Found::default(), Some((1.4, 25)), false);
        assert!(l4[0].text.starts_with("The game, built from your disc"));
    }

    #[test]
    fn the_totals_are_rounded_up() {
        assert_eq!(total_text(8.2, (17, 26)), format!("In all: about 9 GB of free space, about 20 to 30 min. Nothing else is installed; afterwards most of the build's {BUILD_GB:.1} GB can be freed."));
        assert!(total_text(8.0, (20, 20)).contains("about 20 to 25 min"));
    }
}
