// Transformers Armada PC version - setup (tfa-setup.exe).
//
// A window over the repository's setup.ps1: the player picks their disc image and a folder, and
// watches the steps. The work itself is the scripts' (one source of truth with the command line):
// this program fetches the repository with Git, then runs setup.ps1 and turns its "==>" lines into
// the steps below. Nothing from the discs leaves the player's PC.
//
// Opened on a folder that has the game already, it shows the installed version and checks the
// repository for a newer one (git, hidden, never asking for a sign-in: TODO 104); "Update" runs
// update.ps1 (pull, rebuild what changed; Mods\, the mods' choices and settings.ini are kept). The game's
// own "Update now" (F1, About) starts this program as `--update --play --wait-pid <the game>` when it
// is in the install folder (a successful setup puts a copy of itself there).
//
// The repository: the game's public one by default; a build made with TFA_REPO_URL set (the
// environment, at `cargo build`) or a run with --repo <url> uses another. TFA_MODS=1 at build time
// (or --mods) adds the mods' parts: the prototype disc and the Mods step (a repository that holds
// the game's folder as its port\ and builds mods on top: setup.ps1 there takes -PrototypeIso).
// The install is <folder>\<the repository's name>, a clone of it.
#![windows_subsystem = "windows"]

mod winapi;
mod launcher;

use eframe::egui::{self, Color32, RichText, Stroke, Vec2};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc::{Receiver, Sender, channel};
use std::sync::{Arc, Mutex};
use std::time::Instant;

const REPO_URL: &str = match option_env!("TFA_REPO_URL") {
    Some(url) => url,
    None => "https://github.com/kayliox/transformers-armada-recomp",
};
const MODS: bool = option_env!("TFA_MODS").is_some();
const USA_ISO_SIZE: u64 = 2_900_951_040; // Transformers (USA), SLUS-20668
const PAL_ISO_SIZE: u64 = 4_546_691_072; // Transformers (Europe), SLES-52388: experimental, maintained by a contributor
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

const ACCENT: Color32 = Color32::from_rgb(0xF0, 0x8A, 0x24);
const GOOD: Color32 = Color32::from_rgb(0x4C, 0xC3, 0x6A);
const BAD: Color32 = Color32::from_rgb(0xE5, 0x48, 0x48);
const DIM: Color32 = Color32::from_rgb(0x8A, 0x8F, 0x98);

#[derive(Clone, Copy, PartialEq)]
enum Status {
    Pending,
    Active,
    Done,
    Skipped,
    Failed,
}

struct Step {
    title: &'static str,
    detail: String,
    status: Status,
    started: Option<Instant>,
    seconds: u64,
}

enum Msg {
    Step(usize),               // this step starts (the ones before it are done)
    Detail(usize, String),     // a line of what a step is doing
    Progress(f32),             // overall, 0..1
    Finished(Result<(), String>),
    Kept(String), // the player's own changes were committed to this branch before the update
}

#[derive(PartialEq)]
enum Phase {
    Form,
    Installed, // the game is there: its version, the update check
    Running,
    Done(bool),
}

/// The update check's answer (check_updates).
#[derive(Clone, Default)]
struct Check {
    done: bool,
    available: bool,
    current: String,      // the version built, short
    current_name: String, // its number from the VERSION file ("0.1.3"), "" for a build from before the file
    date: String,
    latest: String,
    latest_name: String,
    count: usize,
    changes: Vec<String>, // "hash  subject", newest first
    error: String,
}

struct App {
    repo_url: String, // the repository the install is a clone of
    mods: bool,       // the mods' parts shown (the prototype disc, the Mods step)
    retail: String,
    prototype: String,
    folder: String,
    hd: bool,
    hd_quality: HdQuality,  // (TODO 219, PR #20) HD textures preset: Fast / Balanced / Best
    hd_saved: Option<HdQuality>, // the preset hd_options.json says (None: none saved)
    hd_set_gb: Option<f32>,      // the install's HD set (hd_textures\upscaled) now, GB (None: no set)
    hd_note: String,             // the update screen's line after Apply
    hd_while_playing: bool, // keep making them while the game runs
    hd_gentle: bool,        // (TODO 310) "Go easy on my PC": the build on half the processor threads at a lower priority, the HD textures with rests
    gpu: Option<(String, u64)>, // the graphics card the suggestion is for (name, video memory)
    low_end: bool,              // built-in or low-end graphics: no HD textures made (detect_gpu)
    hd_playable: bool,      // the HD step has got to the upscale: the game can be played meanwhile
    phase: Phase,
    steps: Vec<Step>,
    progress: f32,
    error: String,
    rx: Option<Receiver<Msg>>,
    child: Arc<Mutex<Option<Arc<winapi::ProcessTree>>>>, // the running step's process tree (Cancel ends it)
    update: bool,  // the job updates an installed copy (update.ps1) instead of setting up
    play: bool,    // the game started again after the update (the game's own "Update now")
    wait_pid: u32, // the game that asked for the update: the update waits for it to close
    kept: String,  // the branch the player's own changes were kept on ("" none)
    check: Check,
    check_rx: Option<Receiver<Check>>,
    done_at: Option<Instant>,
    // Tests (--shot <file.bmp> [--offscreen]): the window's own picture once it has settled (the
    // update check answered, or the update done), then the program closes.
    shot: Option<String>,
    shot_state: Arc<Mutex<u8>>, // 0 waiting, 1 asked, 2 written
    settled_at: Option<Instant>,
}

const STEP_TOOLS: usize = 0;
const STEP_DOWNLOAD: usize = 1;
const STEP_GAME: usize = 2;
const STEP_HD: usize = 3;
const STEP_MODS: usize = 4;

impl App {
    fn new() -> Self {
        let home = std::env::var("USERPROFILE").unwrap_or_else(|_| "C:\\".into());
        Self {
            repo_url: REPO_URL.to_string(),
            mods: MODS,
            retail: String::new(),
            prototype: String::new(),
            folder: format!("{home}\\Games\\TFA"), // (short: Windows' path limit, see exec)
            hd: true,
            hd_quality: HdQuality::Balanced,
            hd_saved: None,
            hd_set_gb: None,
            hd_note: String::new(),
            hd_while_playing: false,
            hd_gentle: false,
            gpu: None,
            low_end: false,
            hd_playable: false,
            phase: Phase::Form,
            steps: make_steps(MODS),
            progress: 0.0,
            error: String::new(),
            rx: None,
            child: Arc::new(Mutex::new(None)),
            update: false,
            play: false,
            wait_pid: 0,
            kept: String::new(),
            check: Check::default(),
            check_rx: None,
            done_at: None,
            shot: None,
            shot_state: Arc::new(Mutex::new(0)),
            settled_at: None,
        }
    }

    /// The graphics card found, and the preset suggested for it (suggest_quality).
    fn detect_gpu(&mut self) {
        let cards = winapi::graphics_cards();
        let best = cards.into_iter().filter(|(n, _)| !n.to_ascii_lowercase().contains("basic")).max_by_key(|(_, v)| *v);
        if let Some((name, vram)) = best {
            self.hd_quality = suggest_quality(&name, vram);
            // (User decision 2026-11-11) Built-in or low-end graphics (no card with 3.5 GB of its own video memory; the
            // GTX 1650's 4 GB and up are not): no HD textures are made on it - make_hd_textures.ps1 refuses too.
            self.low_end = vram < 3584 * 1024 * 1024 || std::env::var("TFA_LOW_END").is_ok_and(|v| v == "1"); // (TFA_LOW_END=1: tests)
            if self.low_end {
                self.hd = false;
            }
            self.gpu = Some((name, vram));
        }
    }

    /// The HD choices an install made before (hd_textures\hd_options.json), shown again in the form.
    fn load_hd_options(&mut self) {
        let hd = game_folder(&self.repo()).join("hd_textures");
        self.hd_saved = None;
        if let Ok(text) = std::fs::read_to_string(hd.join("hd_options.json")) {
            let t: String = text.chars().filter(|c| !c.is_whitespace()).collect();
            if let Some(q) = HdQuality::ALL.into_iter().find(|q| t.contains(&format!("\"quality\":\"{}\"", q.arg()))) {
                self.hd_quality = q;
                self.hd_saved = Some(q);
            }
            self.hd_while_playing = t.contains("\"while_playing\":true");
            self.hd_gentle = t.contains("\"gentle\":true");
        }
        // (the update screen: the set's size now, beside what each preset makes)
        self.hd_set_gb = dir_size(&hd.join("upscaled")).map(|b| b as f32 / (1u64 << 30) as f32);
    }

    /// (PR #20) The preset chosen on the update screen, saved for the HD step (hd_options.json, as setup.ps1 writes it).
    fn save_hd_quality(&mut self) -> Result<(), String> {
        let hd = game_folder(&self.repo()).join("hd_textures");
        let text = format!("{{\n    \"quality\":  \"{}\",\n    \"while_playing\":  {},\n    \"gentle\":  {}\n}}",
                           self.hd_quality.arg(), self.hd_while_playing, self.hd_gentle);
        std::fs::write(hd.join("hd_options.json"), text).map_err(|e| format!("could not save the HD texture preset: {e}"))?;
        self.hd_saved = Some(self.hd_quality);
        Ok(())
    }

    fn repo(&self) -> PathBuf {
        checkout_in(Path::new(&self.folder), &self.repo_url)
    }

    /// --repo / --mods after new(): the steps follow.
    fn set_flavour(&mut self, repo_url: Option<String>, mods: bool) {
        if let Some(url) = repo_url {
            self.repo_url = url;
        }
        self.mods = self.mods || mods;
        self.steps = make_steps(self.mods);
    }

    fn installed(&self) -> bool {
        self.repo().join(".git").exists() && self.repo().join("update.ps1").exists()
    }

    /// The update check, on a thread of its own.
    fn start_check(&mut self) {
        let (tx, rx) = channel();
        self.check_rx = Some(rx);
        self.check = Check::default();
        let repo = self.repo();
        std::thread::spawn(move || {
            let _ = tx.send(check_updates(&repo));
        });
    }

    fn start_update(&mut self) {
        self.update = true;
        self.update_step_texts();
        self.start();
    }

    /// The steps' texts for an update.
    fn update_step_texts(&mut self) {
        for (i, s) in self.steps.iter_mut().enumerate() {
            s.status = Status::Pending;
            s.detail = match i {
                STEP_TOOLS => "already there".into(),
                STEP_DOWNLOAD => "the new changes, from GitHub".into(),
                STEP_GAME => "rebuilt when the changes need it".into(),
                STEP_HD => "updated when the changes need it".into(),
                _ => "rebuilt when the changes need it (your on / off choices are kept)".into(),
            };
        }
        self.steps[STEP_TOOLS].status = Status::Done;
    }

    fn start(&mut self) {
        let (tx, rx) = channel();
        self.rx = Some(rx);
        self.phase = Phase::Running;
        self.kept.clear();
        self.progress = 0.0;
        self.hd_playable = false;
        if !self.hd && !self.update {
            self.steps[STEP_HD].status = Status::Skipped;
            self.steps[STEP_HD].detail = "skipped: the game shows its original textures".into();
        }
        if self.mods && self.prototype.is_empty() && !self.update {
            self.steps[STEP_MODS].detail = "without the prototype disc: no jet bike".into();
        }
        let job = Job {
            repo_url: self.repo_url.clone(),
            mods: self.mods,
            retail: self.retail.clone(),
            prototype: self.prototype.clone(),
            folder: PathBuf::from(&self.folder),
            hd: self.hd,
            hd_quality: self.hd_quality,
            hd_while_playing: self.hd_while_playing,
            hd_gentle: self.hd_gentle,
            child: self.child.clone(),
            update: self.update,
            play: self.play,
            wait_pid: self.wait_pid,
            source_ref: None,
            managed: false,
        };
        std::thread::spawn(move || {
            let result = job.run(&tx);
            let _ = tx.send(Msg::Finished(result));
        });
    }

    /// (TODO 289) A step the setup before this one left running (its window died - a graphics driver reset): followed
    /// to its end in this window instead of a second update or install started beside it.
    fn follow_left_step(&mut self, left: LeftStep) {
        self.update = left.update;
        self.play = left.play;
        for s in self.steps.iter_mut() {
            s.status = Status::Pending;
        }
        if self.update {
            self.update_step_texts();
        }
        let (tx, rx) = channel();
        self.rx = Some(rx);
        self.phase = Phase::Running;
        self.progress = 0.0;
        let folder = PathBuf::from(&self.folder);
        let repo = self.repo();
        let (update, managed) = (self.update, false);
        let first = if update { STEP_DOWNLOAD } else { STEP_TOOLS };
        let _ = tx.send(Msg::Step(first));
        let _ = tx.send(Msg::Detail(first, "the setup closed while this was running: it went on by itself, followed here".into()));
        std::thread::spawn(move || {
            let pid = left.pid;
            let done = Arc::new(std::sync::atomic::AtomicBool::new(false));
            let code = Arc::new(Mutex::new(None::<i32>));
            let waiter = {
                let (done, code) = (done.clone(), code.clone());
                std::thread::spawn(move || {
                    *code.lock().unwrap() = winapi::wait_for_exit(pid);
                    done.store(true, std::sync::atomic::Ordering::SeqCst);
                })
            };
            // (from a little before the log's end: the last lines shown at once)
            let len = std::fs::metadata(&left.log).map(|m| m.len()).unwrap_or(0);
            let tail = Arc::new(Mutex::new(Vec::<String>::new()));
            follow_log(&left.log, len.saturating_sub(4096), first, &tx, &tail, &|| done.load(std::sync::atomic::Ordering::SeqCst));
            let _ = waiter.join();
            LeftStep::clear(&folder);
            let result = match *code.lock().unwrap() {
                Some(0) => {
                    copy_self(&folder);
                    if !update && !managed {
                        remember_folder(&folder);
                    }
                    let _ = std::fs::remove_file(folder.join(UNFINISHED_MARK));
                    desktop_shortcut_once(&folder, &repo);
                    Ok(())
                }
                _ => Err(format!("{}\n\nThe full log: {}", tail.lock().unwrap().join("\n"), left.log.display())),
            };
            let _ = tx.send(Msg::Finished(result));
        });
    }

