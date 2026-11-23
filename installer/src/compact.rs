// (TODO 451) The compact updater window: the game's own "Update now" (F1, About) opens this program as
// `--update --compact --play --wait-pid <the game>`. A small borderless window in the F1 menu's look (the game's
// menu_style: its colours, its fonts, its spacing) over the same update as the full setup window: the same Job, the
// same update script, the same steps, forecast, checks and report. Only the window is different.
//
// It shows the version the update goes from and to, the patch notes between the two (CHANGELOG.md's sections), the step
// in plain words with the progress bar, and at the end either starts the game and closes, says "Update done" (the
// player unticked "Start the game when the update is done"), or says why the update stopped, with the report to attach
// when asking for help. On top of other windows while the game is closed; started without --compact (or opened by
// hand) the program shows its full window as before.

use crate::{App, Phase, Status, STEP_DOWNLOAD, STEP_GAME, STEP_HD, STEP_MODS};
use eframe::egui::{self, Color32, CornerRadius, FontFamily, FontId, Pos2, Rect, Sense, Shape, Stroke, Vec2};
use std::path::{Path, PathBuf};
use std::sync::Arc;

// The F1 menu's colours (transformers/src/menu_style.cpp, its colour table).
const RED: Color32 = Color32::from_rgb(214, 32, 47);
const RED_HOVER: Color32 = Color32::from_rgb(236, 52, 66);
const RED_DARK: Color32 = Color32::from_rgb(110, 14, 24);
const BLUE: Color32 = Color32::from_rgb(54, 209, 255);
const AMBER: Color32 = Color32::from_rgb(255, 181, 71);
const TEXT: Color32 = Color32::from_rgb(232, 236, 242);
const DIM: Color32 = Color32::from_rgb(138, 148, 166);
const PANEL: Color32 = Color32::from_rgb(18, 22, 29);
const PANEL_LIFT: Color32 = Color32::from_rgb(30, 37, 48);
const LINE: Color32 = Color32::from_rgb(48, 58, 72);
const FRAME: Color32 = Color32::from_rgb(12, 15, 20);
const BUTTON: Color32 = Color32::from_rgb(22, 27, 36);

const TITLE_FAMILY: &str = "tfa_title";
const HEADING_FAMILY: &str = "tfa_heading";

fn title_font(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name(TITLE_FAMILY.into()))
}

fn heading_font(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name(HEADING_FAMILY.into()))
}

fn text_font(size: f32) -> FontId {
    FontId::new(size, FontFamily::Proportional)
}

// ------------------------------------------------------------------------------------------------
// The theme: the menu's style (menu_style::applyStyle) and fonts (menu_style::prepare), for egui.

/// A font file as the game finds it: the fonts folder in the game's folder, then beside this program; Bahnschrift also
/// from Windows' own fonts; on Linux the system's sans (Noto, DejaVu) for what is missing, as the game does.
fn font_file(game: &Path, name: &str) -> Option<Vec<u8>> {
    let mut places = vec![game.join("fonts").join(name)];
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)) {
        places.push(dir.join("fonts").join(name));
    }
    #[cfg(windows)]
    if name == "Bahnschrift.ttf" {
        let windows = std::env::var("WINDIR").unwrap_or_else(|_| "C:\\Windows".into());
        places.push(Path::new(&windows).join("Fonts").join("bahnschrift.ttf"));
    }
    #[cfg(not(windows))]
    {
        let bold = name != "Bahnschrift.ttf";
        for dir in ["/usr/share/fonts/noto/", "/usr/share/fonts/truetype/noto/", "/usr/share/fonts/google-noto/"] {
            places.push(Path::new(dir).join(if bold { "NotoSans-Bold.ttf" } else { "NotoSans-Regular.ttf" }));
        }
        for dir in ["/usr/share/fonts/TTF/", "/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/dejavu-sans-fonts/"] {
            places.push(Path::new(dir).join(if bold { "DejaVuSans-Bold.ttf" } else { "DejaVuSans.ttf" }));
        }
    }
    places.into_iter().find_map(|p| std::fs::read(p).ok())
}

/// The fonts: Bahnschrift for the text, Nulshock Bd for the headings, Transformers Movie for the title; a missing one
/// falls back as in the game (the heading to the text's, the title to the heading's), egui's own font after all of them.
fn install_fonts(ctx: &egui::Context, game: &Path) {
    let mut defs = egui::FontDefinitions::default();
    let mut add = |key: &str, file: &str| -> Option<String> {
        let bytes = font_file(game, file)?;
        defs.font_data.insert(key.to_string(), Arc::new(egui::FontData::from_owned(bytes)));
        Some(key.to_string())
    };
    let (text, heading, title) = (add("tfa_text", "Bahnschrift.ttf"), add("tfa_heading_font", "Nulshock Bd.otf"),
                                  add("tfa_title_font", "Transformers Movie.ttf"));
    let proportional = defs.families.entry(FontFamily::Proportional).or_default();
    if let Some(t) = text {
        proportional.insert(0, t);
    }
    let base = proportional.clone();
    let headings: Vec<String> = heading.into_iter().chain(base.iter().cloned()).collect();
    let titles: Vec<String> = title.into_iter().chain(headings.iter().cloned()).collect();
    defs.families.insert(FontFamily::Name(HEADING_FAMILY.into()), headings);
    defs.families.insert(FontFamily::Name(TITLE_FAMILY.into()), titles);
    ctx.set_fonts(defs);
}

