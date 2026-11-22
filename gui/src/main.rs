mod desktop;
mod model;
mod devices;
mod preview;
mod theme;

use gpui::{
    Bounds, ClickEvent, Context, FocusHandle, FontWeight, IntoElement, RenderImage, ObjectFit, img,
    Window, deferred, div, prelude::*,
    px, rgb, size, canvas, MouseButton,
};
use model::{Description, FrameGenInfo, Mode};
use std::{
    collections::{HashMap, VecDeque},
    io::{BufRead, BufReader},
    path::PathBuf,
    process::{Child, Command, Stdio},
    sync::{Arc, atomic::Ordering, mpsc::{self, Receiver, Sender}},
    thread,
    time::{Duration, Instant},
};

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Capture,
    Viewer,
    Settings,
    Console,
    About,
}
#[derive(Clone, Copy, PartialEq)]
enum Field {
    Video,
    Mode,
    Input,
    Output,
    VirtualDevices,
    Latency,
    Pacing,
    Theme,
    Volume,
    FrameGeneration,
}
#[derive(Clone, Copy)]
enum Toggle {
    Tray,
    Preview,
    Audio,

    Mute,
}
#[derive(Clone, Copy)]
enum Action {
    Page(Page),
    Refresh,
    Start,
    Stop,
    Toggle(Toggle),
    Clear,
    CopyLogs,
    OpenRepo,
    OpenConfig,
    Fullscreen,
    OpenNvidiaLicense,
    InstallNvidiaRuntime,
    RemoveNvidiaRuntime,
}
#[derive(Clone, Copy)]
enum Kind {
    Primary,
    Secondary,
    Outline,
}
const REPO_URL: &str = "https://github.com/D35P4C1T0/capture-view";
const NVIDIA_LICENSE_URL: &str =
    "https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt";
const VERSION: &str = "v0.2.1";
fn clock() -> String {
    // SAFETY: localtime_r only writes into the zeroed tm we own.
    unsafe {
        let now = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&now, &mut tm);
        format!("{:02}:{:02}:{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec)
    }
}
fn log_level(line: &str) -> (&'static str, u32) {
    let lower = line.to_ascii_lowercase();
    if lower.contains("error") || lower.contains("fail") || lower.contains("could not") {
        ("ERROR", 0xf87171)
    } else if lower.contains("warn") {
        ("WARN", 0xfbbf24)
    } else {
        ("INFO", 0x4ade80)
    }
}
#[derive(Clone)]
struct Choice {
    value: String,
    label: String,
}
fn choices(values: &[(&str, &str)]) -> Vec<Choice> {
    values
        .iter()
        .map(|(value, label)| Choice {
            value: (*value).into(),
            label: (*label).into(),
        })
        .collect()
}
// A queued startup event must not orphan a capture process if the GUI closes
// before consuming it. Ownership transfers only when the UI accepts the event.
struct StartedViewer { child: Option<Child>, preview_file: Option<PathBuf> }
impl Drop for StartedViewer {
    fn drop(&mut self) {
        if let Some(mut child) = self.child.take() { stop_child(&mut child); }
        if let Some(path) = self.preview_file.take() { let _=std::fs::remove_file(path); }
    }
}

enum Event {
    Discovery(Result<Description, String>),
    TrayReady(u64, Result<desktop::Tray, String>),
    Started(StartedViewer, u32),
    SaveFailed(String),
    Error(String),
    Log(String),
    FrameGenInfo(Result<FrameGenInfo, String>),
    RuntimeProgress(u64, u64),
    RuntimeFinished(Result<(), String>),
}

// Serialize background saves so an older snapshot cannot overwrite a newer one.
// Closing the sender flushes queued changes before the worker exits.
struct SettingsSaver {
    tx: Option<Sender<Vec<String>>>,
    join: Option<thread::JoinHandle<()>>,
}
impl SettingsSaver {
    fn new(engine: PathBuf, events: Sender<Event>) -> Self {
        let (tx, rx) = mpsc::channel::<Vec<String>>();
        let join = thread::spawn(move || {
            while let Ok(mut args) = rx.recv() {
                for latest in rx.try_iter() { args = latest; }
                let error = match Command::new(&engine).args(args).arg("--save-only").output() {
                    Ok(output) if output.status.success() => None,
                    Ok(output) => Some(String::from_utf8_lossy(&output.stderr).trim().to_owned()),
                    Err(error) => Some(error.to_string()),
                };
                if let Some(error) = error {
                    let _ = events.send(Event::SaveFailed(format!("Could not save settings: {error}")));
                }
            }
        });
        Self { tx: Some(tx), join: Some(join) }
    }
    fn save(&self, args: Vec<String>) {
        if let Some(tx) = &self.tx { let _ = tx.send(args); }
    }
}
impl Drop for SettingsSaver {
    fn drop(&mut self) {
        self.tx.take();
        if let Some(join) = self.join.take() { let _ = join.join(); }
    }
}

struct CaptureView {
    engine: PathBuf,
    settings_saver: SettingsSaver,
    desktop_commands: Sender<desktop::Command>,
    tray: Option<desktop::Tray>,
    tray_requested: bool,
    tray_generation: u64,
    window_visible: bool,
    incoming: Vec<String>,
    description: Option<Description>,
    page: Page,
    theme: usize,
    open: Option<Field>,
    popup_index: usize,
    dropdown_bounds: HashMap<usize, Bounds<gpui::Pixels>>,
    viewer_area: gpui::Size<gpui::Pixels>,
    fullscreen_mode: bool,
    fullscreen_activity: Instant,
    fullscreen_opacity: f32,
    fullscreen_pointer: Option<gpui::Point<gpui::Pixels>>,
    focus: FocusHandle,
    status: String,
    discovering: bool,
    pending: bool,
    viewer: Option<Child>,
    preview: Option<preview::Worker>,
    preview_image: Option<Arc<RenderImage>>,
    retired_image: Option<Arc<RenderImage>>,
    preview_size: Option<(u32,u32)>,
    preview_rates: Option<(f32,f32)>,
    active_outputs: u32,
    logs: VecDeque<(String, String)>,
    frame_gen: Option<FrameGenInfo>,
    frame_gen_checking: bool,
    runtime_busy: Option<(u64, u64)>,
    tx: Sender<Event>,
    rx: Receiver<Event>,
}

fn engine_path() -> PathBuf {
    if let Some(path) = std::env::var_os("CAPTURE_VIEW_ENGINE") {
        return path.into();
    }
    let sibling = std::env::current_exe()
        .unwrap_or_default()
        .with_file_name("capture-view");
    if sibling.is_file() {
        sibling
    } else {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("build/capture-view")
    }
}