    fn cancel(&mut self) {
        let tree = self.child.lock().unwrap().clone();
        if let Some(tree) = tree {
            tree.terminate();
        }
    }

    fn poll(&mut self) {
        if let Some(rx) = &self.check_rx {
            if let Ok(c) = rx.try_recv() {
                self.check = c;
                self.check_rx = None;
            }
        }
        let Some(rx) = &self.rx else { return };
        while let Ok(msg) = rx.try_recv() {
            match msg {
                Msg::Step(i) if i < self.steps.len() => {
                    // (the tools step comes back after the download: setup.ps1 checks them again)
                    for (j, s) in self.steps.iter_mut().enumerate() {
                        if j != i && s.status == Status::Active {
                            s.status = Status::Done;
                        }
                    }
                    if self.steps[i].status != Status::Skipped {
                        self.steps[i].status = Status::Active;
                        self.steps[i].started = Some(Instant::now());
                    }
                }
                Msg::Detail(i, line) if i < self.steps.len() => {
                    // (TODO 219: from the upscale on, the game can be played while the rest is made)
                    if i == STEP_HD && (line.starts_with("Upscaling") || line.starts_with("HD textures:") || line.starts_with("paused while")) {
                        self.hd_playable = true;
                    }
                    if self.steps[i].status != Status::Skipped {
                        self.steps[i].detail = line;
                    }
                }
                Msg::Step(_) | Msg::Detail(..) => {} // (a step this flavour does not show)
                Msg::Progress(p) => self.progress = self.progress.max(p),
                Msg::Kept(branch) => self.kept = branch,
                Msg::Finished(Ok(())) => {
                    for s in &mut self.steps {
                        if s.status == Status::Active || s.status == Status::Pending {
                            s.status = Status::Done;
                        }
                    }
                    self.progress = 1.0;
                    self.phase = Phase::Done(true);
                    self.done_at = Some(Instant::now());
                }
                Msg::Finished(Err(e)) => {
                    for s in &mut self.steps {
                        if s.status == Status::Active {
                            s.status = Status::Failed;
                        }
                    }
                    self.error = e;
                    self.phase = Phase::Done(false);
                }
            }
        }
        for s in &mut self.steps {
            if s.status == Status::Active {
                if let Some(t) = s.started {
                    s.seconds = t.elapsed().as_secs();
                }
            }
        }
    }
}

/// The steps the window shows (the Mods step only with the mods' parts).
fn make_steps(mods: bool) -> Vec<Step> {
    let mut steps = vec![
        ("Tools", "Git, Python and the Visual Studio C++ tools"),
        ("Download", "the game's code, from GitHub"),
        ("Disc and game", "your disc checked, the game compiled (15-25 minutes)"),
        ("HD textures", "every texture upscaled on your graphics card"),
    ];
    if mods {
        steps.push(("Mods", "the jet bike and the other mods, from your discs"));
    }
    steps.into_iter().map(|(title, detail)| Step { title, detail: detail.into(), status: Status::Pending, started: None, seconds: 0 }).collect()
}

/// The install's folder name: the repository's ("https://github.com/x/name(.git)" -> "name").
/// (TODO 279) The checkout in an install folder: source\ (the name planned for new installs) when it is a clone, else the
/// repository's own name (every install so far). Either way the rest is the same.
fn checkout_in(folder: &Path, url: &str) -> PathBuf {
    let source = folder.join("source");
    if source.join(".git").exists() { source } else { folder.join(repo_name(url)) }
}

/// (TODO 279) The install folder's layout number (layout.json: {"layout": 2}), 1 without it.
fn layout_of(folder: &Path) -> u32 {
    let Ok(text) = std::fs::read_to_string(folder.join("layout.json")) else { return 1 };
    let Some(at) = text.find("\"layout\"") else { return 1 };
    let rest = &text[at + 8..];
    let Some(colon) = rest.find(':') else { return 1 };
    rest[colon + 1..].trim_start().chars().take_while(|c| c.is_ascii_digit()).collect::<String>().parse().unwrap_or(1)
}

fn repo_name(url: &str) -> String {
    let last = url.trim_end_matches('/').rsplit(['/', '\\', ':']).next().unwrap_or("transformers");
    let name = last.strip_suffix(".git").unwrap_or(last);
    if name.is_empty() { "transformers".to_string() } else { name.to_string() }
}

/// The game's own folder in an install: the clone itself, or its port\ (a repository holding it).
fn game_folder(repo: &Path) -> PathBuf {
    let port = repo.join("port");
    if port.is_dir() { port } else { repo.to_path_buf() }
}

trait NoWindow {
    fn creation_flags_no_window(&mut self) -> &mut Self;
}
impl NoWindow for Command {
    fn creation_flags_no_window(&mut self) -> &mut Self {
        use std::os::windows::process::CommandExt;
        self.creation_flags(CREATE_NO_WINDOW)
    }
}

// ------------------------------------------------------------------------------------------------
// The work, on its own thread.

struct Job {
    repo_url: String,
    mods: bool,
    retail: String,
    prototype: String,
    folder: PathBuf,
    hd: bool,
    hd_quality: HdQuality,
    hd_while_playing: bool,
    hd_gentle: bool,
    child: Arc<Mutex<Option<Arc<winapi::ProcessTree>>>>, // the running step's process tree (Cancel ends it)
    update: bool,
    play: bool,
    wait_pid: u32,
    source_ref: Option<String>, // RecompLauncher releases build the source embedded by CI.
    managed: bool,             // no standalone shortcuts or remembered install folder
}

impl Job {
    fn log_path(&self) -> PathBuf {
        let name = if self.update { "update-setup.log" } else { "setup.log" };
        // (TODO 279: the install's logs\ once its folders were moved; beside this program before that)
        let logs = self.folder.join("logs");
        if layout_of(&self.folder) == 2 && logs.is_dir() { logs.join(name) } else { self.folder.join(name) }
    }

    /// (TODO 310) The steps' own record beside the log (TF_STEP_LOG, tools\step_log.ps1): each step with its time,
    /// written straight to the disk - it names the step even when the PC went off. Made empty at each start.
    fn step_log_path(&self) -> PathBuf {
        let log = self.log_path();
        log.with_file_name(if self.update { "update_steps.log" } else { "setup_steps.log" })
    }

    fn run(&self, tx: &Sender<Msg>) -> Result<(), String> {
        std::fs::create_dir_all(&self.folder).map_err(|e| format!("cannot make {}: {e}", self.folder.display()))?;
        let log_path = self.log_path();
        // (TODO 289: opened for appending - the steps' programs write their output into it themselves, so a step
        // goes on writing it when this window is gone - a graphics driver reset took a player's setup window, and
        // its update with it)
        let log = Arc::new(Mutex::new(std::fs::File::create(&log_path).ok().and_then(|_| {
            std::fs::OpenOptions::new().append(true).open(&log_path).ok()
        })));
        let mut path_env = std::env::var("PATH").unwrap_or_default();
        // (a run that does not finish - closed half way - leaves this mark: the setup opens again next time,
        // not the game; a player's report. Installs from before the mark have none: finished.)
        let _ = std::fs::write(self.folder.join(UNFINISHED_MARK), b"");
        let _ = std::fs::write(self.step_log_path(), b"");

        if self.update {
            // update.ps1 does it all (and, for the game's own update, starts it again: -Play).
            let repo = checkout_in(&self.folder, &self.repo_url);
            let _ = tx.send(Msg::Step(STEP_DOWNLOAD));
            // (the game that asked for the update closes first: its folder is about to change)
            if self.wait_pid != 0 {
                let _ = tx.send(Msg::Detail(STEP_DOWNLOAD, "waiting for the game to close".into()));
                winapi::wait_for_process(self.wait_pid, 120_000);
            }
            self.keep_local_work(&repo, tx, &log).map_err(|e| format!("{e}\n\nThe full log: {}", log_path.display()))?;
            let script = repo.join("update.ps1").to_string_lossy().to_string();
            let mut args: Vec<String> = vec!["-NoProfile".into(), "-ExecutionPolicy".into(), "RemoteSigned".into(), "-File".into(), script,
                "-StashLocalChanges".into()];
            if self.wait_pid != 0 {
                args.extend(["-WaitForPid".into(), self.wait_pid.to_string()]);
            }
            if self.play {
                args.push("-Play".into());
            }
            let refs: Vec<&str> = args.iter().map(String::as_str).collect();
            self.exec("powershell", &refs, Some(&repo), &path_env, tx, STEP_DOWNLOAD, &log)
                .map_err(|e| format!("{e}\n\nThe full log: {}", log_path.display()))?;
            copy_self(&self.folder);
            let _ = std::fs::remove_file(self.folder.join(UNFINISHED_MARK));
            // (installs from before the shortcut get it with their next update, once)
            desktop_shortcut_once(&self.folder, &repo);
            return Ok(());
        }

        // Git first: the repository comes with it.
        let _ = tx.send(Msg::Step(STEP_TOOLS));
        // (a Git that is installed but not on PATH - its "Git Bash only" option, a per-user install - is used where it is)
        if !has(&path_env, "git") {
            path_env = with_git_dir(&path_env);
        }
        if !has(&path_env, "git") {
            let _ = tx.send(Msg::Detail(STEP_TOOLS, "installing Git (winget)".into()));
            // (winget's exit code does not decide: "already installed, no newer version" is an error
            // code too - a player's setup stopped on it. The check after it decides.)
            let r = self.exec("winget", &["install", "--id", "Git.Git", "-e", "--source", "winget", "--accept-package-agreements", "--accept-source-agreements",
                "--disable-interactivity"], None, &path_env, tx, STEP_TOOLS, &log);
            path_env = with_git_dir(&fresh_path(&path_env));
            if !has(&path_env, "git") {
                return Err(match r {
                    Err(e) => format!("{e}\n\n{}", git_failure_text(&e)),
                    Ok(()) => "Git was installed but is not found: restart the setup".into(),
                });
            }
        }
        let _ = tx.send(Msg::Progress(0.03));

        let _ = tx.send(Msg::Step(STEP_DOWNLOAD));
        let repo = checkout_in(&self.folder, &self.repo_url);
        if self.managed {
            // Settings may already exist here: initialize around them rather than requiring an
            // empty clone destination. Checkout refuses conflicting files instead of deleting them.
            let r = repo.to_string_lossy().to_string();
            if !repo.join(".git").exists() {
                std::fs::create_dir_all(&repo).map_err(|e| e.to_string())?;
                self.exec("git", &["init", &r], None, &path_env, tx, STEP_DOWNLOAD, &log)?;
                self.exec("git", &["-C", &r, "remote", "add", "origin", &self.repo_url], None, &path_env, tx, STEP_DOWNLOAD, &log)?;
            }
            let revision = self.source_ref.as_deref().unwrap_or("HEAD");
            self.exec("git", &["-C", &r, "fetch", "--depth", "1", &self.repo_url, revision], None, &path_env, tx, STEP_DOWNLOAD, &log)?;
            self.exec("git", &["-C", &r, "checkout", "--detach", "FETCH_HEAD"], None, &path_env, tx, STEP_DOWNLOAD, &log)?;
            self.exec("git", &["-C", &r, "submodule", "update", "--init", "--recursive"], None, &path_env, tx, STEP_DOWNLOAD, &log)?;
        } else if repo.join(".git").exists() {
            let _ = tx.send(Msg::Detail(STEP_DOWNLOAD, "already there: the newest changes pulled".into()));
            let r = repo.to_string_lossy().to_string();
            // Edits in the install folder (a player's, or an AI helper's fix) would stop the pull:
            // they are committed to a branch of their own first - kept, never deleted.
            self.keep_local_work(&repo, tx, &log)?;
            self.exec("git", &["-C", &r, "pull", "--ff-only"], None, &path_env, tx, STEP_DOWNLOAD, &log)?;
            self.exec("git", &["-C", &r, "submodule", "update", "--init", "--recursive"], None, &path_env, tx, STEP_DOWNLOAD, &log)?;
        } else {
            let r = repo.to_string_lossy().to_string();
            self.exec("git", &["clone", "--recursive", &self.repo_url, &r], None, &path_env, tx, STEP_DOWNLOAD, &log)
                .map_err(|e| format!("{e}\n\nThe repository could not be downloaded from {} (no network? the address right?).", self.repo_url))?;
        }
        let _ = tx.send(Msg::Progress(0.06));

        // setup.ps1 does the rest; a second run after it installed missing tools (they need a fresh PATH).
        let script = repo.join("setup.ps1").to_string_lossy().to_string();
        let mut args: Vec<String> = vec!["-NoProfile".into(), "-ExecutionPolicy".into(), "RemoteSigned".into(), "-File".into(), script,
            "-Iso".into(), self.retail.clone(), "-InstallPrerequisites".into()];
        if self.mods && !self.prototype.is_empty() {
            args.extend(["-PrototypeIso".into(), self.prototype.clone()]);
        }
        if !self.hd {
            args.push("-SkipHdTextures".into());
        } else {
            args.extend(["-HdQuality".into(), self.hd_quality.arg().into()]);
            args.extend(["-HdWhilePlaying".into(), if self.hd_while_playing { "on" } else { "off" }.into()]);
        }
        // (TODO 310) "Go easy on my PC": the build too, so also without the HD textures (setup.ps1 keeps it in hd_options.json)
        args.extend(["-HdGentle".into(), if self.hd_gentle { "on" } else { "off" }.into()]);
        let refs: Vec<&str> = args.iter().map(String::as_str).collect();
        let _ = tx.send(Msg::Step(STEP_TOOLS));
        for attempt in 0..2 {
            match self.exec("powershell", &refs, Some(&repo), &path_env, tx, STEP_TOOLS, &log) {
                Ok(()) => {
                    copy_self(&self.folder);
                    if !self.managed { remember_folder(&self.folder); }
                    let _ = std::fs::remove_file(self.folder.join(UNFINISHED_MARK));
                    if !self.managed { desktop_shortcut_once(&self.folder, &repo); }
                    return Ok(());
                }
                Err(e) if attempt == 0 && e.contains("prerequisites installed") => {
                    path_env = fresh_path(&path_env);
                    let _ = tx.send(Msg::Detail(STEP_TOOLS, "tools installed: carrying on".into()));
                }
                Err(e) => return Err(format!("{e}\n\nThe full log: {}", log_path.display())),
            }
        }
        Err("the tools were installed but are still not found: restart the setup".into())
    }

