use crate::CaptureView;
use gpui::{App, AppContext, Application, Bounds, Entity, SharedString, TitlebarOptions, WindowBounds,
    WindowHandle, WindowOptions, px, size};
use std::{
    fs::{self, File, OpenOptions, TryLockError},
    io::{self, Read, Write},
    os::unix::{fs::{DirBuilderExt, MetadataExt, OpenOptionsExt, PermissionsExt},
        net::{UnixListener, UnixStream}},
    path::PathBuf,
    sync::{Arc, atomic::{AtomicBool, Ordering}, mpsc::{self, Sender}},
    thread,
    time::{Duration, Instant},
};

#[derive(Clone, Copy)]
pub enum Command { Show, Toggle, Close, Quit }

// One owner per login user, independent of AppImage mount or executable path.
// The OS releases the lock on crashes; only the lock owner replaces the socket.
struct Instance {
    _lock: File,
    socket: PathBuf,
    stop: Arc<AtomicBool>,
    worker: Option<thread::JoinHandle<()>>,
}
impl Instance {
    fn acquire(commands: Sender<Command>) -> io::Result<Option<Self>> {
        let base = std::env::var_os("XDG_RUNTIME_DIR").map(PathBuf::from)
            .or_else(|| std::env::var_os("XDG_CACHE_HOME").map(PathBuf::from))
            .or_else(|| std::env::var_os("HOME").map(|p|PathBuf::from(p).join(".cache")))
            .ok_or_else(|| io::Error::other("No per-user runtime directory"))?;
        let directory = base.join("capture-view-session");
        fs::DirBuilder::new().recursive(true).mode(0o700).create(&directory)?;
        let metadata = fs::symlink_metadata(&directory)?;
        if !metadata.is_dir() || metadata.uid() != unsafe { libc::geteuid() }
            || metadata.mode() & 0o077 != 0 {
            return Err(io::Error::other("Unsafe Capture View runtime directory"));
        }
        let lock = OpenOptions::new().read(true).write(true).create(true).truncate(false)
            .mode(0o600).custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(directory.join("instance.lock"))?;
        let socket = directory.join("activate.sock");
        match lock.try_lock() {
            Ok(()) => {},
            Err(TryLockError::Error(error)) => return Err(error),
            Err(TryLockError::WouldBlock) => {
                let deadline = Instant::now() + Duration::from_secs(3);
                loop {
                    if let Ok(mut stream) = UnixStream::connect(&socket) {
                        stream.set_read_timeout(Some(Duration::from_millis(500)))?;
                        let mut ack = [0];
                        if stream.read_exact(&mut ack).is_ok() && ack == [1] { return Ok(None); }
                    }
                    if Instant::now() >= deadline {
                        return Err(io::Error::other("Capture View is already running but did not respond"));
                    }
                    thread::sleep(Duration::from_millis(25));
                }
            }
        }
        match fs::remove_file(&socket) {
            Ok(()) => {},
            Err(e) if e.kind() == io::ErrorKind::NotFound => {},
            Err(e) => return Err(e),
        }
        let listener = UnixListener::bind(&socket)?;
        fs::set_permissions(&socket, fs::Permissions::from_mode(0o600))?;
        listener.set_nonblocking(true)?;
        let stop = Arc::new(AtomicBool::new(false));
        let shutdown = stop.clone();
        let worker = thread::spawn(move || {
            while !shutdown.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((mut stream, _)) => {
                        if commands.send(Command::Show).is_ok() {
                            let _ = stream.set_write_timeout(Some(Duration::from_millis(100)));
                            let _ = stream.write_all(&[1]);
                        }
                    },
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => thread::sleep(Duration::from_millis(20)),
                    Err(_) => break,
                }
            }
        });
        Ok(Some(Self { _lock: lock, socket, stop, worker: Some(worker) }))
    }
}
impl Drop for Instance {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(worker) = self.worker.take() { let _ = worker.join(); }
        let _ = fs::remove_file(&self.socket);
    }
}