/// The menu's style for egui: its colour table, rounding 0 for windows and 2 for frames, ItemSpacing 10 x 8.
pub fn apply_theme(ctx: &egui::Context, game: &Path) {
    install_fonts(ctx, game);
    let mut v = egui::Visuals::dark();
    v.panel_fill = PANEL;
    v.window_fill = PANEL;
    v.extreme_bg_color = FRAME;
    v.faint_bg_color = Color32::from_rgba_unmultiplied(255, 255, 255, 8);
    v.window_corner_radius = CornerRadius::ZERO;
    v.window_stroke = Stroke::new(1.0, LINE);
    v.hyperlink_color = BLUE;
    v.warn_fg_color = AMBER;
    v.error_fg_color = RED;
    v.selection.bg_fill = Color32::from_rgba_unmultiplied(214, 32, 47, 90);
    v.selection.stroke = Stroke::new(1.0, BLUE);
    let frame = CornerRadius::same(2);
    let w = &mut v.widgets;
    for (state, fill, stroke, fg) in [(&mut w.noninteractive, PANEL, LINE, TEXT), (&mut w.inactive, Color32::from_rgb(26, 32, 42), LINE, TEXT),
                                      (&mut w.hovered, Color32::from_rgb(40, 50, 64), BLUE, Color32::WHITE),
                                      (&mut w.active, RED_DARK, RED, Color32::WHITE), (&mut w.open, RED_DARK, RED, Color32::WHITE)] {
        state.bg_fill = fill;
        state.weak_bg_fill = fill;
        state.bg_stroke = Stroke::new(1.0, stroke);
        state.fg_stroke = Stroke::new(1.0, fg);
        state.corner_radius = frame;
        state.expansion = 0.0;
    }
    ctx.set_visuals_of(egui::Theme::Dark, v.clone());
    ctx.set_visuals_of(egui::Theme::Light, v);
    ctx.all_styles_mut(|s| {
        s.spacing.item_spacing = Vec2::new(10.0, 8.0);
        s.spacing.button_padding = Vec2::new(10.0, 6.0);
        s.text_styles.insert(egui::TextStyle::Body, text_font(15.0));
        s.text_styles.insert(egui::TextStyle::Button, text_font(15.0));
        s.text_styles.insert(egui::TextStyle::Small, text_font(12.5));
        s.text_styles.insert(egui::TextStyle::Heading, heading_font(16.0));
        s.text_styles.insert(egui::TextStyle::Monospace, FontId::new(12.5, FontFamily::Monospace));
    });
}

// ------------------------------------------------------------------------------------------------
// The patch notes: CHANGELOG.md's sections between the version the player has and the one the update brings.