    /// The player's own work in the install (local commits, changed files, new files that are not
    /// ignored) would stop the update: all of it is committed to a branch of its own,
    /// my-changes-<date>-<time>, and the install's branch goes back to where it and the repository
    /// part (the update then moves it on as always). Nothing that is not on that branch first is
    /// touched; ignored files (game data, builds, settings, saves, Mods\) never are. Ok(None):
    /// nothing to keep. Err: the backup could not be made - nothing was changed, the update stops.
    fn keep_local_work(&self, repo: &Path, tx: &Sender<Msg>, log: &Arc<Mutex<Option<std::fs::File>>>) -> Result<Option<String>, String> {
        let say = |text: &str| {
            if let Some(f) = log.lock().unwrap().as_mut() {
                use std::io::Write;
                let _ = writeln!(f, "{text}");
            }
        };
        let kept = keep_local_work(repo, &say)?;
        if let Some(branch) = &kept {
            let line = kept_text(branch);
            say(&line);
            let _ = tx.send(Msg::Detail(STEP_DOWNLOAD, line));
            let _ = tx.send(Msg::Kept(branch.clone()));
            // (the game says so too at its next start: updates.cpp reads and deletes this file)
            let states = game_folder(repo).join("states");
            let _ = std::fs::create_dir_all(&states);
            let _ = std::fs::write(states.join("update_kept.txt"), format!("{branch}\n"));
        }
        Ok(kept)
    }

    /// Runs a program, its output into the log and turned into steps/details. Err = its last lines.
    fn exec(&self, exe: &str, args: &[&str], dir: Option<&Path>, path_env: &str, tx: &Sender<Msg>, step: usize,
            log: &Arc<Mutex<Option<std::fs::File>>>) -> Result<(), String> {
        let mut cmd = Command::new(program(exe));
        let path_env = with_system_dirs(path_env);
        // (PSModulePath dropped: started from PowerShell 7, Windows PowerShell would inherit its
        // module folders and miss its own commands - Get-FileHash, found in the first test)
        // (long paths for git and the build's own downloads: 260 characters is not enough in a
        // nested install folder - toml11's test files, found in the first install test)
        cmd.env("GIT_CONFIG_COUNT", "2").env("GIT_CONFIG_KEY_0", "core.longpaths").env("GIT_CONFIG_VALUE_0", "true");
        // (an install on a drive that records no file owners - exFAT, FAT32: git calls it "dubious ownership" and
        // stops; a player's report. Safe for these git commands only, nothing global is changed.)
        cmd.env("GIT_CONFIG_KEY_1", "safe.directory").env("GIT_CONFIG_VALUE_1", "*");
        // (update.ps1 on a failure: no 30 s pause in a window nobody sees)
        cmd.env("TFA_UPDATE_NO_PAUSE", "1");
        // (TODO 310) each step of setup.ps1 / update.ps1 / build.ps1 / make_hd_textures.ps1 with its time, to the disk
        cmd.env("TF_STEP_LOG", self.step_log_path());
        if self.managed { cmd.env("RECOMPLAUNCHER", "1"); }
        // (TODO 289) Its output goes straight into the log file, which this window follows - not through pipes to this
        // window: a player's graphics driver reset (a GTX 1660 SUPER, during the HD textures) took the setup's window
        // down, and with its pipes gone and its job object closed the whole update died with it. Now the step carries
        // on by itself, and the setup started again follows it (see `follow_left_step`).
        let log_path = self.log_path();
        let out_file = log.lock().unwrap().as_ref().and_then(|f| f.try_clone().ok());
        let (out_file, err_file) = match out_file.and_then(|f| f.try_clone().ok().map(|g| (f, g))) {
            Some(pair) => pair,
            None => return Err(format!("the log {} could not be written", log_path.display())),
        };
        let start_at = std::fs::metadata(&log_path).map(|m| m.len()).unwrap_or(0);
        cmd.args(args).env("PATH", &path_env).env_remove("PSModulePath").stdout(Stdio::from(out_file)).stderr(Stdio::from(err_file))
            .stdin(Stdio::null()).creation_flags_no_window();
        if let Some(d) = dir {
            cmd.current_dir(d);
        }
        winapi::allow_foreground();
        let mut child = cmd.spawn().map_err(|e| format!("{exe} could not be started: {e}"))?;
        // (its whole process tree in a job object: Cancel ends all of it, and so does closing this window (on_exit).
        // Not ended when this program dies (no kill-on-close): an update then finishes on its own. When the step is
        // done what it left is ended - but not after an update that starts the game again at its end)
        let tree = Arc::new(winapi::ProcessTree::new(&child, false));
        *self.child.lock().unwrap() = Some(tree.clone());
        let running = LeftStep { pid: child.id(), started: winapi::process_started(child.id()).unwrap_or(0), log: log_path.clone(),
                                 update: self.update, play: self.play };
        running.write(&self.folder);
        let tail = Arc::new(Mutex::new(Vec::<String>::new()));
        let done = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let follower = {
            let (tx, tail, done, log_path) = (tx.clone(), tail.clone(), done.clone(), log_path.clone());
            std::thread::spawn(move || follow_log(&log_path, start_at, step, &tx, &tail, &|| done.load(std::sync::atomic::Ordering::SeqCst)))
        };
        let status = child.wait().map_err(|e| e.to_string());
        done.store(true, std::sync::atomic::Ordering::SeqCst);
        let _ = follower.join();
        LeftStep::clear(&self.folder);
        *self.child.lock().unwrap() = None;
        if !self.play {
            tree.terminate(); // (what the step left running)
        }
        drop(tree);
        if status?.success() {
            Ok(())
        } else {
            Err(tail.lock().unwrap().join("\n"))
        }
    }
}

/// (TODO 289) The step that is running, written beside the log while it runs: a setup started again after its window
/// died (a graphics driver reset) finds the step still going and follows it instead of starting a second one.
struct LeftStep {
    pid: u32,
    started: u64, // (its start time: with the id it names the process - ids are given out again)
    log: PathBuf,
    update: bool,
    play: bool,
}

const LEFT_STEP_FILE: &str = "setup-running.txt";

impl LeftStep {
    fn write(&self, folder: &Path) {
        let _ = std::fs::write(folder.join(LEFT_STEP_FILE),
                               format!("{}\n{}\n{}\n{}\n{}\n", self.pid, self.started, u8::from(self.update), u8::from(self.play),
                                       self.log.display()));
    }

    fn clear(folder: &Path) {
        let _ = std::fs::remove_file(folder.join(LEFT_STEP_FILE));
    }

    /// The step a setup before this one left running: Some only while that very process still runs.
    fn find(folder: &Path) -> Option<LeftStep> {
        let text = std::fs::read_to_string(folder.join(LEFT_STEP_FILE)).ok()?;
        let mut lines = text.lines();
        let pid: u32 = lines.next()?.trim().parse().ok()?;
        let started: u64 = lines.next()?.trim().parse().ok()?;
        let update = lines.next()?.trim() == "1";
        let play = lines.next()?.trim() == "1";
        let log = PathBuf::from(lines.next()?.trim());
        match winapi::process_started(pid) {
            Some(t) if started == 0 || t == started => Some(LeftStep { pid, started, log, update, play }),
            _ => {
                LeftStep::clear(folder); // (gone: nothing to follow)
                None
            }
        }
    }
}

/// Follows a step's log from `start_at`: each new line into the log's tail, the steps and the details shown, until
/// `ended()` says the step has ended (then what is left is read too).
fn follow_log(log_path: &Path, start_at: u64, step: usize, tx: &Sender<Msg>, tail: &Arc<Mutex<Vec<String>>>, ended: &dyn Fn() -> bool) {
    use std::io::{Read, Seek, SeekFrom};
    let mut current = step;
    let mut file = None;
    let mut pending: Vec<u8> = Vec::new();
    let mut buf = vec![0u8; 16384];
    let (mut quiet_since, mut hinted) = (Instant::now(), false);
    loop {
        let finished = ended(); // (asked before the read: what was written before the end is still read)
        if file.is_none() {
            file = std::fs::File::open(log_path).ok().and_then(|mut f| f.seek(SeekFrom::Start(start_at)).ok().map(|_| f));
        }
        let mut got = false;
        if let Some(f) = file.as_mut() {
            while let Ok(n) = f.read(&mut buf) {
                if n == 0 {
                    break;
                }
                got = true;
                pending.extend_from_slice(&buf[..n]);
                while let Some(at) = pending.iter().position(|&b| b == b'\n') {
                    let line: Vec<u8> = pending.drain(..=at).collect();
                    show_line(&String::from_utf8_lossy(&line), &mut current, tx, tail);
                }
            }
        }
        if finished && !got {
            if !pending.is_empty() {
                show_line(&String::from_utf8_lossy(&pending), &mut current, tx, tail);
            }
            return;
        }
        if got {
            quiet_since = Instant::now();
            hinted = false;
        } else {
            // (TODO 290) A step quiet for a while after an installer started: Windows' permission prompt is most
            // likely waiting, perhaps only as a flashing button in the taskbar.
            // (the last few lines: winget ends with "The installer will request to run as administrator. Expect a
            // prompt." after "Starting package install..." - on the ThinkPad test only the last line was looked at and
            // the hint never came, while Windows' question sat unanswered until it closed itself)
            let recent = tail.lock().unwrap().iter().rev().take(3).map(|l| l.to_ascii_lowercase()).collect::<Vec<_>>().join("\n");
            if !hinted && quiet_since.elapsed().as_secs() >= 20 && waiting_for_permission(&recent) {
                hinted = true;
                let _ = tx.send(Msg::Detail(current, "Windows may be asking for permission to install: if a shield button flashes in the taskbar, click it and choose Yes".into()));
            }
            std::thread::sleep(std::time::Duration::from_millis(150));
        }
    }
}

/// One line of a step's output: the steps and details it marks, and the tail kept for an error.
fn show_line(line: &str, current: &mut usize, tx: &Sender<Msg>, tail: &Arc<Mutex<Vec<String>>>) {
    let text = line.trim().to_string();
    if text.is_empty() {
        return;
    }
    if let Some(title) = text.strip_prefix("==> ") {
        let (s, p) = classify(title, *current);
        if s != *current {
            *current = s;
            let _ = tx.send(Msg::Step(s));
        }
        if let Some(p) = p {
            let _ = tx.send(Msg::Progress(p));
        }
        let _ = tx.send(Msg::Detail(s, title.to_string()));
    } else if !text.starts_with("--") && !text.contains("warning") && !text.contains("RemoteException") {
        if *current == STEP_GAME {
            if let Some(p) = compile_progress(&text) {
                let _ = tx.send(Msg::Progress(p));
            }
        }
        if *current == STEP_HD {
            if let Some(p) = hd_progress(&text) {
                let _ = tx.send(Msg::Progress(p));
            }
        }
        let short: String = text.chars().take(110).collect();
        let _ = tx.send(Msg::Detail(*current, short));
    }
    let mut t = tail.lock().unwrap();
    t.push(text);
    if t.len() > 12 {
        t.remove(0);
    }
}

