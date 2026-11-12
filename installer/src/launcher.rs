//! RecompLauncher's headless prepare and tracked launch entry points.

use super::*;

#[derive(Debug, PartialEq)]
struct Options {
    launch: bool,
    iso: PathBuf,
    folder: PathBuf,
    game_args: Vec<String>,
}

fn options(args: &[String]) -> Result<Options, String> {
    let launch = match args.first().map(String::as_str) {
        Some("--prepare") => false,
        Some("--launch") => true,
        _ => return Err("Start with --prepare or --launch.".into()),
    };
    let (mut iso, mut folder) = (None, None);
    let mut game_args = Vec::new();
    let mut i = 1;
    while i < args.len() {
        if args[i] == "--" && launch {
            game_args = args[i + 1..].to_vec();
            break;
        }
        let target = match args[i].as_str() {
            "--iso" if iso.is_none() => &mut iso,
            "--folder" if folder.is_none() => &mut folder,
            _ => return Err(format!("Unknown or repeated option: {}", args[i])),
        };
        i += 1;
        let value = args.get(i).filter(|v| !v.is_empty() && !v.starts_with("--")).ok_or("Missing option value.")?;
        *target = Some(PathBuf::from(value));
        i += 1;
    }
    Ok(Options {
        launch,
        iso: iso.ok_or("Give --iso <your USA disc image>.")?,
        folder: folder.ok_or("Give --folder <the installation folder>.")?,
        game_args,
    })
}

fn revision() -> Option<String> {
    // GitHub supplies this to cargo automatically. A release therefore builds its own source,
    // including when the launcher installs an older release. Local development builds use main.
    option_env!("GITHUB_SHA").filter(|s| s.len() == 40 && s.bytes().all(|b| b.is_ascii_hexdigit())).map(str::to_string)
}

fn needs_build(repo: &Path, revision: Option<&str>) -> bool {
    if !game_exe(repo).is_file() { return true; }
    let Ok(text) = std::fs::read_to_string(repo.join(".setup.json")) else { return true };
    // Unlike built_commit(), a launch must not mistake HEAD for a successful build.
    let successful = (|| {
        let at = text.find("\"SetUpAt\"")?;
        let value = text[at + 9..].trim_start().strip_prefix(':')?.trim_start().strip_prefix('"')?.split_once('"')?.0;
        (value.len() == 40 && value.bytes().all(|b| b.is_ascii_hexdigit())).then_some(value)
    })();
    match successful {
        None => true,
        Some(built) => revision.is_some_and(|wanted| !built.eq_ignore_ascii_case(wanted)),
    }
}

fn prepare(opts: &Options, revision: Option<String>) -> Result<(), String> {
    if !opts.iso.is_file() { return Err("The selected disc image is missing. Choose your USA ISO again.".into()); }
    let job = Job {
        repo_url: REPO_URL.into(), mods: false, retail: opts.iso.to_string_lossy().into_owned(),
        prototype: String::new(), folder: opts.folder.clone(), hd: false, hd_fast: false,
        hd_while_playing: false, child: Arc::new(Mutex::new(None)), update: false,
        play: false, wait_pid: 0, source_ref: revision, managed: true,
    };
    let (tx, rx) = channel();
    std::thread::spawn(move || { let result = job.run(&tx); let _ = tx.send(Msg::Finished(result)); });
    let mut progress = 0.0f32;
    while let Ok(msg) = rx.recv() {
        match msg {
            Msg::Progress(value) => progress = progress.max(value.clamp(0.0, 1.0)),
            Msg::Detail(_, text) => println!("progress {progress:.3} {}", text.replace(['\r', '\n'], " ")),
            Msg::Finished(result) => {
                result?;
                let repo = opts.folder.join(repo_name(REPO_URL));
                if !game_exe(&repo).is_file() { return Err("The build finished without its game executable.".into()); }
                println!("progress 1 Ready to play");
                return Ok(());
            }
            _ => {}
        }
    }
    Err("The setup worker stopped unexpectedly. See setup.log.".into())
}

/// After the first game process: a setting or mod that takes a restart starts the game again and closes this one,
/// and the new game is in the same job - wait for it too (the session goes on, and closing the job would end it).
/// `code` becomes the last game's exit code (a console host in the job is waited for, but its code is not the game's).
fn wait_for_rest(tree: &winapi::ProcessTree, code: &mut i32) {
    loop {
        let left: Vec<(u32, bool)> = tree.processes().into_iter().map(|pid| (pid, winapi::is_console_host(pid))).collect();
        let Some(&(pid, host)) = left.iter().min_by_key(|(_, host)| *host) else { return };
        match winapi::wait_for_exit(pid) {
            Some(next) if !host => *code = next,
            Some(_) => {}
            None => std::thread::sleep(std::time::Duration::from_millis(200)),
        }
    }
}