#[derive(Clone, Default)]
pub struct Notes {
    pub done: bool,
    pub from: String, // "0.9.8"; a commit's short code when it has no number; "" not known
    pub to: String,
    pub sections: Vec<Section>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Section {
    pub version: String,
    pub lines: Vec<NoteLine>,
}

#[derive(Clone, Debug, PartialEq)]
pub enum NoteLine {
    Heading(String), // **Fixed**, **New**, **Changed**
    Bullet(String),
    Text(String),
}

fn version_number(v: &str) -> Option<(u32, u32, u32)> {
    let mut parts = v.trim().split('.').map(|p| p.parse::<u32>().ok());
    let n = (parts.next()??, parts.next()??, parts.next()??);
    parts.next().is_none().then_some(n)
}

/// Markdown's marks left out: the notes are read, not rendered as Markdown.
fn plain(text: &str) -> String {
    text.replace("**", "").replace('`', "").trim().to_string()
}

/// The sections of `changelog` a player going from `from` to `to` has not seen, newest first. With both numbers: the
/// ones after `from` up to `to`. Only `from`: everything after it. Only `to`: its own. A `to` without a number (a
/// development copy): everything after `from`, and "Unreleased".
pub fn sections_between(changelog: &str, from: &str, to: &str) -> Vec<Section> {
    let (f, t) = (version_number(from), version_number(to));
    let mut out: Vec<Section> = Vec::new();
    let mut keep = false;
    for raw in changelog.lines() {
        let line = raw.trim_end();
        if let Some(head) = line.strip_prefix("## ") {
            let name = head.trim().trim_start_matches(['v', 'V']).to_string();
            keep = match (version_number(&name), f, t) {
                (Some(v), Some(f), Some(t)) => v > f && v <= t,
                (Some(v), Some(f), None) => v > f,
                (Some(v), None, Some(t)) => v == t,
                (Some(_), None, None) => out.is_empty(),
                (None, _, t) => t.is_none() && name.eq_ignore_ascii_case("unreleased"),
            };
            if keep {
                out.push(Section { version: name, lines: Vec::new() });
            }
            if out.len() > 20 {
                break;
            }
            continue;
        }
        if !keep || line.trim().is_empty() || line.starts_with("# ") {
            continue;
        }
        let Some(section) = out.last_mut() else { continue };
        let t = line.trim();
        if t.starts_with("**") && t.ends_with("**") && t.len() > 4 {
            section.lines.push(NoteLine::Heading(plain(t)));
        } else if let Some(b) = t.strip_prefix("- ").or_else(|| t.strip_prefix("* ")) {
            section.lines.push(NoteLine::Bullet(plain(b)));
        } else if line.starts_with(' ') && matches!(section.lines.last(), Some(NoteLine::Bullet(_)) | Some(NoteLine::Text(_))) {
            // (a bullet's next lines, indented)
            if let Some(NoteLine::Bullet(b) | NoteLine::Text(b)) = section.lines.last_mut() {
                b.push(' ');
                b.push_str(&plain(t));
            }
        } else {
            section.lines.push(NoteLine::Text(plain(t)));
        }
    }
    out.retain(|s| !s.lines.is_empty());
    out
}

/// The versions and the notes, from git (nothing downloaded: the game's own check fetched the new commits; update.ps1
/// fetches again). `from` given: kept (read again at the end, the install's own commit has moved on by then).
pub fn read_notes(repo: &Path, from: Option<String>) -> Notes {
    let prefix = if repo.join("port").is_dir() { "port/" } else { "" };
    let from = from.unwrap_or_else(|| {
        crate::built_commit(repo)
            .map(|c| crate::version_at(repo, &c).unwrap_or_else(|| c.chars().take(7).collect()))
            .unwrap_or_default()
    });
    let branch = crate::git_out(repo, &["rev-parse", "--abbrev-ref", "HEAD"]).ok().filter(|b| !b.is_empty() && b != "HEAD")
        .unwrap_or_else(|| "main".into());
    let target = crate::git_out(repo, &["rev-parse", "--verify", &format!("origin/{branch}")])
        .or_else(|_| crate::git_out(repo, &["rev-parse", "HEAD"]))
        .unwrap_or_default();
    let to = if target.is_empty() {
        String::new()
    } else {
        crate::version_at(repo, &target).unwrap_or_else(|| target.chars().take(7).collect())
    };
    let changelog = (!target.is_empty())
        .then(|| crate::git_out(repo, &["show", &format!("{target}:{prefix}CHANGELOG.md")]).ok())
        .flatten()
        .or_else(|| std::fs::read_to_string(crate::game_folder(repo).join("CHANGELOG.md")).ok())
        .unwrap_or_default();
    let sections = sections_between(&changelog, &from, &to);
    Notes { done: true, from, to, sections }
}

fn version_text(v: &str) -> String {
    if version_number(v).is_some() { format!("v{v}") } else { v.to_string() }
}

// ------------------------------------------------------------------------------------------------
// A stopped update: the plain reason, the report, the log.

/// The reason in plain words: update.ps1's own "update: <why>" line, else the last line that is not the log's or the
/// report's.
pub fn plain_error(error: &str) -> String {
    let lines: Vec<&str> = error.lines().map(str::trim).filter(|l| !l.is_empty()).collect();
    let why = lines.iter().rev().find_map(|l| l.strip_prefix("update: ")).map(str::to_string).or_else(|| {
        lines.iter().rev().find(|l| !l.starts_with("The full log") && !l.starts_with("A report to attach") && !l.starts_with('('))
            .map(|l| l.to_string())
    });
    // (the window shows no output above it: update.ps1's "(see above)" left out)
    let mut why = why.unwrap_or_else(|| "The update stopped.".into()).replace(" (see above)", "");
    if let Some(first) = why.chars().next() {
        why = first.to_uppercase().collect::<String>() + &why[first.len_utf8()..];
    }
    if !why.ends_with(['.', '!', '?']) {
        why.push('.');
    }
    why
}

/// update.ps1's line about the report ("A report to attach when you ask for help (Discord or GitHub) was saved ...").
pub fn report_line(error: &str) -> Option<String> {
    error.lines().map(str::trim).find(|l| l.starts_with("A report to attach")).map(str::to_string)
}

/// The report's file name in that line (tfa-report-<date>-<time>.txt).
pub fn report_name(error: &str) -> Option<String> {
    let at = error.find("tfa-report-")?;
    let rest = &error[at..];
    let end = rest.find(".txt")? + 4;
    let name = &rest[..end];
    name.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '.').then(|| name.to_string())
}

/// The full log's path the Job adds to every error ("The full log: <path>").
pub fn log_path(error: &str) -> Option<PathBuf> {
    error.lines().rev().find_map(|l| l.trim().strip_prefix("The full log: ")).map(|p| PathBuf::from(p.trim()))
}