impl CaptureView {
    fn new(window: &mut Window, cx: &mut Context<Self>, desktop_commands: Sender<desktop::Command>) -> Self {
        let (tx, rx) = mpsc::channel();
        let engine = engine_path();
        let settings_saver = SettingsSaver::new(engine.clone(), tx.clone());
        let focus = cx.focus_handle();
        focus.focus(window);
        let mut this = Self {
            engine,
            settings_saver,
            desktop_commands,
            tray: None,
            tray_requested: false,
            tray_generation: 0,
            window_visible: true,
            incoming: std::env::args().skip(1).collect(),
            description: None,
            page: Page::Capture,
            theme: 0,
            open: None,
            popup_index: 0,
            dropdown_bounds: HashMap::new(),
            viewer_area: size(px(0.), px(0.)),
            fullscreen_mode: false,
            fullscreen_activity: Instant::now(),
            fullscreen_opacity: 1.0,
            fullscreen_pointer: None,
            focus,
            status: "Finding capture devices…".into(),
            discovering: false,
            pending: false,
            viewer: None,
            preview: None,
            preview_image: None,
            retired_image: None,
            preview_size: None,
            preview_rates: None,
            active_outputs: 0,
            logs: VecDeque::new(),
            frame_gen: None,
            frame_gen_checking: false,
            runtime_busy: None,
            tx,
            rx,
        };
        this.refresh();
        this.check_frame_generation();
        this
    }
    // Asks the engine what DLSS Frame Generation this machine supports. Creates
    // a short-lived Vulkan device, so it runs off the UI thread.
    fn check_frame_generation(&mut self) {
        if self.frame_gen_checking { return; }
        self.frame_gen_checking = true;
        let (engine, tx) = (self.engine.clone(), self.tx.clone());
        thread::spawn(move || {
            let result = Command::new(engine)
                .arg("--frame-gen-info")
                .stdin(Stdio::null())
                .output()
                .map_err(|e| format!("Could not start the capture engine: {e}"))
                .and_then(|o| {
                    let text = String::from_utf8_lossy(&o.stdout);
                    let line = text.lines().rev().find(|l| l.trim_start().starts_with('{')).unwrap_or("");
                    serde_json::from_str::<FrameGenInfo>(line)
                        .map_err(|e| format!("Could not read frame generation support: {e}"))
                });
            let _ = tx.send(Event::FrameGenInfo(result));
        });
    }
    fn install_runtime(&mut self, remove: bool) {
        if self.runtime_busy.is_some() { return; }
        self.runtime_busy = Some((0, 0));
        self.status = if remove { "Removing NVIDIA runtime…".into() } else { "Downloading NVIDIA runtime…".into() };
        let (engine, tx) = (self.engine.clone(), self.tx.clone());
        thread::spawn(move || {
            let flag = if remove { "--remove-nvidia-runtime" } else { "--install-nvidia-runtime" };
            let child = Command::new(engine).arg(flag).stdin(Stdio::null()).stdout(Stdio::piped())
                .stderr(Stdio::null()).spawn();
            let mut child = match child {
                Ok(child) => child,
                Err(e) => { let _ = tx.send(Event::RuntimeFinished(Err(format!("Could not start the engine: {e}")))); return; }
            };
            let mut outcome = Err("The download did not finish".to_string());
            if let Some(stdout) = child.stdout.take() {
                for line in BufReader::new(stdout).lines().map_while(Result::ok) {
                    if let Some(rest) = line.strip_prefix("progress ") {
                        let mut parts = rest.split_whitespace().filter_map(|v| v.parse::<u64>().ok());
                        if let (Some(done), Some(total)) = (parts.next(), parts.next()) {
                            let _ = tx.send(Event::RuntimeProgress(done, total));
                        }
                    } else if line == "ok" {
                        outcome = Ok(());
                    } else if let Some(error) = line.strip_prefix("error ") {
                        outcome = Err(error.to_string());
                    }
                }
            }
            let _ = child.wait();
            let _ = tx.send(Event::RuntimeFinished(outcome));
        });
    }
    fn log(&mut self, line: String) {
        self.logs.push_back((clock(), line));
        while self.logs.len() > 400 {
            self.logs.pop_front();
        }
    }
    fn refresh(&mut self) {
        if self.discovering {
            return;
        }
        self.discovering = true;
        self.status = "Refreshing devices…".into();
        let (engine, args, tx) = (self.engine.clone(), self.incoming.clone(), self.tx.clone());
        thread::spawn(move || {
            let result = Command::new(engine)
                .args(args)
                .arg("--gui-info")
                .output()
                .map_err(|e| format!("Could not start the capture engine: {e}"))
                .and_then(|o| {
                    if o.status.success() {
                        serde_json::from_slice(&o.stdout)
                            .map_err(|e| format!("Could not read device information: {e}"))
                    } else {
                        Err(String::from_utf8_lossy(&o.stderr).trim().to_owned())
                    }
                });
            let _ = tx.send(Event::Discovery(result));
        });
    }
    fn poll(&mut self, cx: &mut Context<Self>) -> bool {
        let mut changed = false;
        while let Ok(event) = self.rx.try_recv() {
            changed = true;
            match event {
                Event::Discovery(result) => {
                    self.discovering = false;
                    match result {
                        Ok(mut d) => {
                            if let Some(old) = &self.description {
                                d.settings = old.settings.clone();
                            } else {
                                let path = PathBuf::from(&d.config_path).with_extension("ui.json");
                                if let Ok(bytes) = std::fs::read(path) {
                                    if let Ok(index) = serde_json::from_slice::<usize>(&bytes) {
                                        self.theme = index.min(theme::NAMES.len() - 1);
                                    }
                                }
                            }
                            self.status = if d.audio_error.is_empty() {
                                format!("{} capture devices available", d.video_devices.len())
                            } else {
                                format!("Audio discovery: {}", d.audio_error)
                            };
                            self.description = Some(d);
                        }
                        Err(error) => {
                            self.status = error.clone();
                            self.log(error);
                        }
                    }
                }
                Event::TrayReady(generation, result) => {
                    if generation == self.tray_generation && self.tray_requested {
                        match result {
                            Ok(tray) => self.tray = Some(tray),
                            Err(error) => {
                                self.status = format!("System tray unavailable: {error}");
                                self.log(self.status.clone());
                            }
                        }
                    }
                }
                Event::Started(mut startup, outputs) => {
                    self.pending = false;
                    self.active_outputs = outputs;
                    self.viewer = startup.child.take();
                    startup.preview_file.take();
                    self.status = "Viewer running".into();
                }
                Event::SaveFailed(error) => {
                    self.status = error.clone();
                    self.log(error);
                }
                Event::Error(error) => {
                    self.clear_preview();
                    self.pending = false;
                    self.status = error.clone();
                    self.log(error);
                }
                Event::Log(line) => self.log(line),
                Event::FrameGenInfo(result) => {
                    self.frame_gen_checking = false;
                    match result {
                        Ok(info) => {
                            if let Some(d) = &mut self.description {
                                let max = if info.available { info.max_multiplier.max(1) } else { 1 };
                                if d.settings.frame_generation > max && info.runtime_installed {
                                    d.settings.frame_generation = max;
                                }
                            }
                            self.frame_gen = Some(info);
                        }
                        Err(error) => self.log(error),
                    }
                }
                Event::RuntimeProgress(done, total) => self.runtime_busy = Some((done, total)),
                Event::RuntimeFinished(result) => {
                    self.runtime_busy = None;
                    match result {
                        Ok(()) => {
                            self.status = "NVIDIA runtime updated".into();
                            self.log(self.status.clone());
                        }
                        Err(error) => {
                            self.status = format!("NVIDIA runtime: {error}");
                            self.log(self.status.clone());
                        }
                    }
                    self.check_frame_generation();
                }
            }
        }
        self.sync_tray();
        if let Some(child) = &mut self.viewer {
            match child.try_wait() {
                Ok(Some(exit)) => {
                    self.viewer = None;
                    self.clear_preview();
                    self.status = if exit.success() {
                        "Viewer closed".into()
                    } else {
                        "Viewer stopped — see Console for details".into()
                    };
                    changed = true;
                }
                Err(e) => {
                    self.status = format!("Could not check viewer: {e}");
                    changed = true;
                }
                _ => {}
            }
        }
        if self.fullscreen_mode {
            let elapsed = self.fullscreen_activity.elapsed().as_secs_f32();
            let opacity = (1.0 - (elapsed - 2.0).max(0.0) / 0.25).clamp(0.0, 1.0);
            if self.fullscreen_opacity != opacity {
                self.fullscreen_opacity = opacity;
                changed = true;
            }
        }
        if let Some(old) = self.retired_image.take() { cx.drop_image(old, None); }
        let preview_visible = self.window_visible && self.page == Page::Viewer && self.show_preview();
        if let Some(worker) = &self.preview {
            worker.visible.store(preview_visible, Ordering::Relaxed);
            if let Some(error) = worker.error() { self.status = error; changed = true; }
            if preview_visible {
                if let Some(frame) = worker.take() {
                    self.preview_size = Some((frame.width, frame.height));
                    if frame.output_fps > 0.0 { self.preview_rates = Some((frame.input_fps, frame.output_fps)); }
                    if let Some(buffer) = image::RgbaImage::from_raw(frame.width, frame.height, frame.pixels) {
                        let image = Arc::new(RenderImage::new(vec![image::Frame::new(buffer)]));
                        if let Some(old) = self.preview_image.replace(image) { cx.drop_image(old, None); }
                        changed = true;
                    }
                }
            }
        }
        changed
    }
    fn fullscreen_interaction(&mut self) {
        self.fullscreen_activity = Instant::now();
        self.fullscreen_opacity = 1.0;
    }
    fn clear_preview(&mut self) {
        self.preview.take();
        if let Some(old) = self.preview_image.take() { self.retired_image = Some(old); }
        self.preview_size = None;
        self.preview_rates = None;
        self.active_outputs = 0;
    }
    fn tray_available(&self) -> bool {
        self.description.as_ref().is_some_and(|d|d.settings.system_tray)
            && self.tray.as_ref().is_some_and(|tray|tray.available())
    }
    fn sync_tray(&mut self) {
        let enabled = self.description.as_ref().is_some_and(|d|d.settings.system_tray);
        if enabled == self.tray_requested { return; }
        self.tray_requested = enabled;
        self.tray_generation = self.tray_generation.wrapping_add(1);
        if enabled {
            let generation = self.tray_generation;
            let (commands, events) = (self.desktop_commands.clone(), self.tx.clone());
            thread::spawn(move || { let _ = events.send(Event::TrayReady(generation, desktop::Tray::new(commands))); });
        } else {
            self.tray.take();
        }
    }
    fn show_preview(&self) -> bool {
        self.description.as_ref().map_or(true, |d| d.settings.show_preview)
    }
    fn update_preview_visibility(&mut self) {
        let visible = self.window_visible && self.page == Page::Viewer && self.show_preview();
        if let Some(worker) = &self.preview {
            worker.visible.store(visible, Ordering::Relaxed);
            if !visible { worker.take(); }
        }
        if !self.show_preview() {
            if let Some(old) = self.preview_image.take() { self.retired_image = Some(old); }
        }
    }
    fn set_page(&mut self, page: Page) {
        self.page = page;
        self.update_preview_visibility();
    }
    fn stop(&mut self) {
        if let Some(mut viewer) = self.viewer.take() { stop_child(&mut viewer); }
        self.clear_preview();
        self.status = "Viewer stopped".into();
    }
    fn save_settings(&self) {
        if let Some(d) = &self.description { self.settings_saver.save(d.viewer_args()); }
    }
    fn start_viewer(&mut self) {
        if self.pending || self.discovering { return; }
        let Some(d) = &self.description else { return; };
        if !d.arguments.iter().any(|a| a == "--test-pattern") &&
            !d.video_devices.iter().any(|v| v.path == d.settings.video) {
            self.status = "Choose an available capture device first".into(); return;
        }
        let (engine, args, tx, count) = (self.engine.clone(), d.viewer_args(), self.tx.clone(), d.settings.virtual_devices);
        self.save_settings();
        let preview_file = {
            self.stop();
            let directory = std::env::var_os("XDG_RUNTIME_DIR").map(PathBuf::from).unwrap_or_else(std::env::temp_dir);
            let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_nanos();
            let path = directory.join(format!("capture-view-{}-{stamp}.frames", std::process::id()));
            let worker = preview::Worker::new(path.clone());
            worker.visible.store(self.window_visible && self.page == Page::Viewer && self.show_preview(), Ordering::Relaxed);
            self.preview = Some(worker); path
        };
        self.pending = true;
        self.status = "Starting viewer and preparing virtual cameras…".into();
        thread::spawn(move || {
            let outputs = match devices::ensure_count(&engine, count) {
                Ok(paths) => paths,
                Err(error) => {let _=tx.send(Event::Error(error));return;}
            };
            let mut runtime = args;
            let path = preview_file;
            // The GUI owns persistence; the running engine must not save an old
            // settings snapshot when it exits.
            runtime.push("--no-config".into());
            runtime.extend(["--preview-file".into(), path.to_string_lossy().into_owned(),
                "--video-outputs".into(), outputs.join(","),
                "--audio-virtual-source".into(), if count > 0 {"Capture View Audio".into()} else {String::new()}]);
            if count > 0 {
                let labels=(1..=count).map(|i|format!("Capture View {i}")).collect::<Vec<_>>().join(", ");
                let _=tx.send(Event::Log(format!("Virtual cameras ready: {labels}. Audio source: Capture View Audio")));
            }
            match Command::new(engine).args(runtime).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::piped()).spawn() {
                Ok(mut child) => {
                    if let Some(stderr) = child.stderr.take() {
                        let logs=tx.clone(); thread::spawn(move || {
                            for line in BufReader::new(stderr).lines().map_while(Result::ok) {
                                if logs.send(Event::Log(line)).is_err() {break;}
                            }
                        });
                    }
                    let startup = StartedViewer { child: Some(child), preview_file: Some(path) };
                    let _=tx.send(Event::Started(startup, count));
                },
                Err(e) => {let _=tx.send(Event::Error(format!("Could not start viewer: {e}")));}
            }
        });
    }
    fn act(&mut self, action: Action) {
        self.open = None;
        match action {
            Action::Page(page) => self.set_page(page),
            Action::Refresh => self.refresh(),
            Action::Start => self.start_viewer(),
            Action::Stop => self.stop(),
            Action::Clear => self.logs.clear(),
            // These need the window or app context and are handled in `button`.
            Action::Fullscreen | Action::CopyLogs | Action::OpenRepo | Action::OpenConfig | Action::OpenNvidiaLicense => {},
            Action::InstallNvidiaRuntime => self.install_runtime(false),
            Action::RemoveNvidiaRuntime => self.install_runtime(true),
            Action::Toggle(toggle) => {
                if let Some(d) = &mut self.description {
                    let value = match toggle {
                        Toggle::Tray => &mut d.settings.system_tray,
                        Toggle::Preview => &mut d.settings.show_preview,
                        Toggle::Audio => &mut d.settings.audio_monitor,
                        Toggle::Mute => &mut d.settings.muted,
                    };
                    *value = !*value;
                }
                if matches!(toggle, Toggle::Preview) {
                    // A running engine stops (or resumes) the preview copy at once.
                    let show = self.show_preview();
                    let failure = self.preview.as_ref()
                        .and_then(|worker| preview::set_wanted(&worker.path, show).err());
                    if let (Some(error), true) = (failure, self.viewer.is_some()) {
                        self.log(format!("Could not update the preview: {error}"));
                    }
                }
                self.update_preview_visibility();
                self.sync_tray();
                self.save_settings();
            }
        }
    }
    fn set(&mut self, field: Field, value: &str) {
        self.open = None;
        if field == Field::Theme {
            self.theme = value
                .parse::<usize>()
                .unwrap_or(0)
                .min(theme::NAMES.len() - 1);
            let mut failure = None;
            if let Some(d) = &self.description {
                if !d.arguments.iter().any(|a| a == "--no-config") {
                    let path = PathBuf::from(&d.config_path).with_extension("ui.json");
                    let tmp = path.with_extension("ui.json.tmp");
                    let result = (|| -> std::io::Result<()> {
                        if let Some(parent) = path.parent() {
                            std::fs::create_dir_all(parent)?;
                        }
                        std::fs::write(&tmp, self.theme.to_string())?;
                        std::fs::rename(&tmp, &path)
                    })();
                    if let Err(e) = result {
                        failure = Some(format!("Could not save theme: {e}"));
                    }
                }
            }
            if let Some(error) = failure {
                self.status = error.clone();
                self.log(error);
            }
            return;
        }
        let Some(d) = &mut self.description else {
            return;
        };
        match field {
            Field::Video => {
                d.settings.video = value.into();
                if let Some(mode) = d.modes().first().cloned() {
                    apply_mode(d, &mode);
                }
            }
            Field::Mode => {
                if value == "auto" {
                    d.settings.format = "auto".into();
                } else if let Some(mode) = d.modes().iter().find(|m| m.key() == value).cloned() {
                    apply_mode(d, &mode);
                }
            }
            Field::Input => d.settings.audio_input = value.into(),
            Field::Output => d.settings.audio_output = value.into(),
            Field::VirtualDevices => d.settings.virtual_devices = value.parse::<u32>().unwrap_or(0).min(8),
            Field::Latency => d.settings.latency = value.into(),
            Field::Pacing => d.settings.pacing = value.into(),
            Field::Volume => d.settings.volume = value.parse().unwrap_or(1.0),
            Field::FrameGeneration => d.settings.frame_generation = value.parse::<u32>().unwrap_or(1).clamp(1, 6),
            Field::Theme => {}
        }
        self.save_settings();
    }
    fn field_choices(&self, field: Field) -> Vec<Choice> {
        if field == Field::Theme {
            return theme::NAMES
                .iter()
                .enumerate()
                .map(|(i, n)| Choice {
                    value: i.to_string(),
                    label: (*n).into(),
                })
                .collect();
        }
        let Some(d) = &self.description else {
            return vec![];
        };
        match field {
            Field::Video => d
                .video_devices
                .iter()
                .enumerate()
                .map(|(i, v)| {
                    let base = v.label();
                    let duplicate =
                        d.video_devices.iter().filter(|v| v.label() == base).count() > 1;
                    Choice {
                        value: v.path.clone(),
                        label: if duplicate {
                            format!("{base} ({})", i + 1)
                        } else {
                            base
                        },
                    }
                })
                .collect(),
            Field::Mode => {
                let mut list = choices(&[("auto", "Automatic (recommended)")]);
                list.extend(d.modes().into_iter().map(|m| Choice {
                    value: m.key(),
                    label: m.label(),
                }));
                list
            }
            Field::Input | Field::Output => {
                let kind = if field == Field::Input {
                    "Audio/Source"
                } else {
                    "Audio/Sink"
                };
                let mut list = choices(&[("default", "System default")]);
                list.extend(
                    d.audio_devices
                        .iter()
                        .filter(|v| v.kind == kind)
                        .map(|v| Choice {
                            value: v.name.clone(),
                            label: v.label.clone(),
                        }),
                );
                list
            }
            Field::VirtualDevices => (0..=8).map(|n|Choice{value:n.to_string(),label:match n {
                0=>"Off — preview only".into(),1=>"1 virtual camera".into(),_=>format!("{n} virtual cameras")
            }}).collect(),
            Field::Latency => choices(&[
                ("ultra", "Ultra low latency"),
                ("low", "Low latency"),
                ("balanced", "Balanced"),
            ]),
            Field::Pacing => choices(&[
                ("immediate", "As soon as available"),
                ("yield", "Responsive"),
                ("sleep", "Power saving"),
                ("adaptive", "Automatic"),
            ]),
            Field::Volume => choices(&[
                ("0.25", "25%"),
                ("0.5", "50%"),
                ("0.75", "75%"),
                ("1", "100%"),
                ("1.25", "125%"),
                ("1.5", "150%"),
            ]),
            Field::FrameGeneration => {
                let mut list = choices(&[("1", "Off")]);
                let max = self.frame_gen.as_ref()
                    .filter(|f| f.available && f.runtime_installed)
                    .map_or(1, |f| f.max_multiplier.clamp(1, 6));
                // Keep the saved choice visible even before support is known.
                let max = max.max(d.settings.frame_generation.min(6));
                for n in 2..=max {
                    let label = if n >= 5 { format!("{n}x (experimental)") } else { format!("{n}x") };
                    list.push(Choice { value: n.to_string(), label });
                }
                list
            }
            Field::Theme => vec![],
        }
    }
    fn value(&self, field: Field) -> String {
        if field == Field::Theme {
            return self.theme.to_string();
        }
        let Some(d) = &self.description else {
            return String::new();
        };
        let s = &d.settings;
        match field {
            Field::Video => s.video.clone(),
            Field::Mode => {
                if s.format == "auto" {
                    "auto".into()
                } else {
                    Mode {
                        width: s.width,
                        height: s.height,
                        fps: s.fps,
                        format: s.format.clone(),
                    }
                    .key()
                }
            }
            Field::Input => s.audio_input.clone(),
            Field::Output => s.audio_output.clone(),
            Field::VirtualDevices => s.virtual_devices.to_string(),
            Field::Latency => s.latency.clone(),
            Field::Pacing => s.pacing.clone(),
            Field::Volume => s.volume.to_string(),
            Field::FrameGeneration => s.frame_generation.to_string(),
            Field::Theme => String::new(),
        }
    }
    fn button(
        &self,
        id: &'static str,
        label: &str,
        primary: bool,
        action: Action,
        cx: &mut Context<Self>,
    ) -> gpui::AnyElement {
        let kind = if primary { Kind::Primary } else { Kind::Secondary };
        self.styled_button(id, label, kind, action, cx)
    }
    fn styled_button(
        &self,
        id: &'static str,
        label: &str,
        kind: Kind,
        action: Action,
        cx: &mut Context<Self>,
    ) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        let base = div()
            .id(id)
            .flex()
            .items_center()
            .justify_center()
            .min_h(px(36.))
            .min_w(px(96.))
            .px_5()
            .rounded_full()
            .text_sm()
            .cursor_pointer()
            .on_click(cx.listener(move |this, _: &ClickEvent, window, cx| {
                match action {
                    Action::Fullscreen => window.toggle_fullscreen(),
                    Action::CopyLogs => {
                        let text = this.logs.iter().map(|(time, line)| {
                            format!("{time} {} {line}", log_level(line).0)
                        }).collect::<Vec<_>>().join("\n");
                        cx.write_to_clipboard(gpui::ClipboardItem::new_string(text));
                        this.status = "Console copied to clipboard".into();
                    }
                    Action::OpenRepo => cx.open_url(REPO_URL),
                    Action::OpenNvidiaLicense => {
                        let url = this.frame_gen.as_ref().map(|f| f.license_url.clone())
                            .filter(|u| !u.is_empty()).unwrap_or_else(|| NVIDIA_LICENSE_URL.into());
                        cx.open_url(&url);
                    }
                    Action::OpenConfig => {
                        if let Some(d) = &this.description {
                            cx.reveal_path(std::path::Path::new(&d.config_path));
                        }
                    }
                    _ => this.act(action),
                }
                cx.notify();
            }))
            .child(label.to_owned());
        let button = match kind {
            Kind::Primary => base
                .bg(rgb(p.primary))
                .shadow_md()
                .hover(move |s| s.bg(rgb(p.primary_hover))),
            Kind::Secondary => base
                .bg(rgb(p.accent))
                .shadow_md()
                .hover(move |s| s.bg(rgb(p.hover))),
            Kind::Outline => base
                .border_1()
                .border_color(rgb(p.muted))
                .hover(move |s| s.bg(rgb(p.accent))),
        };
        button.into_any_element()
    }
    fn heading(&self, title: &str, pill: Option<(String, bool)>) -> gpui::AnyElement {
        div()
            .flex()
            .items_center()
            .justify_between()
            .child(div().text_xl().font_weight(FontWeight::SEMIBOLD).child(title.to_owned()))
            .when_some(pill, |s, (text, live)| s.child(self.pill(text, live)))
            .into_any_element()
    }
    fn pill(&self, text: String, live: bool) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        div()
            .flex()
            .items_center()
            .gap_1p5()
            .px_2p5()
            .py_0p5()
            .rounded_full()
            .border_1()
            .border_color(rgb(p.border))
            .text_xs()
            .text_color(rgb(p.muted))
            .child(div().size(px(6.)).rounded_full().bg(rgb(if live { 0x4ade80 } else { p.muted })))
            .child(text)
            .into_any_element()
    }
    fn section_label(&self, text: &str) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        div()
            .text_xs()
            .font_weight(FontWeight::SEMIBOLD)
            .text_color(rgb(p.muted))
            .child(text.to_uppercase())
            .into_any_element()
    }
    fn divider(&self) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        div().w_full().h(px(1.)).flex_shrink_0().bg(rgb(p.border)).into_any_element()
    }
    fn stat(&self, label: &str, value: String) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        div()
            .flex()
            .flex_col()
            .gap_1()
            .min_w_0()
            .child(div().text_xs().text_color(rgb(p.muted)).child(label.to_owned()))
            .child(div().text_sm().overflow_hidden().whitespace_nowrap().child(value))
            .into_any_element()
    }
    fn theme_card(&self, index: usize, cx: &mut Context<Self>) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        let t = theme::palette(index);
        let selected = index == self.theme;
        div()
            .id(("theme-card", index))
            .w(px(148.))
            .h(px(88.))
            .flex()
            .flex_col()
            .justify_between()
            .p_1p5()
            .rounded_lg()
            .bg(rgb(t.background))
            .border_2()
            .border_color(rgb(if selected { p.ring } else { t.border }))
            .cursor_pointer()
            .hover(move |s| s.border_color(rgb(if selected { p.ring } else { t.muted })))
            .on_click(cx.listener(move |this, _, _, cx| {
                this.set(Field::Theme, &index.to_string());
                cx.notify();
            }))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap_1p5()
                    .p_2()
                    .rounded_md()
                    .bg(rgb(t.surface))
                    .child(div().w(px(48.)).h(px(9.)).rounded_full().bg(rgb(t.primary)))
                    .child(div().w(px(70.)).h(px(9.)).rounded_full().bg(rgb(t.accent))),
            )
            .child(div().px_1().text_xs().child(theme::NAMES[index]))
            .into_any_element()
    }
    fn select(&self, field: Field, label: &str, cx: &mut Context<Self>) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        let list = self.field_choices(field);
        let value = self.value(field);
        let selected = list
            .iter()
            .find(|c| c.value == value)
            .map(|c| c.label.clone())
            .unwrap_or_else(|| {
                if self.discovering {
                    "Finding devices…".into()
                } else {
                    "Choose an option".into()
                }
            });
        let trigger_entity = cx.entity().downgrade();
        let mut control = div()
            .relative()
            .flex()
            .flex_col()
            .gap_2()
            .w_full()
            .child(
                div()
                    .text_sm()
                    .text_color(rgb(p.muted))
                    .child(label.to_owned()),
            )
            .child(
                div()
                    .id(("select", field as usize))
                    .relative()
                    .flex()
                    .items_center()
                    .justify_between()
                    .gap_2()
                    .h(px(36.))
                    .px_4()
                    .text_sm()
                    .shadow_sm()
                    .rounded_full()
                    .bg(rgb(p.accent))
                    .border_1()
                    .border_color(rgb(if self.open == Some(field) {
                        p.ring
                    } else {
                        p.accent
                    }))
                    .cursor_pointer()
                    .hover(move |s| s.bg(rgb(p.hover)))
                    .on_click(cx.listener(move |this, _, window, cx| {
                        this.focus.focus(window);
                        this.open = if this.open == Some(field) {
                            None
                        } else {
                            Some(field)
                        };
                        this.popup_index = this
                            .field_choices(field)
                            .iter()
                            .position(|c| c.value == this.value(field))
                            .unwrap_or(0);
                        cx.notify();
                    }))
                    .child(
                        div()
                            .flex_1()
                            .overflow_hidden()
                            .whitespace_nowrap()
                            .child(selected),
                    )
                    .child(div().text_xs().child("▾"))
                    .child(canvas(
                        move |bounds, _, cx| {
                            let _ = trigger_entity.update(cx, |this, _| {
                                this.dropdown_bounds.insert(field as usize, bounds);
                            });
                        },
                        |_, _, _, _| {},
                    ).absolute().top_0().left_0().size_full()),
            );
        if self.open == Some(field) {
            let mut menu = div()
                .id("choice-menu")
                .absolute()
                .top(px(64.))
                .left_0()
                .right_0()
                .max_h(px(260.))
                .overflow_y_scroll()
                .flex()
                .flex_col()
                .p_2()
                .gap_1()
                .rounded_xl()
                .bg(rgb(p.surface))
                .border_1()
                .border_color(rgb(p.border))
                .shadow_lg()
                .on_mouse_down_out(cx.listener(move |this, event: &gpui::MouseDownEvent, _, cx| {
                    if this.dropdown_bounds.get(&(field as usize))
                        .is_some_and(|bounds| bounds.contains(&event.position)) {
                        return;
                    }
                    this.open = None;
                    cx.notify();
                }));
            for (i, choice) in list.into_iter().enumerate() {
                let picked = choice.value == value;
                menu = menu.child(
                    div()
                        .id(("choice", i))
                        .px_3()
                        .py_2()
                        .text_sm()
                        .rounded_lg()
                        .cursor_pointer()
                        .bg(rgb(if i == self.popup_index {
                            p.hover
                        } else if picked {
                            p.accent
                        } else {
                            p.surface
                        }))
                        .hover(move |s| s.bg(rgb(p.hover)))
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.set(field, &choice.value);
                            cx.stop_propagation();
                            cx.notify();
                        }))
                        .child(choice.label),
                );
            }
            control = control.child(deferred(menu).with_priority(10));
        }
        control.into_any_element()
    }
    fn toggle(
        &self,
        label: &str,
        enabled: bool,
        toggle: Toggle,
        id: &'static str,
        cx: &mut Context<Self>,
    ) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        div()
            .id(id)
            .flex()
            .items_center()
            .gap_2p5()
            .py_1p5()
            .text_sm()
            .cursor_pointer()
            .on_click(cx.listener(move |this, _, _, cx| {
                this.act(Action::Toggle(toggle));
                cx.notify();
            }))
            .child(
                div()
                    .size(px(18.))
                    .flex_shrink_0()
                    .rounded_full()
                    .flex()
                    .items_center()
                    .justify_center()
                    .text_xs()
                    .bg(rgb(if enabled { p.primary } else { p.accent }))
                    .when(enabled, |s| s.child("✓")),
            )
            .child(label.to_owned())
            .into_any_element()
    }
    fn frame_generation_section(&self, cx: &mut Context<Self>) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        let note = |text: String| div().text_sm().text_color(rgb(p.muted)).child(text);
        let info = self.frame_gen.as_ref();
        let installed = info.is_some_and(|f| f.runtime_installed);
        let status = match (info, self.frame_gen_checking) {
            (None, true) => "Checking what this GPU supports…".to_string(),
            (None, false) => "Frame generation support is unknown.".to_string(),
            (Some(f), _) if !f.runtime_installed => {
                "Needs NVIDIA's DLSS Frame Generation runtime (about 7 MB), downloaded directly from NVIDIA.".to_string()
            }
            (Some(f), _) if !f.available => format!("Unavailable: {}", f.reason),
            (Some(f), _) => {
                let mfg = if f.max_multiplier > 2 { " (multi frame generation)" } else { "" };
                format!("This GPU supports up to {}x{mfg}. 2x–4x are stable; 5x and 6x are experimental.", f.max_multiplier)
            }
        };
        let mut column = div().flex().flex_col().gap_3()
            .child(div().flex().gap_6()
                .child(div().flex_1().min_w_0().child(self.select(Field::FrameGeneration, "Frame generation (NVIDIA DLSS)", cx)))
                .child(div().flex_1().min_w_0()))
            .child(note(status))
            .child(note("Applies to the preview and every virtual camera. A 30 fps game sent at 60 Hz is detected, so 2x gives a smooth 60 fps.".into()));
        let mut buttons = div().flex().items_center().gap_3()
            .child(self.styled_button("nvidia-license", "View NVIDIA license", Kind::Outline, Action::OpenNvidiaLicense, cx));
        if let Some((done, total)) = self.runtime_busy {
            let percent = if total > 0 { done * 100 / total } else { 0 };
            buttons = buttons.child(note(format!("Working… {percent}%")));
        } else if installed {
            buttons = buttons.child(self.styled_button("nvidia-remove", "Remove NVIDIA runtime", Kind::Secondary,
                Action::RemoveNvidiaRuntime, cx));
        } else {
            buttons = buttons.child(self.styled_button("nvidia-install", "Accept license and download", Kind::Primary,
                Action::InstallNvidiaRuntime, cx));
        }
        column = column.child(buttons);
        if !installed {
            column = column.child(note("By downloading you accept the NVIDIA RTX SDK license. Capture View is not affiliated with or endorsed by NVIDIA.".into()));
        }
        column.into_any_element()
    }
    fn viewer_page(&self, fullscreen: bool, cx: &mut Context<Self>) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        let picture = if !self.show_preview() {
            div().size_full().flex().items_center().justify_center().text_color(rgb(p.muted))
                .child("Preview hidden").into_any_element()
        } else if let Some(image) = &self.preview_image {
            img(image.clone()).object_fit(ObjectFit::Contain).size_full().into_any_element()
        } else {
            div().size_full().flex().items_center().justify_center().text_color(rgb(p.muted))
                .child(if self.pending { "Starting capture…" } else if self.viewer.is_some() {
                    "Waiting for video…"
                } else { "Viewer stopped" }).into_any_element()
        };
        if fullscreen {
            let mut player = div().relative().size_full().bg(rgb(0x000000)).child(picture);
            if self.fullscreen_opacity > 0.0 {
                player = player.child(div().absolute().right_4().bottom_4().opacity(self.fullscreen_opacity)
                    .child(self.button("player-exit-fullscreen", "Exit fullscreen", false, Action::Fullscreen, cx)));
            }
            return player.into_any_element();
        }

        // Fit BOTH axes of the actual workspace. A width-only aspect ratio would
        // overflow short windows and crop the bottom of the player.
        const MARGIN: f32 = 24.0;
        const CONTROLS: f32 = 54.0;
        const ASPECT: f32 = 16.0 / 9.0;
        let available_width = (f32::from(self.viewer_area.width) - MARGIN * 2.0).max(0.0);
        let available_height = (f32::from(self.viewer_area.height) - MARGIN * 2.0 - CONTROLS).max(0.0);
        let video_width = available_width.min(available_height * ASPECT);
        let video_height = video_width / ASPECT;
        let mut dimensions = self.preview_size.map(|(w,h)|format!("{w} × {h}")).unwrap_or_else(||"Viewer".into());
        // Input = the game's real frame rate (repeated frames not counted);
        // output = what the preview and virtual cameras receive, incl. generated frames.
        if let Some((input, output)) = self.preview_rates {
            dimensions = format!("{dimensions} · in {:.0} fps → out {:.0} fps", input, output);
        }
        let player = div().w(px(video_width)).h(px(video_height + CONTROLS)).flex_shrink_0()
            .flex().flex_col().overflow_hidden().rounded_xl().bg(rgb(0x000000))
            .child(div().w_full().h(px(video_height)).flex_shrink_0().overflow_hidden().child(picture))
            .child(div().h(px(CONTROLS)).flex_shrink_0().flex().items_center().gap_3().px_4().bg(rgb(p.surface))
                .child(div().flex_1().min_w_0().overflow_hidden().whitespace_nowrap().text_sm().text_color(rgb(p.muted))
                    .child(if self.active_outputs > 0 {
                        format!("{dimensions} · {} virtual cameras · Capture View Audio", self.active_outputs)
                    } else { dimensions }))
                .when(self.viewer.is_some(), |s|s.child(self.button("player-stop", "Stop", false, Action::Stop, cx)))
                .when(self.viewer.is_none(), |s|s.child(self.button("player-start",
                    if self.pending { "Please wait…" } else { "Start viewer" }, true, Action::Start, cx)))
                .child(self.button("player-fullscreen", "Fullscreen", false, Action::Fullscreen, cx)));
        div().size_full().min_h_0().flex().items_center().justify_center()
            .child(player).into_any_element()
    }
    fn content(&self, cx: &mut Context<Self>) -> gpui::AnyElement {
        let p = theme::palette(self.theme);
        let s = self.description.as_ref().map(|d| &d.settings);
        let state = if self.pending { "Starting" } else if self.viewer.is_some() { "Running" } else { "Stopped" };
        match self.page {
            Page::Capture => {
                let resolution = self.preview_size.map(|(w, h)| format!("{w} × {h}"))
                    .or_else(|| s.filter(|s| s.format != "auto").map(|s| format!("{} × {} · {} fps", s.width, s.height, s.fps)))
                    .unwrap_or_else(|| "Automatic".into());
                let cameras = if self.viewer.is_some() { self.active_outputs } else { s.map_or(0, |s| s.virtual_devices) };
                div().flex().flex_col().gap_6()
                    .child(self.heading("Capture", Some((state.into(), self.viewer.is_some()))))
                    .child(div().flex().gap_6()
                        .child(div().flex_1().min_w_0().flex().flex_col().gap_4()
                            .child(self.select(Field::Video, "Capture device", cx))
                            .child(self.select(Field::Mode, "Picture quality", cx)))
                        .child(div().flex_1().min_w_0().flex().flex_col().gap_4()
                            .child(self.select(Field::Input, "Audio input", cx))
                            .child(self.select(Field::Output, "Audio output", cx))
                            .child(self.select(Field::Volume, "Volume", cx)))
                        .child(div().w(px(220.)).flex_shrink_0().flex().flex_col().gap_1().pt_6()
                            .child(self.toggle("Play capture audio", s.is_some_and(|s| s.audio_monitor), Toggle::Audio, "audio-toggle", cx))
                            .child(self.toggle("Mute", s.is_some_and(|s| s.muted), Toggle::Mute, "mute-toggle", cx))
                            .child(self.toggle("Show video in viewer", s.is_none_or(|s| s.show_preview), Toggle::Preview, "preview-toggle", cx))))
                    .child(self.divider())
                    .child(div().flex().gap_10()
                        .child(self.stat("Status", state.into()))
                        .child(self.stat("Resolution", resolution))
                        .child(self.stat("Virtual cameras", cameras.to_string()))
                        .child(div().flex_1().min_w_0().child(self.stat("Last activity", self.status.clone()))))
                    .child(self.divider())
                    .child(div().flex().justify_end().gap_3()
                        .child(self.styled_button("refresh-devices",
                            if self.discovering { "Refreshing…" } else { "Refresh devices" },
                            Kind::Outline, Action::Refresh, cx))
                        .when(self.viewer.is_some(), |d| d.child(self.button("stop-viewer", "Stop viewer", false, Action::Stop, cx)))
                        .child(self.button("start-viewer",
                            if self.pending { "Please wait…" } else if self.viewer.is_some() { "Restart viewer" } else { "Start viewer" },
                            true, Action::Start, cx)))
                    .into_any_element()
            }
            Page::Viewer => self.viewer_page(false, cx),
            Page::Settings => div().flex().flex_col().gap_5()
                .child(self.heading("Settings", None))
                .child(self.section_label("Theme"))
                .child(div().flex().flex_wrap().gap_3()
                    .children((0..theme::NAMES.len()).map(|i| self.theme_card(i, cx))))
                .child(div().pt_2().child(self.section_label("Capture")))
                .child(div().flex().gap_6()
                    .child(div().flex_1().min_w_0().child(self.select(Field::VirtualDevices, "Virtual video devices", cx)))
                    .child(div().flex_1().min_w_0().child(self.select(Field::Latency, "Latency", cx)))
                    .child(div().flex_1().min_w_0().child(self.select(Field::Pacing, "Frame timing", cx))))
                .child(div().pt_2().child(self.section_label("Frame generation")))
                .child(self.frame_generation_section(cx))
                .child(div().pt_2().child(self.section_label("General")))
                .child(div().flex().items_center().gap_6()
                    .child(self.toggle("Minimize to system tray", s.is_some_and(|s| s.system_tray), Toggle::Tray, "tray-toggle", cx)))
                .child(div().text_sm().text_color(rgb(p.muted)).child("Restart viewer to apply capture changes."))
                .into_any_element(),
            Page::Console => {
                let lines = div().id("console-lines").h(px(380.)).overflow_y_scroll().rounded_xl().bg(rgb(p.surface))
                    .p_4().text_sm().font_family("DejaVu Sans Mono");
                let lines = if self.logs.is_empty() {
                    lines.child(div().text_color(rgb(p.muted)).child("No log messages yet."))
                } else {
                    lines.children(self.logs.iter().map(|(time, line)| {
                        let (level, color) = log_level(line);
                        div().flex().gap_3().pb_1()
                            .child(div().flex_shrink_0().text_color(rgb(p.muted)).child(time.clone()))
                            .child(div().w(px(48.)).flex_shrink_0().text_color(rgb(color)).child(level))
                            .child(div().flex_1().min_w_0().child(line.clone()))
                    }))
                };
                div().flex().flex_col().gap_5()
                    .child(self.heading("Console", None))
                    .child(lines)
                    .child(div().flex().justify_end().gap_3()
                        .child(self.styled_button("clear-console", "Clear", Kind::Outline, Action::Clear, cx))
                        .child(self.styled_button("copy-console", "Copy all", Kind::Primary, Action::CopyLogs, cx)))
                    .into_any_element()
            }
            Page::About => {
                let row = |label: &str, value: String| div().flex().justify_between().gap_6().text_sm()
                    .child(div().flex_shrink_0().text_color(rgb(p.muted)).child(label.to_owned()))
                    .child(div().min_w_0().overflow_hidden().whitespace_nowrap().child(value));
                let config = self.description.as_ref().map_or_else(|| "—".into(), |d| d.config_path.clone());
                div().flex().flex_col().items_center().gap_3().pt_10()
                    .child(div().size(px(76.)).rounded_2xl().shadow_lg().flex().items_center().justify_center()
                        .bg(gpui::linear_gradient(135.,
                            gpui::linear_color_stop(rgb(p.primary_hover), 0.),
                            gpui::linear_color_stop(rgb(p.accent), 1.)))
                        .text_size(px(38.)).font_weight(FontWeight::BOLD).child("C"))
                    .child(div().pt_1().text_2xl().font_weight(FontWeight::SEMIBOLD).child("Capture View"))
                    .child(div().text_sm().text_color(rgb(p.muted))
                        .child("Low-latency capture card viewer with audio monitoring and virtual cameras."))
                    .child(self.pill(format!("MIT license · {VERSION}"), true))
                    .child(div().mt_2().w(px(440.)).max_w_full().flex().flex_col().gap_3().p_4().rounded_xl().bg(rgb(p.surface))
                        .child(row("Config file", config))
                        .child(row("Engine", self.engine.to_string_lossy().into_owned())))
                    .child(div().mt_2().flex().gap_3()
                        .child(self.styled_button("about-github", "GitHub", Kind::Outline, Action::OpenRepo, cx))
                        .child(self.styled_button("about-config", "Open config folder", Kind::Primary, Action::OpenConfig, cx)))
                    .into_any_element()
            }
        }
    }
}
fn stop_child(child: &mut Child) {
    unsafe { libc::kill(child.id() as libc::pid_t, libc::SIGTERM); }
    for _ in 0..60 {
        if matches!(child.try_wait(), Ok(Some(_))) { return; }
        thread::sleep(Duration::from_millis(5));
    }
    let _=child.kill(); let _=child.wait();
}