/// A "==>" line of setup.ps1 / build.ps1 / make_hd_textures.ps1: which step it belongs to, and the
/// overall progress it marks.
fn classify(title: &str, current: usize) -> (usize, Option<f32>) {
    let t = title.to_ascii_lowercase();
    // update.ps1's steps
    let update = [("waiting for the game to close", STEP_DOWNLOAD, 0.03), ("checking the repository", STEP_DOWNLOAD, 0.05),
                  ("pulling", STEP_DOWNLOAD, 0.08), ("rebuilding the game", STEP_GAME, 0.10), ("installing the prebuilt game", STEP_GAME, 0.10),
                  ("updating the hd textures", STEP_HD, 0.85), ("rebuilding the mods", STEP_MODS, 0.92)];
    for (k, s, p) in update {
        if t.starts_with(k) {
            return (s, Some(p));
        }
    }
    if t.starts_with("starting the game") {
        return (current, Some(0.99));
    }
    if t.starts_with("checking what the build needs") || t.starts_with("installing") {
        return (STEP_TOOLS, Some(0.07));
    }
    if t.starts_with("building the game") || t.starts_with("fetching the ps2recomp") {
        return (STEP_GAME, Some(0.10));
    }
    if t.starts_with("making the hd textures") {
        return (STEP_HD, Some(0.62));
    }
    if t.starts_with("making the mods") {
        return (STEP_MODS, Some(0.92));
    }
    // build.ps1's own steps, in its order
    let game = [("extracting", 0.13), ("building recompiler", 0.17), ("scanning", 0.20), ("recovering", 0.24),
                ("translating", 0.28), ("updating generated", 0.33), ("applying the ps2recomp", 0.15),
                ("configuring", 0.36), ("building transformersrecomp", 0.40)];
    if current == STEP_GAME {
        for (k, p) in game {
            if t.starts_with(k) {
                return (STEP_GAME, Some(p));
            }
        }
    }
    let hd = [("extracting the disc", 0.64), ("converting", 0.67), ("downloading real-esrgan", 0.70), ("upscaling", 0.72)];
    if current == STEP_HD {
        for (k, p) in hd {
            if t.starts_with(k) {
                return (STEP_HD, Some(p));
            }
        }
    }
    (current, None)
}

/// build.ps1's "compiling the game: 120 of about 329 files, about 3 min left" (TODO 115): the bar
/// moves through the compile, from where "building transformersrecomp" put it (0.40) to 0.60.
fn compile_progress(text: &str) -> Option<f32> {
    let rest = text.strip_prefix("compiling the game: ")?;
    let mut words = rest.split_whitespace();
    let done: f32 = words.next()?.parse().ok()?;
    if words.next()? != "of" || words.next()? != "about" {
        return None;
    }
    let total: f32 = words.next()?.parse().ok()?;
    if total <= 0.0 {
        return None;
    }
    Some(0.40 + 0.20 * (done / total).clamp(0.0, 1.0))
}

/// The mark a setup run keeps in the install folder until it has finished every step.
const UNFINISHED_MARK: &str = ".setup_unfinished";

/// (TODO 310) The last line of the newer steps record (setup_steps.log / update_steps.log, beside the setup's log or in
/// the install's logs\): the step a setup or an update was at when it stopped.
fn last_step(folder: &Path) -> Option<String> {
    let mut newest: Option<(std::time::SystemTime, PathBuf)> = None;
    for dir in [folder.to_path_buf(), folder.join("logs")] {
        for name in ["setup_steps.log", "update_steps.log"] {
            let p = dir.join(name);
            if let Ok(t) = std::fs::metadata(&p).and_then(|m| m.modified()) {
                if newest.as_ref().map_or(true, |(n, _)| t > *n) {
                    newest = Some((t, p));
                }
            }
        }
    }
    let text = std::fs::read_to_string(newest?.1).ok()?;
    let line = text.lines().rev().find(|l| !l.trim().is_empty())?.trim().to_string();
    Some(line.chars().take(140).collect())
}

/// make_hd_textures.ps1's "HD textures: 640 of 1900 made, about 25 min left" (TODO 219): the bar moves
/// through the upscale, from where "upscaling" put it (0.72) to 0.97.
fn hd_progress(text: &str) -> Option<f32> {
    let rest = text.strip_prefix("HD textures: ")?;
    let mut words = rest.split_whitespace();
    let done: f32 = words.next()?.parse().ok()?;
    if words.next()? != "of" {
        return None;
    }
    let total: f32 = words.next()?.trim_end_matches(',').parse().ok()?;
    if total <= 0.0 {
        return None;
    }
    Some(0.72 + 0.25 * (done / total).clamp(0.0, 1.0))
}

/// The HD textures presets (PR #20): tools/hd_textures/tier_rules.py PRESETS and ESTIMATES, measured in the home
/// repository's docs/HD_UPSCALE_RATIOS.md. Keep the numbers here in step with ESTIMATES.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum HdQuality {
    Fast,
    Balanced,
    Best,
}

impl HdQuality {
    const ALL: [HdQuality; 3] = [HdQuality::Fast, HdQuality::Balanced, HdQuality::Best];

    /// setup.ps1's -HdQuality and hd_options.json's "quality".
    fn arg(self) -> &'static str {
        match self {
            HdQuality::Fast => "fast",
            HdQuality::Balanced => "balanced",
            HdQuality::Best => "best",
        }
    }

    fn label(self) -> &'static str {
        match self {
            HdQuality::Fast => "Fast",
            HdQuality::Balanced => "Balanced",
            HdQuality::Best => "Best",
        }
    }

    /// The HD set in the renderer's form (BC7 with mipmaps), GB.
    fn gigabytes(self) -> f32 {
        match self {
            HdQuality::Fast => 0.6,
            HdQuality::Balanced => 1.4,
            HdQuality::Best => 2.3,
        }
    }

    /// Making time measured on an RTX 3070, minutes.
    fn minutes_rtx3070(self) -> f32 {
        match self {
            HdQuality::Fast => 10.0,
            HdQuality::Balanced => 25.0,
            HdQuality::Best => 33.0,
        }
    }

    fn hover(self) -> &'static str {
        match self {
            HdQuality::Fast => "For the Steam Deck and 1080p: characters 8x, the rest 4x (menus 2x). One upscale pass for all but the characters: the quickest and smallest",
            HdQuality::Balanced => "For 1440p: your characters and the levels 8x (two upscale passes); enemies, Mini-Cons and effects 4x; menus 2x",
            HdQuality::Best => "For 4K: your characters 16x; enemies, Mini-Cons, levels and effects 8x; menus 4x",
        }
    }

    /// "about 35 min, 1.9 GB" for the card found: one of the stronger cards about as quick as the RTX 3070 it was
    /// measured on, another (or none found) about 1.5-2 times slower.
    fn estimate(self, gpu: Option<&(String, u64)>) -> String {
        let slow = gpu.is_none_or(|(n, v)| suggest_quality(n, *v) == HdQuality::Fast);
        let m = self.minutes_rtx3070() * if slow { 1.75 } else { 1.0 };
        let m = ((m / 5.0).round() * 5.0).max(5.0) as u32;
        format!("about {m} min, {:.1} GB", self.gigabytes())
    }
}

/// The bytes of the files in a folder and its subfolders (None: no such folder).
fn dir_size(dir: &Path) -> Option<u64> {
    let mut total = 0u64;
    let mut stack = vec![std::fs::read_dir(dir).ok()?];
    while let Some(entries) = stack.pop() {
        for e in entries.flatten() {
            match e.file_type() {
                Ok(t) if t.is_dir() => {
                    if let Ok(sub) = std::fs::read_dir(e.path()) {
                        stack.push(sub);
                    }
                }
                Ok(_) => total += e.metadata().map(|m| m.len()).unwrap_or(0),
                Err(_) => {}
            }
        }
    }
    Some(total)
}

/// (PR #20) The HD step in its own small window (make_hd_textures.ps1 -Resume, as the game starts it for an unfinished
/// set): it waits while the game runs and carries on when it closes.
fn start_hd_maker(repo: &Path) -> Result<(), String> {
    use std::os::windows::process::CommandExt;
    let game = game_folder(repo);
    let script = game.join("tools").join("make_hd_textures.ps1");
    if !script.is_file() {
        return Err(format!("{} is missing", script.display()));
    }
    const CREATE_NEW_CONSOLE: u32 = 0x10;
    Command::new(program("powershell"))
        .args(["-NoProfile", "-ExecutionPolicy", "RemoteSigned", "-File"])
        .arg(&script)
        .arg("-Resume")
        .current_dir(&game)
        .creation_flags(CREATE_NEW_CONSOLE)
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("could not start the HD textures window: {e}"))
}

/// The preset suggested for a graphics card: Fast for the older and smaller ones (suggest_fast), the Steam Deck and a
/// card it does not know; Best for the strongest (RTX 4080 / 5080 and up, Radeon RX 7900, RX 9070 XT, with 16 GB);
/// Balanced between.
fn suggest_quality(name: &str, vram: u64) -> HdQuality {
    if suggest_fast(name, vram) {
        return HdQuality::Fast;
    }
    let n = name.to_ascii_lowercase();
    let gb = vram as f64 / (1u64 << 30) as f64;
    let model = |after: &str| -> Option<u32> {
        let at = n.find(after)? + after.len();
        let d: String = n[at..].trim_start().chars().take_while(|c| c.is_ascii_digit()).collect();
        d.parse().ok()
    };
    let strong = match (model("rtx"), model("rx")) {
        (Some(m), _) => m >= 4000 && m % 1000 >= 80,
        (None, Some(m)) if m >= 9000 => n.contains(" xt"),
        (None, Some(m)) => m >= 7000 && m % 1000 >= 900,
        _ => false,
    };
    if strong && (vram == 0 || gb >= 15.5) { HdQuality::Best } else { HdQuality::Balanced }
}

/// Fast suggested for a graphics card on which the two-pass presets would take a long time (TODO 219: the times
/// measured are in docs/TODO.md, item 219). Fast for the older and smaller cards, and for a card it does not know.
fn suggest_fast(name: &str, vram: u64) -> bool {
    let n = name.to_ascii_lowercase();
    let gb = vram as f64 / (1u64 << 30) as f64;
    if vram > 0 && gb < 5.5 {
        return true;
    }
    let model = |after: &str| -> Option<u32> {
        let at = n.find(after)? + after.len();
        let d: String = n[at..].trim_start().chars().take_while(|c| c.is_ascii_digit()).collect();
        d.parse().ok()
    };
    if n.contains("nvidia") || n.contains("geforce") {
        // RTX 30, 40 and 50 series: the 60 cards and up; RTX 20 series: the 70 and up; GTX and older: Fast.
        return match model("rtx") {
            Some(m) if m >= 3000 => m % 1000 < 60,
            Some(m) if m >= 2000 => m % 1000 < 70,
            _ => true,
        };
    }
    if n.contains("amd") || n.contains("radeon") {
        // Radeon RX 7000 and 9000: Best; RX 6000: the 6700 and up.
        return match model("rx") {
            Some(m) if m >= 7000 => false,
            Some(m) if m >= 6000 => m % 1000 < 700,
            _ => true,
        };
    }
    if n.contains("arc") {
        return !(n.contains("a770") || n.contains("a750") || n.contains(" b5") || n.contains(" b7"));
    }
    true
}

/// Where the install finished last, so a setup started from somewhere else (a launcher's own copy, Quiver) finds it.
fn remembered_folder_file() -> Option<PathBuf> {
    std::env::var("LOCALAPPDATA").ok().map(|v| Path::new(&v).join("TransformersArmadaSetup").join("install_folder.txt"))
}

fn remember_folder(folder: &Path) {
    if let Some(f) = remembered_folder_file() {
        if let Some(dir) = f.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        let _ = std::fs::write(f, folder.to_string_lossy().as_bytes());
    }
}

fn remembered_folder() -> Option<String> {
    let text = std::fs::read_to_string(remembered_folder_file()?).ok()?;
    let t = text.trim();
    (!t.is_empty()).then(|| t.to_string())
}

/// A "Transformers Armada" shortcut on the desktop, made once after the first finished install (deleted by the
/// player, it stays deleted). It starts the setup's copy in the install folder, which starts the game when the
/// install is finished and carries on the setup when it is not; its icon is the game's own.
fn desktop_shortcut_once(folder: &Path, repo: &Path) {
    grant_player(folder);
    shortcut_once(folder, repo, "Desktop", "desktop_shortcut_made");
    // (TODO 276) The same in the Start menu's programs: Steam's "Add a Non-Steam Game" lists the Start menu's
    // programs, so the game is there to tick - no browsing for a file deep in the install folder.
    shortcut_once(folder, repo, "Programs", "start_menu_shortcut_made");
}

/// (TODO 290) A setup run as administrator with another account's password: the install's files are made by that
/// account. The signed-in player (who owns Windows' desktop - explorer.exe) is given the right to change them, so the
/// game's settings, saves, HD textures and updates work when he plays without being administrator. Nothing happens
/// when the setup is not elevated, or elevated as the player himself (the folder's own rights already cover him).
fn grant_player(folder: &Path) {
    let script = "if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { exit 0 }; $o = Get-CimInstance Win32_Process -Filter \"name='explorer.exe'\" | Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner; if (-not $o.User) { exit 0 }; $u = \"$($o.Domain)\\$($o.User)\"; if ($u -ieq [Security.Principal.WindowsIdentity]::GetCurrent().Name) { exit 0 }; icacls $env:TFA_DIR /grant \"${u}:(OI)(CI)M\" /Q | Out-Null";
    let _ = Command::new(program("powershell"))
        .args(["-NoProfile", "-NonInteractive", "-Command", script])
        .env("TFA_DIR", folder)
        .env_remove("PSModulePath")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .creation_flags_no_window()
        .status();
}