/// Where tools\setup_report.ps1 may have put the report: TF_REPORT_DIR, the Desktop, the install's logs, the game folder.
fn find_report(name: &str, folder: &Path, repo: &Path) -> Option<PathBuf> {
    let mut dirs: Vec<PathBuf> = std::env::var("TF_REPORT_DIR").ok().filter(|d| !d.is_empty()).map(PathBuf::from).into_iter().collect();
    dirs.extend(crate::winapi::desktop_dir());
    dirs.extend([folder.join("logs"), crate::game_folder(repo), repo.to_path_buf(), folder.to_path_buf()]);
    dirs.into_iter().map(|d| d.join(name)).find(|p| p.is_file())
}

// ------------------------------------------------------------------------------------------------
// The window's state beside the App's (the update itself is the App's, as in the full window).

#[derive(Default)]
pub struct Compact {
    pub on: bool,
    pub notes: Notes,
    pub notes_rx: Option<std::sync::mpsc::Receiver<Notes>>,
    pub area: Option<[i32; 4]>, // (Windows) the work area of the monitor the game was on, pixels
    pub placing: u32,           // the frames the window is still being placed in
    pub lowered: bool,          // no longer on top (the game is starting)
    pub copied: bool,           // "Copy the path" pressed
    pub demo: Option<String>,   // (tests) TFA_COMPACT_DEMO: the window in a state of its own, no update run
}

impl App {
    /// The small window: borderless, 640 x 360 points (Windows scales it to the monitor), on top while the game is closed,
    /// placed in the middle of the game's monitor; its versions and notes read at once.
    pub fn compact_window(&mut self, offscreen: bool) -> eframe::NativeOptions {
        self.compact.on = true;
        if !offscreen {
            self.compact.area = crate::winapi::game_monitor_area(self.wait_pid);
        }
        self.start_notes(None);
        let mut viewport = egui::ViewportBuilder::default().with_inner_size([640.0, 360.0]).with_resizable(false).with_decorations(false)
            .with_always_on_top().with_title("Updating Transformers Armada");
        #[cfg(not(windows))]
        {
            viewport = viewport.with_app_id("tfa-setup");
        }
        if offscreen {
            viewport = viewport.with_position([-20000.0, -20000.0]).with_taskbar(false);
        }
        eframe::NativeOptions { viewport, ..Default::default() }
    }

    /// The versions and the patch notes, on a thread (git, quick, but not on the window's).
    pub fn start_notes(&mut self, from: Option<String>) {
        let (tx, rx) = std::sync::mpsc::channel();
        self.compact.notes_rx = Some(rx);
        let repo = self.repo();
        std::thread::spawn(move || {
            let _ = tx.send(read_notes(&repo, from));
        });
    }

    fn poll_notes(&mut self) {
        if let Some(rx) = &self.compact.notes_rx {
            if let Ok(n) = rx.try_recv() {
                // (read again at the end: a version found then replaces an unknown one, never the reverse)
                if self.compact.notes.done && n.sections.is_empty() && !self.compact.notes.sections.is_empty() {
                    self.compact.notes.to = if n.to.is_empty() { self.compact.notes.to.clone() } else { n.to };
                } else {
                    self.compact.notes = n;
                }
                self.compact.notes_rx = None;
            }
        }
    }