fn apply_mode(d: &mut Description, m: &Mode) {
    d.settings.width = m.width;
    d.settings.height = m.height;
    d.settings.fps = m.fps;
    d.settings.format = m.format.clone();
}
impl Drop for CaptureView {
    fn drop(&mut self) {
        self.stop();
    }
}
impl Render for CaptureView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let p = theme::palette(self.theme);
        let fullscreen = self.page == Page::Viewer && window.is_fullscreen();
        if fullscreen && !self.fullscreen_mode {
            self.fullscreen_interaction();
            self.fullscreen_pointer = Some(window.mouse_position());
        }
        self.fullscreen_mode = fullscreen;
        if fullscreen {
            return div().size_full().track_focus(&self.focus)
                .on_mouse_move(cx.listener(|this, event: &gpui::MouseMoveEvent, _, cx| {
                    if this.fullscreen_pointer != Some(event.position) {
                        this.fullscreen_pointer = Some(event.position);
                        this.fullscreen_interaction();
                        cx.notify();
                    }
                }))
                .on_mouse_down(MouseButton::Left, cx.listener(|this, _, _, cx| {
                    this.fullscreen_interaction();cx.notify();
                }))
                .on_scroll_wheel(cx.listener(|this, _, _, cx| {
                    this.fullscreen_interaction();cx.notify();
                }))
                .on_key_down(cx.listener(|this, event: &gpui::KeyDownEvent, window, cx| {
                    if matches!(event.keystroke.key.as_str(), "escape" | "f" | "f11") {window.toggle_fullscreen();}
                    else {this.fullscreen_interaction();}
                    cx.notify();
                }))
                .child(self.viewer_page(true,cx)).into_any_element();
        }
        let workspace_entity = cx.entity().downgrade();
        let mut tabs = div().flex().justify_center().gap_3();
        for (page, id, label) in [
            (Page::Capture, "tab-capture", "Capture"),
            (Page::Viewer, "tab-viewer", "Viewer"),
            (Page::Settings, "tab-settings", "Settings"),
            (Page::Console, "tab-console", "Console"),
            (Page::About, "tab-about", "About"),
        ] {
            tabs = tabs.child(self.button(id, label, self.page == page, Action::Page(page), cx));
        }
        div()
            .track_focus(&self.focus)
            .on_key_down(cx.listener(|this, event: &gpui::KeyDownEvent, window, cx| {
                if this.page == Page::Viewer && matches!(event.keystroke.key.as_str(), "f" | "f11") {
                    window.toggle_fullscreen();cx.notify();return;
                }
                if let Some(field) = this.open {
                    let list = this.field_choices(field);
                    match event.keystroke.key.as_str() {
                        "escape" => this.open = None,
                        "down" => {
                            this.popup_index =
                                (this.popup_index + 1).min(list.len().saturating_sub(1))
                        }
                        "up" => this.popup_index = this.popup_index.saturating_sub(1),
                        "enter" => {
                            if let Some(c) = list.get(this.popup_index) {
                                this.set(field, &c.value);
                            }
                        }
                        _ => return,
                    }
                    cx.stop_propagation();
                    cx.notify();
                }
            }))
            .size_full()
            .flex()
            .flex_col()
            .bg(rgb(p.background))
            .text_color(rgb(0xffffff))
            .text_size(px(15.))
            .font_family("DejaVu Sans")
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap_6()
                    .pt_6()
                    .pb_5()
                    .px_8()
                    .border_b_1()
                    .border_color(rgb(p.border))
                    .child(
                        div()
                            .flex()
                            .items_baseline()
                            .justify_center()
                            .gap_3()
                            .child(
                                div()
                                    .text_size(px(42.))
                                    .font_weight(FontWeight::NORMAL)
                                    .child("Capture View"),
                            )
                            .child(div().text_sm().text_color(rgb(p.muted)).child(VERSION)),
                    )
                    .child(tabs),
            )
            .child(
                div()
                    .id("workspace")
                    .relative()
                    .flex_1()
                    .min_h_0()
                    .flex().flex_col()
                    .when(self.page == Page::Viewer, |s|s.overflow_hidden().child(canvas(
                        move |bounds, _, cx| {
                            let _ = workspace_entity.update(cx, |this, cx| {
                                if this.viewer_area != bounds.size {
                                    this.viewer_area = bounds.size;
                                    cx.notify();
                                }
                            });
                        },
                        |_, _, _, _| {},
                    ).absolute().top_0().left_0().size_full()))
                    .when(self.page != Page::Viewer, |s|s.overflow_y_scroll().px_12().py_8())
                    .child(self.content(cx)),
            )
            .into_any_element()
    }
}
fn main() {
    desktop::run();
}