/// The shortcut in one of Windows' special folders (`Desktop`, `Programs`: the Start menu's), once (its flag file
/// beside the remembered folder's).
fn shortcut_once(folder: &Path, repo: &Path, place: &str, flag_name: &str) {
    let Some(flag) = remembered_folder_file().and_then(|f| f.parent().map(|d| d.join(flag_name))) else { return };
    if flag.exists() {
        return;
    }
    // (TODO 290) A setup run as administrator: the shortcut where every user of the PC sees it (the Public desktop,
    // the all-users Start menu) - run elevated by another account's password, "the user's own" desktop is that
    // account's, not the player's, and the player found no shortcut.
    let script = "$p=$env:TFA_PLACE; if (([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { $p=@{Desktop='CommonDesktopDirectory'; Programs='CommonPrograms'}[$p] }; $d=[Environment]::GetFolderPath($p); $s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $d 'Transformers Armada.lnk'));                   $s.TargetPath=$env:TFA_TARGET; $s.WorkingDirectory=$env:TFA_DIR; $s.IconLocation=$env:TFA_ICON+',0';                   $s.Description='Transformers Armada (PC)'; $s.Save()";
    let ok = Command::new(program("powershell"))
        .args(["-NoProfile", "-NonInteractive", "-Command", script])
        .env("TFA_PLACE", place)
        .env("TFA_TARGET", folder.join("tfa-setup.exe"))
        .env("TFA_DIR", folder)
        .env("TFA_ICON", game_exe(repo))
        .env_remove("PSModulePath")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .creation_flags_no_window()
        .status()
        .is_ok_and(|s| s.success());
    if ok {
        let _ = std::fs::write(flag, b"");
    }
}

/// The built game's exe in an install.
fn game_exe(repo: &Path) -> PathBuf {
    game_folder(repo).join("out").join("build").join("transformers").join("RelWithDebInfo").join("TransformersRecomp.exe")
}

/// A copy of this program in the install folder (tfa-setup.exe), for the game's own "Update now".
fn copy_self(folder: &Path) {
    let Ok(me) = std::env::current_exe() else { return };
    let target = folder.join("tfa-setup.exe");
    let same = std::fs::canonicalize(&me).ok().zip(std::fs::canonicalize(&target).ok()).is_some_and(|(a, b)| a == b);
    if !same {
        let _ = std::fs::copy(&me, &target);
    }
}

/// git in the install, hidden, never asking for anything (a sign-in would wait for nobody).
fn git_out(repo: &Path, args: &[&str]) -> Result<String, String> {
    git_out_env(repo, args, &[])
}

/// git_out with variables of its own (GIT_INDEX_FILE: keep_local_work).
fn git_out_env(repo: &Path, args: &[&str], vars: &[(&str, &std::ffi::OsStr)]) -> Result<String, String> {
    let out = Command::new("git")
        .envs(vars.iter().copied())
        .arg("-C")
        .arg(repo)
        .args(args)
        .env("GIT_TERMINAL_PROMPT", "0")
        .env("GCM_INTERACTIVE", "never")
        .stdin(Stdio::null())
        .creation_flags_no_window()
        .output()
        .map_err(|e| format!("Git could not be started: {e}"))?;
    if out.status.success() {
        Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
    } else {
        let err = String::from_utf8_lossy(&out.stderr);
        Err(err.trim().lines().last().unwrap_or("git failed").to_string())
    }
}

/// The commit the install was last built at (.setup.json's SetUpAt), else its HEAD.
fn built_commit(repo: &Path) -> Option<String> {
    let text = std::fs::read_to_string(repo.join(".setup.json")).unwrap_or_default();
    if let Some(at) = text.find("\"SetUpAt\"") {
        let rest = &text[at + 9..];
        if let Some(q1) = rest.find('"') {
            if let Some(len) = rest[q1 + 1..].find('"') {
                let v = rest[q1 + 1..q1 + 1 + len].trim().to_string();
                if !v.is_empty() {
                    return Some(v);
                }
            }
        }
    }
    git_out(repo, &["rev-parse", "HEAD"]).ok()
}

/// "0.1.3" and the like: three numbers with dots.
fn is_version_name(v: &str) -> bool {
    let parts: Vec<&str> = v.split('.').collect();
    parts.len() == 3 && parts.iter().all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()))
}

/// "0.1.3": the number a commit's VERSION file holds (TODO 111), None before the file existed.
fn version_at(repo: &Path, commit: &str) -> Option<String> {
    let prefix = if repo.join("port").is_dir() { "port/" } else { "" };
    let v = git_out(repo, &["show", &format!("{commit}:{prefix}VERSION")]).ok()?;
    let v = v.trim();
    is_version_name(v).then(|| v.to_string())
}

/// The version bump's own commit ("version 0.1.3", sync_home.ps1): bookkeeping, not a change.
fn is_version_bump(subject: &str) -> bool {
    subject.strip_prefix("version ").is_some_and(is_version_name)
}

impl Check {
    /// "v0.1.0 (2026-11-01)", or "an earlier build (a5cb9f5, 2026-10-31)" from before the VERSION file.
    fn current_text(&self) -> String {
        if self.current_name.is_empty() {
            let date = if self.date.is_empty() { String::new() } else { format!(", {}", self.date) };
            format!("an earlier build ({}{date})", self.current)
        } else if self.date.is_empty() {
            format!("v{}", self.current_name)
        } else {
            format!("v{} ({})", self.current_name, self.date)
        }
    }

    /// "v0.1.3", or the commit's short code when it has no number.
    fn latest_text(&self) -> String {
        if self.latest_name.is_empty() { self.latest.clone() } else { format!("v{}", self.latest_name) }
    }
}

/// What the player reads after their own changes were kept (the setup's screen, the logs).
fn kept_text(branch: &str) -> String {
    format!("Your own changes were kept on the branch {branch}; nothing was lost.")
}

#[repr(C)]
#[derive(Default)]
struct LocalTime {
    year: u16,
    month: u16,
    day_of_week: u16,
    day: u16,
    hour: u16,
    minute: u16,
    second: u16,
    millis: u16,
}

unsafe extern "system" {
    fn GetLocalTime(time: *mut LocalTime);
}

/// The install's own work kept on a branch (Job::keep_local_work; the steps are there).
fn keep_local_work(repo: &Path, say: &dyn Fn(&str)) -> Result<Option<String>, String> {
    let git = |args: &[&str]| {
        let mut all = vec!["-c", "core.longpaths=true"];
        all.extend_from_slice(args);
        git_out(repo, &all)
    };
    let branch = match git(&["rev-parse", "--abbrev-ref", "HEAD"]) {
        Ok(b) if !b.is_empty() && b != "HEAD" => b,
        _ => "main".to_string(),
    };
    // (no answer from the repository: nothing to compare with - update.ps1 says why it cannot go on)
    if git(&["fetch", "--quiet", "--no-tags", "origin", &branch]).is_err() {
        return Ok(None);
    }
    let upstream = format!("origin/{branch}");
    let Ok(theirs) = git(&["rev-parse", "--verify", &upstream]) else { return Ok(None) };
    let head = git(&["rev-parse", "HEAD"]).map_err(|e| format!("cannot read the install's repository: {e}"))?;
    // (nothing new to merge: the update's merge is a no-op, the player's work is not in its way)
    if git(&["merge-base", "--is-ancestor", &theirs, "HEAD"]).is_ok() {
        return Ok(None);
    }
    let on_branch = git(&["rev-parse", "--abbrev-ref", "HEAD"]).is_ok_and(|b| b == branch);
    let commits: usize = git(&["rev-list", "--count", &format!("{theirs}..HEAD")]).ok().and_then(|n| n.parse().ok()).unwrap_or(0);
    // (the PS2Recomp submodule always has the port's patches applied: not the player's change)
    // (update.log: update.ps1's own log, not ignored by older copies)
    let changes = git(&["status", "--porcelain", "--ignore-submodules=dirty", "--", ".", ":(exclude)update.log"])
        .map_err(|e| format!("git status failed: {e}"))?;
    if commits == 0 && changes.is_empty() && on_branch {
        return Ok(None);
    }
    // (a copy not on its branch: that branch is moved below, so it must hold nothing of its own)
    if !on_branch {
        let local = format!("refs/heads/{branch}");
        if git(&["rev-parse", "--verify", "--quiet", &local]).is_ok()
            && git(&["merge-base", "--is-ancestor", &local, &theirs]).is_err()
            && git(&["merge-base", "--is-ancestor", &local, "HEAD"]).is_err()
        {
            return Err(format!("your copy is not on its branch {branch}, and that branch has commits of its own: put it back on {branch} (git switch {branch}), then update again"));
        }
    }
    let base = git(&["merge-base", "HEAD", &theirs]).map_err(|_| format!("your copy shares no history with {upstream}: cannot update it"))?;
    // Ignored files the update would write over (a file of yours with the name of a new one): stop.
    let added = git(&["diff", "--name-only", "--diff-filter=A", "--no-renames", &head, &theirs]).unwrap_or_default();
    for path in added.lines().filter(|p| !p.is_empty()) {
        if repo.join(path).exists() && git(&["check-ignore", "-q", "--no-index", path]).is_ok() {
            return Err(format!("the update brings {path}, and your copy has a file of its own there (an ignored one): move it away, then update again"));
        }
    }
    let mut t = LocalTime::default();
    unsafe { GetLocalTime(&mut t) };
    let stamp = format!("my-changes-{:04}-{:02}-{:02}-{:02}{:02}", t.year, t.month, t.day, t.hour, t.minute);
    let mut name = stamp.clone();
    let mut n = 2;
    while git(&["rev-parse", "--verify", "--quiet", &format!("refs/heads/{name}")]).is_ok() {
        name = format!("{stamp}-{n}");
        n += 1;
    }
    say(&format!("update: your own changes ({commits} commit(s) of your own, {} changed or new file(s)) are kept on the branch {name}",
                 changes.lines().count()));
    // 1. The backup commit, made beside the copy (in a temporary index): the folder, its index and
    //    its branch are not touched until a branch holding all of it exists.
    let kept = if changes.is_empty() {
        head.clone()
    } else {
        let temp = git(&["rev-parse", "--git-path", "tfa-keep.index"]).map(|p| repo.join(p)).map_err(|e| format!("git rev-parse: {e}"))?;
        if let Ok(index) = git(&["rev-parse", "--git-path", "index"]) {
            let _ = std::fs::copy(repo.join(index), &temp); // (none yet: git starts an empty one)
        }
        let tgit = |args: &[&str]| {
            let mut all = vec!["-c", "core.longpaths=true"];
            all.extend_from_slice(args);
            git_out_env(repo, &all, &[("GIT_INDEX_FILE", temp.as_os_str())])
        };
        let made = (|| -> Result<String, String> {
            tgit(&["add", "-A"]).map_err(|e| format!("git add: {e}"))?;
            // (update.ps1's own log stays out: an older copy does not ignore it)
            let _ = tgit(&["reset", "-q", "--", "update.log"]);
            // (everything in: nothing untracked or unstaged left beside it)
            let left = tgit(&["status", "--porcelain", "--ignore-submodules=dirty", "--", ".", ":(exclude)update.log"]).map_err(|e| format!("git status: {e}"))?;
            if let Some(l) = left.lines().find(|l| l.starts_with("??") || l.as_bytes().get(1).is_some_and(|c| *c != b' ')) {
                return Err(format!("a file left out: {l}"));
            }
            let tree = tgit(&["write-tree"]).map_err(|e| format!("git write-tree: {e}"))?;
            let mut commit: Vec<&str> = Vec::new();
            // (a commit needs a name: the player's own when git has one)
            if git(&["config", "user.email"]).map(|v| v.is_empty()).unwrap_or(true) {
                commit.extend_from_slice(&["-c", "user.name=Transformers setup", "-c", "user.email=setup@transformers.local"]);
            }
            commit.extend_from_slice(&["commit-tree", &tree, "-p", &head, "-m", "My own changes, kept before an update"]);
            git(&commit).map_err(|e| format!("git commit-tree: {e}"))
        })();
        let _ = std::fs::remove_file(&temp);
        made.map_err(|e| format!("your own changes could not be kept on a branch ({e}): nothing was changed, the update stops"))?
    };
    git(&["branch", &name, &kept])
        .map_err(|e| format!("your own changes could not be kept on a branch (git branch: {e}): nothing was changed, the update stops"))?;
    // From here on all of it is on the branch: a step that fails stops the update, nothing is lost.
    let stopped = |e: String| format!("your own changes are kept on the branch {name}, but the update could not go on ({e}): it stops here");
    // 2. The install's branch back to where it and the repository part (the update moves it on);
    //    its changed files go back to that version (theirs are on the branch).
    git(&["checkout", "-q", "-f", "-B", &branch, &base]).map_err(&stopped)?;
    // 3. The new files out of the way, one by one: exactly the ones on the branch (never a folder,
    //    never an ignored file).
    let untracked = git(&["status", "--porcelain", "-z", "-uall", "--ignore-submodules=dirty", "--", ".", ":(exclude)update.log"]).map_err(&stopped)?;
    for path in untracked.split('\0').filter_map(|e| e.strip_prefix("?? ")) {
        if git(&["cat-file", "-e", &format!("{kept}:{path}")]).is_err() {
            return Err(stopped(format!("{path} is not on the branch: left as it is")));
        }
        std::fs::remove_file(repo.join(path)).map_err(|e| stopped(format!("{path}: {e}")))?;
    }
    Ok(Some(name))
}