pub(super) fn run(args: &[String]) -> i32 {
    let mut opts = match options(args) {
        Ok(opts) => opts,
        Err(error) => { eprintln!("{error}"); return 2; }
    };
    let absolute = || -> std::io::Result<(PathBuf, PathBuf)> {
        Ok((std::path::absolute(&opts.folder)?, std::path::absolute(&opts.iso)?))
    };
    match absolute() {
        Ok((folder, iso)) => { opts.folder = folder; opts.iso = iso; }
        Err(error) => { eprintln!("Couldn't resolve the installation paths: {error}"); return 2; }
    }
    let repo = opts.folder.join(repo_name(REPO_URL));
    let revision = revision();
    if !opts.launch || opts.folder.join(UNFINISHED_MARK).exists() || needs_build(&repo, revision.as_deref()) {
        if let Err(error) = prepare(&opts, revision) {
            eprintln!("{error}");
            println!("progress 0 Setup failed. See setup.log in the installation folder.");
            return 1;
        }
    }
    if !opts.launch { return 0; }
    // Stay alive until the actual game closes: session time and save backups belong to the game.
    let game = game_folder(&repo);
    let mut command = Command::new(game_exe(&repo));
    command.current_dir(&game).args(&opts.game_args).env("TF_UPDATE_CHECK", "0");
    match command.spawn() {
        Ok(mut child) => {
            let tree = winapi::ProcessTree::new(&child, true);
            let mut code = match child.wait() {
                Ok(status) => status.code().unwrap_or(1),
                Err(error) => { eprintln!("Couldn't wait for the game: {error}"); return 1; }
            };
            wait_for_rest(&tree, &mut code);
            code
        }
        Err(error) => { eprintln!("Couldn't start the game: {error}"); 1 }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(values: &[&str]) -> Vec<String> { values.iter().map(|v| v.to_string()).collect() }

    #[test]
    fn arguments_keep_paths_and_forward_game_arguments_only_after_separator() {
        let opts = options(&args(&["--launch", "--iso", "D:\\My discs\\TF's $disc.iso", "--folder", "C:\\My Games\\TF", "--", "--flag", "two words"])).unwrap();
        assert!(opts.launch);
        assert_eq!(opts.iso, PathBuf::from("D:\\My discs\\TF's $disc.iso"));
        assert_eq!(opts.game_args, ["--flag", "two words"]);
        assert!(!options(&args(&["--prepare", "--iso", "disc.iso", "--folder", "install"])).unwrap().launch);
    }

    #[test]
    fn malformed_requests_never_fall_back_to_the_setup_window() {
        for request in [vec!["--prepare"], vec!["--launch", "--iso"], vec!["--prepare", "--iso", "a", "--folder", "b", "--iso", "c"], vec!["--launch", "--repo", "other"]] {
            assert!(options(&args(&request)).is_err());
        }
    }

    #[test]
    fn a_process_started_again_inside_the_job_is_waited_for() {
        use std::os::windows::process::CommandExt;
        use std::sync::atomic::{AtomicBool, Ordering};
        use std::sync::Arc;
        // (as the game's restart: the first process starts the next and ends at once; the next stays in the job)
        // (a second's wait first: the job is assigned right after the start, as for the game long before its restart)
        // The started-again process runs until the test creates a file (or about 5 minutes pass), so the test - not a
        // timing that a busy PC can stretch - decides when it ends: the wait must return only after that.
        let release = std::env::temp_dir().join(format!("tfa-launcher-release-{}", std::process::id()));
        let _ = std::fs::remove_file(&release);
        let next = format!("for /l %i in (1,1,300) do @(if exist \"{0}\" (exit 0) else ping -n 2 127.0.0.1 >nul)", release.display());
        let mut first = Command::new("cmd")
            .raw_arg(format!("/c \"ping -n 2 127.0.0.1 >nul & start /b cmd /c \"{next}\"\""))
            .stdout(Stdio::null()).stderr(Stdio::null()).spawn().unwrap();
        let tree = winapi::ProcessTree::new(&first, true);
        let mut code = first.wait().unwrap().code().unwrap_or(1);
        assert!(!tree.processes().is_empty(), "the started-again process is still in the job");
        let released = Arc::new(AtomicBool::new(false));
        let releaser = {
            let (released, release) = (released.clone(), release.clone());
            std::thread::spawn(move || {
                std::thread::sleep(std::time::Duration::from_millis(500));
                released.store(true, Ordering::SeqCst);
                std::fs::write(&release, b"release").unwrap();
            })
        };
        wait_for_rest(&tree, &mut code);
        assert!(released.load(Ordering::SeqCst), "waited for the started-again process");
        releaser.join().unwrap();
        let _ = std::fs::remove_file(&release);
        assert_eq!(code, 0);
        assert!(tree.processes().is_empty());
    }

    #[test]
    fn executable_without_successful_setup_is_not_ready() {
        let root = std::env::temp_dir().join(format!("tfa-launcher-test-{}", std::process::id()));
        std::fs::create_dir_all(game_exe(&root).parent().unwrap()).unwrap();
        std::fs::write(game_exe(&root), b"test fixture").unwrap();
        let first = "a".repeat(40);
        let next = "b".repeat(40);
        assert!(needs_build(&root, Some(&first)));
        std::fs::write(root.join(".setup.json"), "{\"SetUpAt\":\"\"}").unwrap();
        assert!(needs_build(&root, Some(&first)));
        std::fs::write(root.join(".setup.json"), format!("{{\"SetUpAt\":\"{first}\"}}")).unwrap();
        assert!(!needs_build(&root, Some(&first)));
        assert!(needs_build(&root, Some(&next)));
        std::fs::remove_dir_all(&root).unwrap();
    }
}