    /// (tests) TFA_COMPACT_DEMO=running|done|updated|failed: the window in that state with made-up steps, nothing run.
    pub fn compact_demo(&mut self, state: &str) {
        self.compact.demo = Some(state.to_string());
        self.update = true;
        self.update_step_texts();
        self.steps[STEP_DOWNLOAD].status = Status::Done;
        self.forecast = "This update rebuilds only the changed parts of the game: about 2 min on this PC.".into();
        match state {
            "failed" => {
                self.steps[STEP_GAME].status = Status::Failed;
                self.progress = 0.31;
                self.error = "building the game\nupdate: the build failed (see above)\nA report to attach when you ask for help (Discord or GitHub) \
                              was saved on your Desktop as tfa-report-20261123-120000.txt\n\nThe full log: C:\\Games\\TFA\\logs\\update-setup.log".into();
                self.phase = Phase::Done(false);
            }
            "done" | "updated" => {
                for s in &mut self.steps {
                    s.status = Status::Done;
                }
                self.progress = 1.0;
                self.phase = Phase::Done(true);
                self.play_started = state == "updated";
                self.done_at = Some(std::time::Instant::now() + std::time::Duration::from_secs(3600));
            }
            _ => {
                self.play = true; // (the choice shown, as in the game's own update)
                self.steps[STEP_GAME].status = Status::Active;
                self.steps[STEP_GAME].detail = "compiling the game: 120 of about 329 files, about 2 min left".into();
                self.steps[STEP_GAME].seconds = 74;
                self.progress = 0.47;
                self.phase = Phase::Running;
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// The menu's widgets, drawn the way menu_style.cpp draws them.

/// A box with its top-left and bottom-right corners cut off (the menu's panels, tabs and buttons).
fn chamfered(rect: Rect, cut: f32) -> Vec<Pos2> {
    let (a, b) = (rect.min, rect.max);
    vec![Pos2::new(a.x + cut, a.y), Pos2::new(b.x, a.y), Pos2::new(b.x, b.y - cut), Pos2::new(b.x - cut, b.y), Pos2::new(a.x, b.y),
         Pos2::new(a.x, a.y + cut)]
}

/// menu_style::button: the label upper case in the heading font; primary = Autobot red, the others outlined.
fn button(ui: &mut egui::Ui, label: &str, primary: bool) -> egui::Response {
    let galley = ui.painter().layout_no_wrap(label.to_uppercase(), heading_font(12.5), Color32::WHITE);
    let pad = Vec2::new(16.0, 9.0);
    let (rect, response) = ui.allocate_exact_size(galley.size() + pad * 2.0, Sense::click());
    let hovered = response.hovered();
    let cut = 6.0;
    let painter = ui.painter();
    if primary {
        painter.add(Shape::convex_polygon(chamfered(rect, cut), if hovered { RED_HOVER } else { RED }, Stroke::NONE));
    } else {
        painter.add(Shape::convex_polygon(chamfered(rect, cut), if hovered { PANEL_LIFT } else { BUTTON }, Stroke::NONE));
        painter.line_segment([Pos2::new(rect.min.x, rect.max.y - 1.0), Pos2::new(rect.max.x - cut, rect.max.y - 1.0)],
                             Stroke::new(2.0, if hovered { BLUE } else { LINE }));
    }
    painter.galley(rect.min + pad, galley, Color32::WHITE);
    response.on_hover_cursor(egui::CursorIcon::PointingHand)
}

/// menu_style::heading: a red bar, the words upper case in energon blue, a line to the right edge.
fn heading(ui: &mut egui::Ui, text: &str) {
    let galley = ui.painter().layout_no_wrap(text.to_uppercase(), heading_font(12.0), BLUE);
    let (rect, _) = ui.allocate_exact_size(Vec2::new(ui.available_width(), galley.size().y), Sense::hover());
    let painter = ui.painter();
    painter.rect_filled(Rect::from_min_size(rect.min, Vec2::new(4.0, rect.height())), 0.0, RED);
    let text_at = rect.min + Vec2::new(10.0, 0.0);
    let line_x = text_at.x + galley.size().x + 12.0;
    painter.galley(text_at, galley, BLUE);
    if line_x < rect.max.x {
        painter.line_segment([Pos2::new(line_x, rect.center().y), Pos2::new(rect.max.x, rect.center().y)], Stroke::new(1.0, LINE));
    }
}

/// A tick box in the menu's colours (an energon-blue mark).
fn tick_box(ui: &mut egui::Ui, on: &mut bool, label: &str) -> egui::Response {
    let galley = ui.painter().layout_no_wrap(label.to_string(), text_font(13.5), TEXT);
    let size = Vec2::new(16.0 + 8.0 + galley.size().x, galley.size().y.max(16.0));
    let (rect, mut response) = ui.allocate_exact_size(size, Sense::click());
    if response.clicked() {
        *on = !*on;
        response.mark_changed();
    }
    let painter = ui.painter();
    let mark = Rect::from_min_size(Pos2::new(rect.min.x, rect.center().y - 8.0), Vec2::splat(16.0));
    painter.rect_filled(mark, 2.0, FRAME);
    painter.rect_stroke(mark, 2.0, Stroke::new(1.0, if response.hovered() { BLUE } else { LINE }), egui::StrokeKind::Inside);
    if *on {
        painter.rect_filled(mark.shrink(4.0), 1.0, BLUE);
    }
    painter.galley(Pos2::new(mark.max.x + 8.0, rect.center().y - galley.size().y / 2.0), galley, TEXT);
    response.on_hover_cursor(egui::CursorIcon::PointingHand)
}

/// The progress bar: a dark frame, energon blue filling it, a red notch where it has got to.
fn progress_bar(ui: &mut egui::Ui, value: f32) {
    let (rect, _) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 10.0), Sense::hover());
    let painter = ui.painter();
    painter.rect_filled(rect, 2.0, FRAME);
    let w = rect.width() * value.clamp(0.0, 1.0);
    if w > 0.5 {
        painter.rect_filled(Rect::from_min_size(rect.min, Vec2::new(w, rect.height())), 2.0, BLUE);
        let notch = Rect::from_min_max(Pos2::new((rect.min.x + w - 3.0).max(rect.min.x), rect.min.y), Pos2::new(rect.min.x + w, rect.max.y));
        painter.rect_filled(notch, 0.0, RED);
    }
    painter.rect_stroke(rect, 2.0, Stroke::new(1.0, LINE), egui::StrokeKind::Inside);
}

/// The step the update is at, in plain words.
fn step_words(step: usize) -> &'static str {
    match step {
        STEP_DOWNLOAD => "Getting the new version",
        STEP_GAME => "Building the new version",
        STEP_HD => "Updating the HD textures",
        STEP_MODS => "Rebuilding the mods",
        _ => "Getting ready",
    }
}

fn duration_text(seconds: u64) -> String {
    if seconds >= 60 { format!("{} min {} s", seconds / 60, seconds % 60) } else { format!("{seconds} s") }
}

// ------------------------------------------------------------------------------------------------
// The window.

pub(crate) fn view(app: &mut App, ui: &mut egui::Ui) {
    app.poll_notes();
    let ctx = ui.ctx().clone();
    // (Windows) In the middle of the game's monitor: placed again over the first frames, as the move to another monitor
    // gives the window that monitor's scale (its size changes once more).
    if app.compact.placing == 1 {
        crate::winapi::square_own_window(); // (the menu's square corners, also on Windows 11)
    }
    if let Some(area) = app.compact.area {
        if app.compact.placing < 40 {
            app.compact.placing += 1;
            if !crate::winapi::centre_own_window(area) {
                ctx.request_repaint();
            }
        }
    } else if app.compact.placing < 2 {
        app.compact.placing += 1;
        ctx.request_repaint();
    }
    // On top while the game is closed; the game starting again: an ordinary window (the game comes in front).
    if app.play_started && matches!(app.phase, Phase::Done(_)) && !app.compact.lowered {
        app.compact.lowered = true;
        ctx.send_viewport_cmd(egui::ViewportCommand::WindowLevel(egui::WindowLevel::Normal));
    }

    let full = ui.max_rect();
    let painter = ui.painter().clone();
    painter.rect_filled(full, 0.0, PANEL);
    let mut y = full.min.y;
    while y < full.max.y {
        painter.line_segment([Pos2::new(full.min.x, y), Pos2::new(full.max.x, y)], Stroke::new(1.0, Color32::from_rgba_unmultiplied(255, 255, 255, 4)));
        y += 4.0;
    }
    painter.rect_stroke(full, 0.0, Stroke::new(1.0, LINE), egui::StrokeKind::Inside);

    let pad = 22.0;
    let inner = full.shrink(pad);
    let mut content = ui.new_child(egui::UiBuilder::new().max_rect(inner).layout(egui::Layout::top_down(egui::Align::Min)));
    let ui = &mut content;

    // The header: the title, the versions, the red rule (cut at the right, a blue end), as the menu's header.
    let header_top = ui.cursor().min;
    ui.label(egui::RichText::new("Updating Transformers Armada").font(title_font(27.0)).color(Color32::WHITE));
    let n = &app.compact.notes;
    let versions = match (n.from.is_empty(), n.to.is_empty()) {
        (false, false) if n.from != n.to => format!("{}  ->  {}", version_text(&n.from), version_text(&n.to)),
        (false, _) => version_text(&n.from),
        (true, false) => format!("->  {}", version_text(&n.to)),
        (true, true) => String::new(),
    };
    ui.label(egui::RichText::new(if versions.is_empty() { " ".to_string() } else { versions }).font(heading_font(12.0)).color(BLUE));
    ui.add_space(2.0);
    let rule_y = ui.cursor().min.y;
    let (l, r) = (inner.min.x, inner.max.x);
    painter.add(Shape::convex_polygon(vec![Pos2::new(l, rule_y), Pos2::new(r - 22.0, rule_y), Pos2::new(r - 30.0, rule_y + 3.0), Pos2::new(l, rule_y + 3.0)],
                                      RED, Stroke::NONE));
    painter.rect_filled(Rect::from_min_max(Pos2::new(r - 18.0, rule_y), Pos2::new(r, rule_y + 3.0)), 0.0, BLUE);
    // (a borderless window: moved by its header)
    let drag = ui.interact(Rect::from_min_max(Pos2::new(full.min.x, full.min.y), Pos2::new(full.max.x, rule_y + 3.0)),
                           ui.scope_id().with("compact_drag"), Sense::drag());
    if drag.drag_started() {
        ctx.send_viewport_cmd(egui::ViewportCommand::StartDrag);
    }
    let _ = header_top;
    ui.add_space(14.0);
    let body_top = ui.cursor().min;

    // The bottom part (progress, or the outcome and its buttons) is drawn from the bottom up; the notes fill the rest.
    let failed = app.phase == Phase::Done(false);
    let bottom = ui.with_layout(egui::Layout::bottom_up(egui::Align::Min), |ui| {
        match app.phase {
            Phase::Done(false) => failure_actions(app, ui),
            Phase::Done(true) => done_actions(app, ui),
            _ => running_status(app, ui),
        }
    });
    let notes_rect = Rect::from_min_max(body_top, Pos2::new(inner.max.x, bottom.response.rect.min.y - 16.0));
    let mut notes_ui = ui.new_child(egui::UiBuilder::new().max_rect(notes_rect).layout(egui::Layout::top_down(egui::Align::Min)));
    if failed {
        failure_text(app, &mut notes_ui);
    } else {
        patch_notes(app, &mut notes_ui);
    }
}

fn patch_notes(app: &App, ui: &mut egui::Ui) {
    heading(ui, "Patch notes");
    ui.add_space(2.0);
    let notes = &app.compact.notes;
    egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
        if !notes.done {
            ui.label(egui::RichText::new("Reading what is new...").color(DIM));
            return;
        }
        if notes.sections.is_empty() {
            ui.label(egui::RichText::new("No patch notes came with this update.").color(DIM));
            return;
        }
        let many = notes.sections.len() > 1;
        for section in &notes.sections {
            if many {
                let name = if version_number(&section.version).is_some() { format!("v{}", section.version) } else { section.version.clone() };
                ui.label(egui::RichText::new(name.to_uppercase()).font(heading_font(11.5)).color(AMBER));
            }
            for line in &section.lines {
                match line {
                    NoteLine::Heading(h) => {
                        ui.add_space(2.0);
                        ui.label(egui::RichText::new(h.to_uppercase()).font(heading_font(10.5)).color(DIM));
                    }
                    NoteLine::Bullet(b) => {
                        ui.horizontal_top(|ui| {
                            ui.spacing_mut().item_spacing.x = 8.0;
                            let (dot, _) = ui.allocate_exact_size(Vec2::new(6.0, 17.0), Sense::hover());
                            ui.painter().rect_filled(Rect::from_center_size(Pos2::new(dot.center().x, dot.min.y + 9.0), Vec2::splat(4.0)), 0.0, RED);
                            ui.add(egui::Label::new(egui::RichText::new(b).font(text_font(14.0)).color(TEXT)).wrap());
                        });
                    }
                    NoteLine::Text(t) => {
                        ui.add(egui::Label::new(egui::RichText::new(t).font(text_font(14.0)).color(TEXT)).wrap());
                    }
                }
            }
            ui.add_space(6.0);
        }
    });
}