/// Is there a newer version on the repository's branch than the one built? (TODO 104)
fn check_updates(repo: &Path) -> Check {
    let mut c = Check { done: true, ..Default::default() };
    let Some(built) = built_commit(repo) else {
        c.error = "this folder's copy of the repository cannot be read".into();
        return c;
    };
    c.current = built.chars().take(7).collect();
    c.current_name = version_at(repo, &built).unwrap_or_default();
    c.date = git_out(repo, &["log", "-1", "--format=%cs", &built]).unwrap_or_default();
    let branch = match git_out(repo, &["rev-parse", "--abbrev-ref", "HEAD"]) {
        Ok(b) if !b.is_empty() && b != "HEAD" => b,
        _ => "main".into(),
    };
    if let Err(e) = git_out(repo, &["fetch", "--quiet", "--no-tags", "origin", &branch]) {
        c.error = if e.contains("could not read Username") || e.contains("terminal prompts disabled") || e.contains("Authentication") {
            "the repository asked for a sign-in: run update.ps1 once in the install folder (it asks), then check again".into()
        } else {
            e
        };
        return c;
    }
    let latest = match git_out(repo, &["rev-parse", "--verify", &format!("origin/{branch}")]) {
        Ok(l) => l,
        Err(e) => {
            c.error = e;
            return c;
        }
    };
    c.latest = latest.chars().take(7).collect();
    c.latest_name = version_at(repo, &latest).unwrap_or_default();
    if latest == built || git_out(repo, &["merge-base", "--is-ancestor", &latest, &built]).is_ok() {
        return c;
    }
    c.available = true;
    let range = format!("{built}..{latest}");
    c.count = git_out(repo, &["rev-list", "--count", &range]).ok().and_then(|n| n.parse().ok()).unwrap_or(0);
    for line in git_out(repo, &["log", "--no-merges", "-n", "40", "--format=%h%x09%s", &range]).unwrap_or_default().lines() {
        if let Some((hash, subject)) = line.split_once('\t') {
            if !subject.starts_with("port: sync from the working copy") && !is_version_bump(subject) && c.changes.len() < 15 {
                c.changes.push(format!("{hash}  {subject}"));
            }
        }
    }
    c
}

/// A picture as a 24-bit BMP (tests: --shot).
fn write_bmp(path: &str, image: &egui::ColorImage) -> std::io::Result<()> {
    let (w, h) = (image.size[0], image.size[1]);
    let row = (w * 3 + 3) & !3;
    let size = 54 + row * h;
    let mut out = Vec::with_capacity(size);
    out.extend_from_slice(b"BM");
    out.extend_from_slice(&(size as u32).to_le_bytes());
    out.extend_from_slice(&[0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0]);
    out.extend_from_slice(&(w as i32).to_le_bytes());
    out.extend_from_slice(&(h as i32).to_le_bytes());
    out.extend_from_slice(&[1, 0, 24, 0, 0, 0, 0, 0]);
    out.extend_from_slice(&((row * h) as u32).to_le_bytes());
    out.extend_from_slice(&[0; 16]);
    for y in (0..h).rev() {
        let start = out.len();
        for x in 0..w {
            let c = image.pixels[y * w + x];
            out.extend_from_slice(&[c.b(), c.g(), c.r()]);
        }
        out.resize(start + row, 0);
    }
    std::fs::write(path, out)
}

/// Whether a step's last lines say an installer is waiting for Windows' permission (winget's own words).
fn waiting_for_permission(recent_lower: &str) -> bool {
    recent_lower.contains("starting package install")
        || recent_lower.contains("expect a prompt")
        || recent_lower.contains("request to run as administrator")
        || recent_lower.contains("winget install")
        || recent_lower.contains("asks for permission") // (setup.ps1's own steps: the Build Tools, Visual Studio's installer)
}

/// What to do when Git could not be installed, from what winget said (`e`: its last lines, or why it did not start).
fn git_failure_text(e: &str) -> String {
    let lower = e.to_ascii_lowercase();
    let by_hand = "Or install Git by hand: https://git-scm.com/download/win (keep the default choices), then run the setup again.";
    // (exit code 2: Git's installer when Windows' question was declined or closed itself unanswered - the ThinkPad test)
    let cancelled = lower.contains("you cancelled the installation")
        || lower.lines().any(|l| l.trim_end().ends_with("exit code: 2") || l.trim_end().ends_with("exit code: 1223") || l.trim_end().ends_with("exit code: 1602"));
    if cancelled {
        format!("Windows asked for permission to install Git and nobody answered (or No was chosen). The question may show only as \
                 a flashing shield button in the taskbar, and it closes by itself after about 2 minutes. Run the setup again and click Yes.\n{by_hand}")
    } else if lower.contains("winget could not be started") {
        format!("winget (Windows' package manager) is not on this PC, so the setup cannot install the tools for you. Install or \
                 update 'App Installer' from the Microsoft Store (https://apps.microsoft.com/detail/9NBLGGH4NNS1) and run the setup again.\n{by_hand}")
    } else {
        format!("Git could not be installed or found.\n{by_hand}")
    }
}

fn has(path_env: &str, exe: &str) -> bool {
    path_env.split(';').any(|d| !d.is_empty() && Path::new(d).join(format!("{exe}.exe")).exists())
}

/// PATH plus the folder of an installed Git that is not on it (the usual install folders), if any.
fn with_git_dir(path_env: &str) -> String {
    let mut dirs: Vec<PathBuf> = Vec::new();
    for var in ["ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"] {
        if let Ok(v) = std::env::var(var) {
            dirs.push(Path::new(&v).join("Git").join("cmd"));
        }
    }
    if let Ok(v) = std::env::var("LOCALAPPDATA") {
        dirs.push(Path::new(&v).join("Programs").join("Git").join("cmd"));
    }
    match dirs.into_iter().find(|d| d.join("git.exe").exists()) {
        Some(d) => format!("{};{path_env}", d.display()),
        None => path_env.to_string(),
    }
}

/// Windows' own folders added to a PATH that lacks them (at its end: the player's order stays). A player's PC had a PATH
/// without them and the setup stopped at once: "powershell could not be started: program not found" (2026-11-09);
/// setup.ps1 also runs cmd and winget, which need System32.
fn with_system_dirs(path: &str) -> String {
    let root = std::env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".to_string());
    let mut out = path.trim_end_matches(';').to_string();
    for dir in [format!(r"{root}\System32"), root.clone(), format!(r"{root}\System32\Wbem"),
                format!(r"{root}\System32\WindowsPowerShell\v1.0")] {
        if !out.split(';').any(|p| p.trim().trim_end_matches('\\').eq_ignore_ascii_case(&dir)) {
            if !out.is_empty() { out.push(';'); }
            out.push_str(&dir);
        }
    }
    out
}

/// The program to start: Windows PowerShell by its full path (found even when PATH lacks its folder), else the name.
fn program(exe: &str) -> PathBuf {
    if exe.eq_ignore_ascii_case("powershell") {
        let root = std::env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".to_string());
        let full = PathBuf::from(format!(r"{root}\System32\WindowsPowerShell\v1.0\powershell.exe"));
        if full.is_file() {
            return full;
        }
    }
    PathBuf::from(exe)
}

/// PATH as the registry has it now (programs installed meanwhile), plus what we had.
fn fresh_path(old: &str) -> String {
    let now = winapi::registry_path();
    if now.is_empty() { old.to_string() } else { format!("{now};{old}") }
}

/// Starts the game (what Play.bat does): settings.ini made from settings.example.ini on the first
/// start, then the game's exe in the game's folder.
fn play(repo: &Path) {
    let _ = start_game(repo);
}

fn start_game(repo: &Path) -> Option<std::process::Child> {
    let game = game_folder(repo);
    let settings = game.join("settings.ini");
    let example = game.join("settings.example.ini");
    // (TODO 279: with the install's folders moved its settings.ini is the one in use - none made in the checkout)
    let moved = repo.parent().is_some_and(|f| layout_of(f) == 2 && f.join("settings.ini").exists());
    if !moved && !settings.exists() && example.exists() {
        let _ = std::fs::copy(&example, &settings);
    }
    Command::new(game_exe(repo)).current_dir(&game).spawn().ok()
}