pub struct Tray(ksni::blocking::Handle<TrayItem>);
struct TrayItem { commands: Sender<Command>, icon: ksni::Icon }
impl ksni::Tray for TrayItem {
    fn id(&self) -> String { "capture-view".into() }
    fn title(&self) -> String { "Capture View".into() }
    fn icon_name(&self) -> String { "capture-view".into() }
    fn icon_pixmap(&self) -> Vec<ksni::Icon> { vec![self.icon.clone()] }
    fn activate(&mut self, _: i32, _: i32) { let _ = self.commands.send(Command::Toggle); }
    fn watcher_offline(&self, _: ksni::OfflineReason) -> bool {
        // Never strand a hidden app if the desktop tray disappears.
        let _ = self.commands.send(Command::Show);
        false
    }
    fn menu(&self) -> Vec<ksni::MenuItem<Self>> {
        vec![ksni::menu::StandardItem {
            label: "Quit".into(),
            activate: Box::new(|tray: &mut Self| { let _ = tray.commands.send(Command::Quit); }),
            ..Default::default()
        }.into()]
    }
}
impl Tray {
    pub fn new(commands: Sender<Command>) -> Result<Self, String> {
        use ksni::blocking::TrayMethods;
        let image = image::load_from_memory(include_bytes!("../../data/capture-view.png"))
            .map_err(|e|e.to_string())?.into_rgba8();
        let icon = ksni::Icon {
            width: image.width() as i32, height: image.height() as i32,
            data: image.pixels().flat_map(|p|[p[3],p[0],p[1],p[2]]).collect(),
        };
        TrayItem { commands, icon }.spawn().map(Self).map_err(|e|e.to_string())
    }
    pub fn available(&self) -> bool { !self.0.is_closed() }
}
impl Drop for Tray {
    fn drop(&mut self) { let _ = self.0.shutdown(); }
}

fn options(cx: &App) -> WindowOptions {
    WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(1040.),px(740.)),cx))),
        window_min_size: Some(size(px(820.),px(620.))),
        titlebar: Some(TitlebarOptions { title: Some(SharedString::from("Capture View")), ..Default::default() }),
        app_id: Some("capture-view".into()),
        ..Default::default()
    }
}
fn close_handler(window: &gpui::Window, cx: &App, commands: Sender<Command>) {
    window.on_window_should_close(cx, move |_, _| {
        let _ = commands.send(Command::Close);
        false
    });
}
fn show(view: &Entity<CaptureView>, window: &mut Option<WindowHandle<CaptureView>>,
        commands: &Sender<Command>, cx: &mut App) {
    if let Some(handle) = window {
        let _ = handle.update(cx, |view, window, cx| {
            window.activate_window();
            view.focus.focus(window);
            cx.notify();
        });
        return;
    }
    let root = view.clone();
    let commands = commands.clone();
    match cx.open_window(options(cx), move |window, cx| {
        close_handler(window, cx, commands);
        root.update(cx, |view, cx| {
            view.window_visible = true;
            view.fullscreen_mode = false;
            view.dropdown_bounds.clear();
            view.focus.focus(window);
            view.update_preview_visibility();
            cx.notify();
        });
        root
    }) {
        Ok(handle) => *window = Some(handle),
        Err(error) => eprintln!("Could not open Capture View: {error}"),
    }
}
fn hide(window: &mut Option<WindowHandle<CaptureView>>, cx: &mut App) {
    if let Some(handle) = window.take() {
        let _ = handle.update(cx, |view, window, cx| {
            view.window_visible = false;
            view.fullscreen_mode = false;
            view.open = None;
            view.update_preview_visibility();
            if let Some(image) = view.preview_image.take() { cx.drop_image(image, None); }
            window.remove_window();
        });
    }
}

pub fn run() {
    let (commands, incoming) = mpsc::channel();
    let instance = match Instance::acquire(commands.clone()) {
        Ok(Some(instance)) => instance,
        Ok(None) => return,
        Err(error) => { eprintln!("{error}"); return; },
    };
    Application::new().run(move |cx: &mut App| {
        let tx = commands.clone();
        let handle = match cx.open_window(options(cx), move |window, cx| {
            close_handler(window, cx, tx.clone());
            cx.new(|cx|CaptureView::new(window, cx, tx))
        }) {
            Ok(handle) => handle,
            Err(error) => { eprintln!("Could not open Capture View: {error}"); cx.quit(); return; },
        };
        let view = handle.update(cx, |_, _, cx|cx.entity()).expect("newly created window");
        let mut window = Some(handle);
        // Own the entity outside the native window so capture survives closing it.
        cx.spawn(async move |cx| {
            let _instance = instance;
            loop {
                smol::Timer::after(Duration::from_millis(8)).await;
                let keep_running = cx.update(|cx| {
                    while let Ok(command) = incoming.try_recv() {
                        match command {
                            Command::Show => show(&view, &mut window, &commands, cx),
                            Command::Toggle => {
                                if window.is_some() && view.read(cx).tray_available() { hide(&mut window, cx); }
                                else { show(&view, &mut window, &commands, cx); }
                            },
                            Command::Close => {
                                if view.read(cx).tray_available() { hide(&mut window, cx); }
                                else {
                                    view.update(cx, |view, _|view.stop());
                                    cx.quit(); return false;
                                }
                            },
                            Command::Quit => {
                                view.update(cx, |view, _|view.stop());
                                cx.quit(); return false;
                            },
                        }
                    }
                    view.update(cx, |view, cx| { if view.poll(cx) { cx.notify(); } });
                    if window.is_none() && !view.read(cx).tray_available() {
                        show(&view, &mut window, &commands, cx);
                    }
                    true
                });
                if !matches!(keep_running, Ok(true)) { break; }
            }
        }).detach();
    });
}