/// While the update runs (drawn bottom up): the choice, the bar, the step.
fn running_status(app: &mut App, ui: &mut egui::Ui) {
    if app.play {
        let mut on = app.start_game;
        if tick_box(ui, &mut on, "Start the game when the update is done").changed() {
            app.start_game = on;
            app.choice_note = match crate::set_start_game_after_update(&app.repo(), on) {
                Ok(()) => String::new(),
                Err(e) => e,
            };
        }
        if !app.choice_note.is_empty() {
            ui.label(egui::RichText::new(&app.choice_note).color(RED).small());
        }
        ui.add_space(4.0);
    }
    progress_bar(ui, app.progress);
    let active = app.steps.iter().position(|s| s.status == Status::Active);
    let (mut words, mut detail, seconds) = match active {
        Some(i) => (step_words(i), app.steps[i].detail.clone(), app.steps[i].seconds),
        None => ("Getting ready", String::new(), 0),
    };
    if detail.to_ascii_lowercase().starts_with("waiting for the game to close") {
        (words, detail) = ("Waiting for the game to close", String::new());
    }
    // (bottom up: what is added last is on top - on screen the forecast, the step, its detail, the bar, the choice)
    if !detail.is_empty() {
        let short: String = detail.chars().take(90).collect();
        ui.label(egui::RichText::new(short).color(DIM).small());
    }
    ui.horizontal(|ui| {
        ui.label(egui::RichText::new(words).font(text_font(16.0)).color(Color32::WHITE));
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
            ui.label(egui::RichText::new(format!("{:.0}%", app.progress * 100.0)).font(heading_font(12.0)).color(BLUE));
            if seconds > 0 {
                ui.label(egui::RichText::new(duration_text(seconds)).color(DIM).small());
            }
        });
    });
    if !app.forecast.is_empty() {
        let colour = if app.forecast.contains(" most of the game") { AMBER } else { DIM };
        ui.add(egui::Label::new(egui::RichText::new(&app.forecast).color(colour).small()).wrap());
    }
}