/// (TODO 276) The game started for a launcher - Steam's non-Steam game, Quiver, a shortcut - and waited for: the
/// setup's process lives as long as the game, and as long as a game the game started again itself (a setting's
/// restart), so Steam shows the player in game for the whole session and keeps its overlay and Steam Input on it.
/// (Before, the setup ended at once: to Steam the game had closed as soon as it opened.) No window: nothing shows.
fn play_and_wait(repo: &Path) {
    let Some(child) = start_game(repo) else { return };
    let tree = winapi::ProcessTree::new(&child, false);
    let _ = winapi::wait_for_exit(child.id());
    loop {
        let left: Vec<u32> = tree.processes().into_iter().filter(|&pid| !winapi::is_console_host(pid)).collect();
        if left.is_empty() {
            break;
        }
        for pid in left {
            let _ = winapi::wait_for_exit(pid);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// The window.

/// (TODO 303) How often the window is drawn again while a step runs or the update check waits: the steps' texts, the
/// seconds and the progress bar follow at this pace. (The mouse and the keys still draw it at once.)
const TICK: std::time::Duration = std::time::Duration::from_millis(500);

/// (TODO 303) The running mark: a quarter arc moved an eighth of a turn each TICK. egui's Spinner asks for a new frame
/// every frame (it turns smoothly), so the window was drawn at the screen's refresh rate for the whole build - about
/// 14% of one core on a fast PC, for a few lines of text.
fn ticking_spinner(ui: &egui::Ui, rect: egui::Rect, color: Color32) {
    use std::f64::consts::TAU;
    let c = rect.center();
    let r = rect.width().min(rect.height()) * 0.5 - 1.5;
    let p = ui.painter();
    p.circle_stroke(c, r, Stroke::new(2.0, color.gamma_multiply(0.25)));
    let start = (ui.input(|i| i.time) / TICK.as_secs_f64()).floor() * TAU / 8.0;
    let points: Vec<egui::Pos2> = (0..=12)
        .map(|k| {
            let a = start + k as f64 * (TAU / 4.0) / 12.0;
            c + Vec2::new(a.cos() as f32, a.sin() as f32) * r
        })
        .collect();
    p.add(egui::Shape::line(points, Stroke::new(2.5, color)));
}

fn status_icon(ui: &mut egui::Ui, status: Status) {
    let (rect, _) = ui.allocate_exact_size(Vec2::splat(22.0), egui::Sense::hover());
    let c = rect.center();
    let p = ui.painter();
    match status {
        Status::Active => {
            ticking_spinner(ui, rect, ACCENT);
        }
        Status::Done => {
            p.circle_stroke(c, 9.5, Stroke::new(2.0, GOOD));
            p.line_segment([c + Vec2::new(-4.5, 0.5), c + Vec2::new(-1.0, 4.0)], Stroke::new(2.0, GOOD));
            p.line_segment([c + Vec2::new(-1.0, 4.0), c + Vec2::new(5.0, -3.5)], Stroke::new(2.0, GOOD));
        }
        Status::Failed => {
            p.circle_stroke(c, 9.5, Stroke::new(2.0, BAD));
            p.line_segment([c + Vec2::new(-4.0, -4.0), c + Vec2::new(4.0, 4.0)], Stroke::new(2.0, BAD));
            p.line_segment([c + Vec2::new(4.0, -4.0), c + Vec2::new(-4.0, 4.0)], Stroke::new(2.0, BAD));
        }
        Status::Pending | Status::Skipped => {
            p.circle_stroke(c, 9.5, Stroke::new(1.5, DIM));
        }
    }
}

fn pick_iso(target: &mut String, title: &str) {
    if let Some(f) = rfd::FileDialog::new().set_title(title).add_filter("Disc image", &["iso"]).pick_file() {
        *target = f.to_string_lossy().to_string();
    }
}

fn file_row(ui: &mut egui::Ui, label: &str, value: &mut String, hint: &str, pick: impl FnOnce(&mut String)) {
    ui.label(RichText::new(label).strong());
    ui.horizontal(|ui| {
        ui.add_sized([ui.available_width() - 90.0, 28.0], egui::TextEdit::singleline(value).hint_text(hint));
        if ui.add_sized([80.0, 28.0], egui::Button::new("Browse…")).clicked() {
            pick(value);
        }
    });
    ui.add_space(10.0);
}

impl eframe::App for App {
    // (TODO 289) Closing the window ends the running step, as before; only a setup that dies (a graphics driver
    // reset took one) leaves it running - to finish on its own, followed by the setup started again.
    fn on_exit(&mut self, _gl: Option<&eframe::glow::Context>) {
        if !self.play {
            self.cancel(); // (the game's own "Update now" goes on without the window, as before)
        }
    }

    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.poll();
        if self.phase == Phase::Running || self.check_rx.is_some() {
            ui.ctx().request_repaint_after(TICK); // (TODO 303: was every 100 ms, and every frame for the Spinner)
        }
        // (the game's own update, done: the game is starting - this window goes after a moment)
        if self.play && self.phase == Phase::Done(true) {
            if self.done_at.is_some_and(|t| t.elapsed().as_secs() >= 5) {
                ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
            }
            ui.ctx().request_repaint_after(std::time::Duration::from_millis(250));
        }
        if let Some(path) = self.shot.clone() {
            // (tests: the window's picture, a second after the check answered or the update ended)
            let settled = (self.phase == Phase::Installed && self.check.done) || matches!(self.phase, Phase::Done(_) | Phase::Form);
            if settled && self.settled_at.is_none() {
                self.settled_at = Some(Instant::now());
            }
            let state = *self.shot_state.lock().unwrap();
            if state == 0 && self.settled_at.is_some_and(|t| t.elapsed().as_millis() >= 1000) {
                *self.shot_state.lock().unwrap() = 1;
                let done = self.shot_state.clone();
                ui.ctx().request_screenshot(move |image| {
                    let _ = write_bmp(&path, &image);
                    *done.lock().unwrap() = 2;
                });
            }
            if state == 2 {
                ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
            }
            ui.ctx().request_repaint_after(std::time::Duration::from_millis(100));
        }
        egui::CentralPanel::default().frame(egui::Frame::new().fill(Color32::from_rgb(0x15, 0x17, 0x1C)).inner_margin(28.0)).show(ui, |ui| {
            match self.phase {
                Phase::Form => self.form(ui),
                Phase::Installed => self.installed_view(ui),
                _ => self.progress_view(ui),
            }
        });
    }
}

impl App {
    fn installed_view(&mut self, ui: &mut egui::Ui) {
        ui.label(RichText::new("Transformers Armada - PC version").size(26.0).color(Color32::WHITE));
        ui.add_space(4.0);
        ui.label(RichText::new(format!("Installed in {}", self.folder)).color(DIM));
        let unfinished = Path::new(&self.folder).join(UNFINISHED_MARK).exists();
        if unfinished {
            ui.add_space(8.0);
            ui.label(RichText::new("The setup did not finish last time. Press \"Finish the setup\" to carry on: finished steps are kept.")
                .color(Color32::from_rgb(240, 180, 0)));
            // (TODO 310) where it stopped, from the steps' own record (it survives the PC going off)
            if let Some(last) = last_step(Path::new(&self.folder)) {
                ui.label(RichText::new(format!("It stopped at: {last}")).color(DIM).small());
            }
        }
        ui.add_space(20.0);
        let c = self.check.clone();
        if !c.current.is_empty() {
            ui.horizontal(|ui| {
                ui.label(RichText::new(format!("Your version: {}", c.current_text())).size(15.0).color(Color32::WHITE));
                if !c.current_name.is_empty() {
                    // (the commit's code, small: for support)
                    ui.label(RichText::new(&c.current).color(DIM).small());
                }
            });
            ui.add_space(8.0);
        }
        if self.check_rx.is_some() {
            ui.horizontal(|ui| {
                let (rect, _) = ui.allocate_exact_size(Vec2::splat(16.0), egui::Sense::hover());
                ticking_spinner(ui, rect, ACCENT);
                ui.label(RichText::new("Checking for updates...").color(DIM));
            });
        } else if !c.error.is_empty() {
            ui.label(RichText::new(format!("Could not check for updates: {}", c.error)).color(BAD));
        } else if c.available {
            let n = if c.count == 1 { "1 change".to_string() } else { format!("{} changes", c.count) };
            ui.horizontal(|ui| {
                ui.label(RichText::new(format!("A new version is available: {} ({n})", c.latest_text())).size(17.0).color(ACCENT));
                if !c.latest_name.is_empty() {
                    ui.label(RichText::new(&c.latest).color(DIM).small());
                }
            });
            ui.add_space(8.0);
            egui::ScrollArea::vertical().max_height(220.0).show(ui, |ui| {
                for line in &c.changes {
                    ui.label(RichText::new(line).color(DIM));
                }
            });
            ui.add_space(6.0);
            let kept = if self.mods { "Your mods, their on / off choices, your settings and saves are kept." } else { "Your settings and saves are kept." };
            ui.label(RichText::new(format!("Updating rebuilds the game on this PC (a few minutes). {kept}")).color(DIM).small());
        } else if c.done {
            ui.label(RichText::new("Up to date.").size(17.0).color(GOOD));
        }
        // (PR #20) The HD textures preset, for an install that has a set: saved with the update (whose HD step shrinks
        // or makes what it needs), or applied at once in the maker's own window when there is no update.
        let has_set = self.hd_set_gb.is_some();
        if has_set {
            ui.add_space(12.0);
            ui.horizontal(|ui| {
                ui.label("HD texture preset");
                let gpu = self.gpu.as_ref();
                let text = |q: HdQuality| format!("{}  ({})", q.label(), q.estimate(gpu));
                egui::ComboBox::from_id_salt("hd_quality_update")
                    .width(240.0)
                    .selected_text(text(self.hd_quality))
                    .show_ui(ui, |ui| {
                        for q in HdQuality::ALL {
                            ui.selectable_value(&mut self.hd_quality, q, text(q)).on_hover_text(q.hover());
                        }
                    })
                    .response
                    .on_hover_text(self.hd_quality.hover());
                if self.hd_saved != Some(self.hd_quality) && !c.available
                    && ui.button("Apply").on_hover_text("Saved, and made in a small window of its own: larger images are shrunk (no graphics card needed), the ones this preset needs larger are made").clicked()
                {
                    self.hd_note = match self.save_hd_quality().and_then(|_| start_hd_maker(&self.repo())) {
                        Ok(()) => "Being made in a small window of its own. You can play meanwhile; restart the game once it is done.".into(),
                        Err(e) => e,
                    };
                }
            });
            let mut line = format!("Your HD set now: {:.1} GB.", self.hd_set_gb.unwrap_or(0.0));
            if self.hd_saved != Some(self.hd_quality) && c.available {
                line.push_str(" The new preset is applied with the update.");
            } else if !self.hd_note.is_empty() {
                line = self.hd_note.clone();
            }
            ui.label(RichText::new(line).color(DIM).small());
        }
        ui.with_layout(egui::Layout::bottom_up(egui::Align::Max), |ui| {
            ui.horizontal(|ui| {
                if c.available && self.check_rx.is_none() && ui.add_sized([150.0, 40.0], egui::Button::new(RichText::new("Update").size(16.0)).fill(ACCENT)).clicked() {
                    // (the update's HD step reads it; an update that does not touch the HD tools runs it for a new preset)
                    if has_set && self.hd_saved != Some(self.hd_quality) {
                        if let Err(e) = self.save_hd_quality() {
                            self.hd_note = e;
                        }
                    }
                    self.start_update();
                }
                if ui.add_sized([150.0, 40.0], egui::Button::new("Play")).clicked() {
                    play(&self.repo());
                }
                if ui.add_enabled(self.check_rx.is_none(), egui::Button::new("Check again").min_size(Vec2::new(150.0, 40.0))).clicked() {
                    self.start_check();
                }
                let again = if unfinished {
                    egui::Button::new(RichText::new("Finish the setup").size(16.0)).fill(ACCENT)
                } else {
                    egui::Button::new("Set up again...")
                };
                if ui.add_sized([150.0, 40.0], again).on_hover_text("The setup form: other discs, the HD textures").clicked() {
                    self.phase = Phase::Form;
                }
            });
        });
    }

    fn form(&mut self, ui: &mut egui::Ui) {
        ui.label(RichText::new("Transformers Armada - PC version").size(26.0).color(Color32::WHITE));
        ui.add_space(4.0);
        ui.label(RichText::new("Built on your PC from your own disc: nothing of the game is downloaded.").color(DIM));
        ui.add_space(20.0);

        file_row(ui, "Transformers disc image", &mut self.retail, "USA SLUS-20668 or Europe SLES-52388, .iso", |v| pick_iso(v, "Transformers disc image"));
        let retail_note = match std::fs::metadata(&self.retail) {
            Ok(m) if m.len() == USA_ISO_SIZE => Some(("the USA release (its checksum is checked by the setup)", GOOD)),
            Ok(m) if m.len() == PAL_ISO_SIZE => Some(("the European release (SLES-52388): experimental, the game is made for the USA disc", GOOD)),
            Ok(_) => Some(("not the size of the USA release (SLUS-20668) or the European one (SLES-52388): only those discs work", BAD)),
            Err(_) if !self.retail.is_empty() => Some(("file not found", BAD)),
            Err(_) => None,
        };
        if let Some((t, c)) = retail_note {
            ui.label(RichText::new(t).color(c));
            ui.add_space(8.0);
        }
        if self.mods {
            ui.label(RichText::new("Prototype disc image (optional)").strong());
            ui.horizontal(|ui| {
                ui.add_sized([ui.available_width() - 180.0, 28.0],
                    egui::TextEdit::singleline(&mut self.prototype).hint_text("Nov 2003 prototype (PS2 ALPHA 3944B), for the jet bike"));
                if ui.add_sized([80.0, 28.0], egui::Button::new("Browse…")).clicked() {
                    pick_iso(&mut self.prototype, "Prototype disc image");
                }
                // (or the small copy of it that the mods' make_prototype_extract.py makes)
                if ui.add_sized([80.0, 28.0], egui::Button::new("Folder…")).on_hover_text("The small copy made by make_prototype_extract.py").clicked() {
                    if let Some(f) = rfd::FileDialog::new().set_title("Prototype files folder").pick_folder() {
                        self.prototype = f.to_string_lossy().to_string();
                    }
                }
            });
        }
        ui.add_space(10.0);
        file_row(ui, "Install folder", &mut self.folder, "", |v| {
            if let Some(f) = rfd::FileDialog::new().set_title("Install folder").pick_folder() {
                *v = f.to_string_lossy().to_string();
            }
        });
        if self.installed() {
            ui.horizontal(|ui| {
                ui.label(RichText::new("The game is installed in this folder.").color(GOOD));
                if ui.button("Its version and updates").clicked() {
                    self.phase = Phase::Installed;
                    self.start_check();
                }
            });
            ui.add_space(6.0);
        }
        if self.low_end {
            ui.add_enabled(false, egui::Checkbox::new(&mut self.hd, "HD textures"));
            ui.label(RichText::new("Your graphics are built-in, so HD textures are off to keep the game smooth.").color(DIM).small());
        } else {
            ui.checkbox(&mut self.hd, "HD textures (recommended; made on your graphics card, you can play meanwhile)");
        }
        if self.hd {
            ui.horizontal(|ui| {
                ui.add_space(24.0);
                ui.label("Texture preset");
                let gpu = self.gpu.as_ref();
                let text = |q: HdQuality| format!("{}  ({})", q.label(), q.estimate(gpu));
                egui::ComboBox::from_id_salt("hd_quality")
                    .width(240.0)
                    .selected_text(text(self.hd_quality))
                    .show_ui(ui, |ui| {
                        for q in HdQuality::ALL {
                            ui.selectable_value(&mut self.hd_quality, q, text(q)).on_hover_text(q.hover());
                        }
                    })
                    .response
                    .on_hover_text(self.hd_quality.hover());
            });
            if let Some((name, vram)) = &self.gpu {
                ui.horizontal(|ui| {
                    ui.add_space(24.0);
                    let pick = suggest_quality(name, *vram).label();
                    ui.label(RichText::new(format!("suggested for your {name}: {pick}")).color(DIM).small());
                });
            }
            ui.horizontal(|ui| {
                ui.add_space(24.0);
                ui.checkbox(&mut self.hd_while_playing, "Keep making HD textures while I play").on_hover_text(
                    "Off: they wait while the game runs and carry on when you close it. On: they carry on, and the game runs a little slower meanwhile.");
            });
        }
        // (TODO 310) Not only the HD textures: the build too (a player's PC reset with 8 BIOS beeps near the end of the last bar)
        ui.checkbox(&mut self.hd_gentle, "Go easy on my PC (slower)").on_hover_text(
            "For a PC that restarts, freezes or overheats while it is set up or updated: the build uses half the processor at a lower priority, and the HD textures are made in short bursts with rests, never at full power for minutes. Takes about twice as long.");
        ui.add_space(6.0);
        ui.label(RichText::new("Missing tools (Git, Python, Visual Studio Build Tools) are installed with winget; Windows may ask for permission.").color(DIM).small());

        let ready = matches!(retail_note, Some((_, c)) if c == GOOD) && !self.folder.trim().is_empty();
        ui.with_layout(egui::Layout::bottom_up(egui::Align::Max), |ui| {
            if ui.add_enabled_ui(ready, |ui| ui.add_sized([150.0, 40.0], egui::Button::new(RichText::new("Install").size(16.0)).fill(ACCENT))).inner.clicked() {
                self.start();
            }
        });
    }

    fn progress_view(&mut self, ui: &mut egui::Ui) {
        let (title, sub) = match (&self.phase, self.update) {
            (Phase::Done(true), true) if self.play => ("Updated", "The game is starting again, where you were."),
            (Phase::Done(true), true) => ("Updated", "Start the game with Play.bat in the install folder (or Play below)."),
            (Phase::Done(false), true) if self.play => ("The update stopped", "The game starts again as it was; try the update later (the log says why)."),
            (Phase::Done(false), true) => ("The update stopped", "Try again: finished steps are kept."),
            (Phase::Done(true), false) if self.mods => ("Ready to play", "Start the game with Play.bat in the install folder. F5 = settings, F1 = mods and cheats."),
            (Phase::Done(true), false) => ("Ready to play", "Start the game with Play.bat in the install folder. F5 = settings."),
            (Phase::Done(false), false) => ("Setup stopped", "Run the setup again to carry on: finished steps are kept."),
            (_, true) if self.mods => ("Updating", "The new version is built on this PC. Your mods, their choices and your settings are kept."),
            (_, true) => ("Updating", "The new version is built on this PC. Your settings and saves are kept."),
            _ => ("Setting up", "You can leave this running. Finished steps are kept if it is interrupted."),
        };
        ui.label(RichText::new(title).size(26.0).color(Color32::WHITE));
        ui.add_space(4.0);
        ui.label(RichText::new(sub).color(DIM));
        if !self.kept.is_empty() {
            ui.add_space(6.0);
            ui.label(RichText::new(kept_text(&self.kept)).color(GOOD));
        }
        ui.add_space(22.0);
        for s in &self.steps {
            ui.horizontal(|ui| {
                status_icon(ui, s.status);
                ui.add_space(8.0);
                ui.vertical(|ui| {
                    let c = if s.status == Status::Pending || s.status == Status::Skipped { DIM } else { Color32::WHITE };
                    ui.horizontal(|ui| {
                        ui.label(RichText::new(s.title).size(15.0).color(c));
                        if s.status == Status::Active {
                            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                let t = if s.seconds >= 60 { format!("{} min {} s", s.seconds / 60, s.seconds % 60) } else { format!("{} s", s.seconds) };
                                ui.label(RichText::new(t).color(DIM));
                            });
                        }
                    });
                    ui.label(RichText::new(&s.detail).color(DIM));
                });
            });
            ui.add_space(10.0);
        }
        ui.add_space(8.0);
        ui.add(egui::ProgressBar::new(self.progress).desired_height(8.0).fill(ACCENT));
        if !self.error.is_empty() {
            ui.add_space(12.0);
            egui::ScrollArea::vertical().max_height(140.0).show(ui, |ui| {
                ui.label(RichText::new(&self.error).color(BAD).monospace().small());
            });
        }
        ui.with_layout(egui::Layout::bottom_up(egui::Align::Max), |ui| match self.phase {
            Phase::Running => {
                ui.horizontal(|ui| {
                    if ui.add_sized([150.0, 40.0], egui::Button::new("Cancel")).clicked() {
                        self.cancel();
                    }
                    // (TODO 219) The game while the HD textures are made: it takes each one as it is ready, and the
                    // upscale waits while the game runs (unless "Keep making HD textures while I play" is on).
                    if !self.update && self.hd_playable && self.steps[STEP_HD].status == Status::Active {
                        let tip = if self.hd_while_playing {
                            "The HD textures carry on while you play; each one shows up as it is ready."
                        } else {
                            "The HD textures wait while you play and carry on when you close the game. The ones made so far are used."
                        };
                        if ui.add_sized([150.0, 40.0], egui::Button::new(RichText::new("Play now").size(16.0)).fill(ACCENT)).on_hover_text(tip).clicked() {
                            play(&self.repo());
                        }
                    }
                });
            }
            Phase::Done(ok) => {
                ui.horizontal(|ui| {
                    let repo = self.repo();
                    if ok && ui.add_sized([150.0, 40.0], egui::Button::new(RichText::new("Play").size(16.0)).fill(ACCENT)).clicked() {
                        play(&repo);
                    }
                    if ui.add_sized([150.0, 40.0], egui::Button::new("Open folder")).clicked() {
                        winapi::open_folder(Path::new(&self.folder));
                    }
                    if !ok && ui.add_sized([150.0, 40.0], egui::Button::new("Try again")).clicked() {
                        self.error.clear();
                        for s in &mut self.steps {
                            if s.status != Status::Skipped {
                                s.status = Status::Pending;
                            }
                        }
                        self.play = false; // (the game started again already, after the failure)
                        self.wait_pid = 0;
                        if self.update {
                            self.start_update();
                        } else {
                            self.start();
                        }
                    }
                });
            }
            Phase::Form | Phase::Installed => {}
        });
    }
}

fn main() -> eframe::Result {
    // Handle the launcher's entry points before creating a window or probing the GPU.
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "--prepare" || a == "--launch") {
        std::process::exit(launcher::run(&args));
    }
    let mut viewport = egui::ViewportBuilder::default().with_inner_size([760.0, 560.0]).with_min_inner_size([640.0, 520.0]).with_title("Transformers Armada - Setup");
    // (tests on a worker: --offscreen puts the window where no screen is, out of the taskbar)
    if std::env::args().any(|a| a == "--offscreen") {
        viewport = viewport.with_position([-20000.0, -20000.0]).with_taskbar(false);
    }
    let options = eframe::NativeOptions { viewport, ..Default::default() };
    // tfa-setup.exe [--iso <retail>] [--prototype <iso>] [--folder <dir>] [--no-hd] [--start]: the form
    // filled in (and --start: started at once), for a shortcut or a test. [--repo <url>] [--mods]:
    // another repository (and its mods' parts), see the top of this file.
    // On a folder with the game installed: its version and the update check. --update: the update at
    // once; --play: the game started again after it; --wait-pid <n>: the update waits for that process
    // (the game) to close - the game's own "Update now" (TODO 104). --check [--out <file>]: no window,
    // the check's answer as text (exit code 0 up to date, 10 a new version, 1 could not check).
    let mut app = App::new();
    let all: Vec<String> = std::env::args().skip(1).collect();
    let repo_arg = all.iter().position(|a| a == "--repo").and_then(|i| all.get(i + 1).cloned());
    app.set_flavour(repo_arg, all.iter().any(|a| a == "--mods"));
    app.detect_gpu();
    // (the copy a setup leaves in the install folder opens on that folder; a copy elsewhere - a launcher's
    // own, Quiver - on the folder the last install finished in)
    let mut found = false;
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)) {
        if checkout_in(&dir, &app.repo_url).join(".git").exists() {
            app.folder = dir.to_string_lossy().to_string();
            found = true;
        }
    }
    if !found {
        if let Some(dir) = remembered_folder() {
            if checkout_in(Path::new(&dir), &app.repo_url).join(".git").exists() {
                app.folder = dir;
            }
        }
    }
    app.load_hd_options();
    // Started with nothing on its command line on a finished install (a launcher's Play button, a shortcut to
    // the setup): the game starts, no setup window. Shift held while starting, or --setup: the setup's window.
    if all.is_empty() && app.installed() && game_exe(&app.repo()).exists() && !Path::new(&app.folder).join(UNFINISHED_MARK).exists()
        && !winapi::shift_held()
    {
        play_and_wait(&app.repo());
        return Ok(());
    }
    let mut start = false;
    let mut update = false;
    let mut check = false;
    let mut out: Option<String> = None;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--iso" => app.retail = args.next().unwrap_or_default(),
            "--prototype" => app.prototype = args.next().unwrap_or_default(),
            "--folder" => app.folder = args.next().unwrap_or_default(),
            "--no-hd" => app.hd = false,
            "--start" => start = true,
            "--update" => update = true,
            "--play" => app.play = true,
            "--wait-pid" => app.wait_pid = args.next().and_then(|n| n.parse().ok()).unwrap_or(0),
            "--check" => check = true,
            "--out" => out = args.next(),
            "--shot" => app.shot = args.next(),
            "--setup" => {} // (the setup's window on a finished install: see above)
            "--repo" => {
                let _ = args.next(); // (read above)
            }
            _ => {}
        }
    }
    // (TODO 289) A step left running by a setup whose window died: this window follows it to its end.
    if !check {
        if let Some(left) = LeftStep::find(Path::new(&app.folder)) {
            app.follow_left_step(left);
            return eframe::run_native("Transformers Armada - Setup", options, Box::new(|cc| {
                cc.egui_ctx.set_visuals(egui::Visuals::dark());
                Ok(Box::new(app))
            }));
        }
    }
    if check {
        let c = check_updates(&app.repo());
        let mut text = format!("version {} [{}]\n", c.current_text(), c.current);
        if !c.error.is_empty() {
            text += &format!("could not check: {}\n", c.error);
        } else if c.available {
            text += &format!("update available: {} ({} commits) [{}]\n", c.latest_text(), c.count, c.latest);
            for l in &c.changes {
                text += &format!("  {l}\n");
            }
        } else {
            text += "up to date\n";
        }
        match out {
            Some(f) => {
                let _ = std::fs::write(f, &text);
            }
            None => print!("{text}"),
        }
        std::process::exit(if !c.error.is_empty() { 1 } else if c.available { 10 } else { 0 });
    }
    if update && app.installed() {
        app.start_update();
    } else if start && !app.retail.is_empty() {
        app.start();
    } else if app.installed() {
        app.phase = Phase::Installed;
        app.start_check();
    }
    eframe::run_native("Transformers Armada - Setup", options, Box::new(|cc| {
        cc.egui_ctx.set_visuals(egui::Visuals::dark());
        Ok(Box::new(app))
    }))
}

#[cfg(test)]
mod path_tests {
    use super::*;

    #[test]
    fn system_dirs_added_once_at_the_end() {
        let root = std::env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".to_string());
        let out = with_system_dirs(r"C:\Tools;");
        assert!(out.starts_with(r"C:\Tools;"));
        assert!(out.to_lowercase().contains(&format!(r"{root}\System32\WindowsPowerShell\v1.0").to_lowercase()));
        assert_eq!(with_system_dirs(&out), out);
    }

    #[test]
    fn powershell_found_by_full_path() {
        assert!(program("powershell").is_absolute());
        assert!(program("powershell").is_file());
        assert_eq!(program("winget"), PathBuf::from("winget"));
    }

    #[test]
    fn powershell_starts_with_an_empty_path() {
        let out = Command::new(program("powershell"))
            .args(["-NoProfile", "-NonInteractive", "-Command", "Get-Command cmd | Select-Object -Expand Source"])
            .env("PATH", with_system_dirs(""))
            .output()
            .expect("powershell starts without PATH");
        assert!(String::from_utf8_lossy(&out.stdout).to_lowercase().contains("cmd.exe"));
    }

    #[test]
    fn layout_read_from_layout_json() {
        let dir = std::env::temp_dir().join(format!("tfa_layout_test_{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        assert_eq!(layout_of(&dir), 1);
        std::fs::write(dir.join("layout.json"), "{\"layout\": 2}
").unwrap();
        assert_eq!(layout_of(&dir), 2);
        assert_eq!(checkout_in(&dir, "https://github.com/kayliox/transformers-armada-recomp"), dir.join("transformers-armada-recomp"));
        std::fs::create_dir_all(dir.join("source").join(".git")).unwrap();
        assert_eq!(checkout_in(&dir, "https://github.com/kayliox/transformers-armada-recomp"), dir.join("source"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn permission_wait_seen_in_the_last_lines() {
        let winget = "starting package install...\nthe installer will request to run as administrator. expect a prompt.";
        assert!(waiting_for_permission(winget));
        assert!(!waiting_for_permission("downloading https://example.org/x.exe"));
    }

    #[test]
    fn git_failures_say_what_to_do() {
        assert!(git_failure_text("You cancelled the installation.\nInstaller failed with exit code: 2").contains("click Yes"));
        assert!(git_failure_text("Installer failed with exit code: 2").contains("nobody answered"));
        assert!(!git_failure_text("Installer failed with exit code: 21").contains("nobody answered"));
        assert!(git_failure_text("winget could not be started: program not found").contains("App Installer"));
        assert!(git_failure_text("something else").contains("https://git-scm.com/download/win"));
    }
}

#[cfg(test)]
mod hd_quality_tests {
    use super::*;

    const GB: u64 = 1 << 30;

    #[test]
    fn suggested_preset_follows_the_card() {
        assert_eq!(suggest_quality("NVIDIA GeForce GTX 1660 SUPER", 6 * GB), HdQuality::Fast);
        assert_eq!(suggest_quality("AMD Custom GPU 0405", GB), HdQuality::Fast); // Steam Deck
        assert_eq!(suggest_quality("NVIDIA GeForce RTX 3070", 8 * GB), HdQuality::Balanced);
        assert_eq!(suggest_quality("NVIDIA GeForce RTX 4070 Ti", 12 * GB), HdQuality::Balanced);
        assert_eq!(suggest_quality("NVIDIA GeForce RTX 4090", 24 * GB), HdQuality::Best);
        assert_eq!(suggest_quality("NVIDIA GeForce RTX 5080", 16 * GB), HdQuality::Best);
        assert_eq!(suggest_quality("AMD Radeon RX 7900 XTX", 24 * GB), HdQuality::Best);
        assert_eq!(suggest_quality("AMD Radeon RX 7800 XT", 16 * GB), HdQuality::Balanced);
        assert_eq!(suggest_quality("AMD Radeon RX 9070 XT", 16 * GB), HdQuality::Best);
        assert_eq!(suggest_quality("Some Unknown Card", 16 * GB), HdQuality::Fast);
    }

    #[test]
    fn presets_match_setup_ps1() {
        let names: Vec<&str> = HdQuality::ALL.iter().map(|q| q.arg()).collect();
        assert_eq!(names, ["fast", "balanced", "best"]);
        let setup = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../setup.ps1")).unwrap();
        assert!(setup.contains(r#"[ValidateSet("", "fast", "balanced", "best")][string]$HdQuality"#));
    }

    #[test]
    fn estimate_is_slower_on_a_small_card() {
        let strong = ("NVIDIA GeForce RTX 3070".to_string(), 8 * GB);
        let small = ("NVIDIA GeForce GTX 1060".to_string(), 6 * GB);
        assert_eq!(HdQuality::Balanced.estimate(Some(&strong)), "about 25 min, 1.4 GB");
        assert_eq!(HdQuality::Balanced.estimate(Some(&small)), "about 45 min, 1.4 GB");
        assert_eq!(HdQuality::Fast.estimate(None), "about 20 min, 0.6 GB");
    }
}