/// The update done: the game starting (the window closes by itself), or "Update done" with Play and Close.
fn done_actions(app: &mut App, ui: &mut egui::Ui) {
    if app.play_started {
        ui.label(egui::RichText::new("The game is starting again.").color(DIM));
        ui.label(egui::RichText::new("Update done").font(text_font(18.0)).color(BLUE));
        return;
    }
    let mut close = false;
    ui.horizontal(|ui| {
        if button(ui, "Play", true).clicked() {
            crate::play(&app.repo());
            close = true;
        }
        if button(ui, "Close", false).clicked() {
            close = true;
        }
    });
    if close {
        ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
    }
    ui.add_space(4.0);
    if !app.kept.is_empty() {
        ui.add(egui::Label::new(egui::RichText::new(crate::kept_text(&app.kept)).color(BLUE).small()).wrap());
    }
    ui.label(egui::RichText::new("The game was not started (\"Start the game when the update is done\" is unticked).").color(DIM).small());
    ui.label(egui::RichText::new("Update done").font(text_font(18.0)).color(BLUE));
}

/// A stopped update: the reason, what to do (drawn in the notes' place).
fn failure_text(app: &App, ui: &mut egui::Ui) {
    heading(ui, "The update stopped");
    ui.add_space(2.0);
    egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
        ui.add(egui::Label::new(egui::RichText::new(plain_error(&app.error)).font(text_font(15.0)).color(Color32::WHITE)).wrap());
        ui.add_space(4.0);
        if app.play_started {
            ui.add(egui::Label::new(egui::RichText::new("The game starts again as it was; you can try the update again later.").color(DIM)).wrap());
        }
        if !app.kept.is_empty() {
            ui.add(egui::Label::new(egui::RichText::new(crate::kept_text(&app.kept)).color(BLUE).small()).wrap());
        }
        let (report, log) = failure_files(app);
        match (&report, report_line(&app.error)) {
            (Some(_), Some(line)) => {
                ui.add(egui::Label::new(egui::RichText::new(line).color(AMBER)).wrap());
                ui.add(egui::Label::new(egui::RichText::new("It holds no game files, and your user name and your PC's name are taken out.").color(DIM).small()).wrap());
            }
            _ => {
                ui.add(egui::Label::new(egui::RichText::new("To ask for help (Discord or GitHub), attach the update's log:").color(AMBER)).wrap());
            }
        }
        // (the file's path, unless the report's line says it whole already)
        let line = report_line(&app.error).unwrap_or_default();
        if let Some(p) = report.or(log).filter(|p| !line.contains(&p.display().to_string())) {
            ui.label(egui::RichText::new(p.display().to_string()).color(DIM).small());
        }
    });
}

/// The report (else the log) a stopped update left.
fn failure_files(app: &App) -> (Option<PathBuf>, Option<PathBuf>) {
    let report = report_name(&app.error).and_then(|name| {
        if app.compact.demo.is_some() {
            return Some(crate::winapi::desktop_dir().unwrap_or_default().join(&name));
        }
        find_report(&name, Path::new(&app.folder), &app.repo())
    });
    (report, log_path(&app.error))
}

/// A stopped update's buttons: Open the report (else the log), Copy the path, Try again, Close.
fn failure_actions(app: &mut App, ui: &mut egui::Ui) {
    let (report, log) = failure_files(app);
    let file = report.clone().or(log);
    let mut close = false;
    let mut again = false;
    ui.horizontal(|ui| {
        if let Some(f) = &file {
            let label = if report.is_some() { "Open the report" } else { "Open the log" };
            if button(ui, label, true).clicked() {
                crate::winapi::open_folder(f);
            }
            if button(ui, if app.compact.copied { "Path copied" } else { "Copy the path" }, false).clicked() {
                ui.ctx().copy_text(f.display().to_string());
                app.compact.copied = true;
            }
        }
        if button(ui, "Try again", false).clicked() {
            again = true;
        }
        if button(ui, "Close", false).clicked() {
            close = true;
        }
    });
    if close {
        ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
    }
    if again && app.compact.demo.is_none() {
        // (as the full window's Try again: the game started again after the failure is not waited for)
        app.error.clear();
        app.compact.copied = false;
        app.compact.lowered = false;
        for s in &mut app.steps {
            if s.status != Status::Skipped {
                s.status = Status::Pending;
            }
        }
        app.play = false;
        app.play_started = false;
        app.wait_pid = 0;
        ui.ctx().send_viewport_cmd(egui::ViewportCommand::WindowLevel(egui::WindowLevel::AlwaysOnTop));
        app.start_update();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const LOG: &str = "# What's new\n\n## Unreleased\n\n**Fixed**\n- The dev fix.\n\n## 0.9.9\n\n**New**\n- Updating shows a small window.\n- A long\n  line that goes on.\n\n## 0.9.8\n\n**Fixed**\n- Haze on far trees.\n\n## 0.9.7\n\n**Fixed**\n- Old one.\n";

    #[test]
    fn the_notes_between_two_versions() {
        let s = sections_between(LOG, "0.9.7", "0.9.9");
        assert_eq!(s.iter().map(|s| s.version.as_str()).collect::<Vec<_>>(), ["0.9.9", "0.9.8"]);
        assert_eq!(s[0].lines, vec![NoteLine::Heading("New".into()), NoteLine::Bullet("Updating shows a small window.".into()),
                                    NoteLine::Bullet("A long line that goes on.".into())]);
    }

    #[test]
    fn a_development_copy_shows_unreleased() {
        let s = sections_between(LOG, "0.9.8", "1cae8c8");
        assert_eq!(s.iter().map(|s| s.version.as_str()).collect::<Vec<_>>(), ["Unreleased", "0.9.9"]);
        assert_eq!(sections_between(LOG, "", "0.9.8").len(), 1);
        assert!(sections_between(LOG, "0.9.9", "0.9.9").is_empty());
    }

    #[test]
    fn a_stopped_update_said_plainly() {
        let e = "==> Rebuilding\nupdate: the build failed (see above)\nA report to attach when you ask for help (Discord or GitHub) was saved on your \
                 Desktop as tfa-report-20261123-120000.txt\n\nThe full log: C:\\Games\\TFA\\logs\\update-setup.log";
        assert_eq!(plain_error(e), "The build failed.");
        assert_eq!(report_name(e).as_deref(), Some("tfa-report-20261123-120000.txt"));
        assert_eq!(log_path(e), Some(PathBuf::from("C:\\Games\\TFA\\logs\\update-setup.log")));
        assert!(report_line(e).unwrap().starts_with("A report to attach"));
        assert_eq!(plain_error("git could not be started\n\nThe full log: x"), "Git could not be started.");
    }
}
