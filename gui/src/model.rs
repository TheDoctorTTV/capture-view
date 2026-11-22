use serde::Deserialize;

fn default_show_preview() -> bool { true }

#[derive(Clone, Debug, Deserialize)]
pub struct Mode {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub format: String,
}
impl Mode {
    pub fn key(&self) -> String {
        format!(
            "{}x{}:{}:{}",
            self.width, self.height, self.fps, self.format
        )
    }
    pub fn label(&self) -> String {
        let name = match (self.width, self.height) {
            (3840, 2160) => "4K",
            (2560, 1440) => "1440p",
            (1920, 1080) => "Full HD",
            (1280, 720) => "HD",
            _ => "",
        };
        let dimensions = format!("{} × {}", self.width, self.height);
        let size = if name.is_empty() {
            dimensions
        } else {
            format!("{name} ({dimensions})")
        };
        let color = match self.format.as_str() {
            "yuyv" => "Uncompressed",
            "nv12" => "Uncompressed, lower bandwidth",
            "mjpeg" => "Motion JPEG",
            "h264" => "H.264",
            _ => "Automatic",
        };
        format!("{size} · {} fps · {color}", self.fps)
    }
    pub fn rank(&self) -> (u64, u32, u32, u32, u8) {
        // Resolution always takes precedence; FPS only orders equal resolutions.
        // Format is a final tie-breaker so duplicate modes remain adjacent.
        let format = match self.format.as_str() {
            "yuyv" => 3,
            "nv12" => 2,
            "mjpeg" => 1,
            _ => 0,
        };
        (
            u64::from(self.width) * u64::from(self.height),
            self.width,
            self.height,
            self.fps,
            format,
        )
    }
}
#[derive(Clone, Debug, Deserialize)]
pub struct VideoDevice {
    pub path: String,
    pub name: String,
    pub modes: Vec<Mode>,
}
impl VideoDevice {
    pub fn label(&self) -> String {
        if let Some((a, b)) = self.name.split_once(':') {
            let a = a.trim();
            let b = b.trim();
            if a.starts_with(b) || b.starts_with(a) {
                return if a.len() >= b.len() { a } else { b }.to_owned();
            }
        }
        if self.name.trim().is_empty() {
            "Capture device".into()
        } else {
            self.name.trim().to_owned()
        }
    }
}
#[derive(Clone, Debug, Deserialize)]
pub struct AudioDevice {
    pub name: String,
    pub label: String,
    pub kind: String,
}
#[derive(Clone, Debug, Deserialize)]
pub struct Settings {
    #[serde(default)]
    pub virtual_devices: u32,
    pub video: String,
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub format: String,
    pub audio_input: String,
    pub audio_output: String,
    #[serde(default = "default_show_preview")]
    pub show_preview: bool,
    #[serde(default)]
    pub system_tray: bool,
    pub audio_monitor: bool,
    pub renderer: String,
    pub latency: String,
    pub pacing: String,
    pub vsync: bool,
    pub muted: bool,
    pub volume: f32,
}
#[derive(Clone, Debug, Deserialize)]
pub struct Description {
    pub config_path: String,
    pub audio_error: String,
    pub arguments: Vec<String>,
    pub settings: Settings,
    pub video_devices: Vec<VideoDevice>,
    pub audio_devices: Vec<AudioDevice>,
}
impl Description {
    pub fn modes(&self) -> Vec<Mode> {
        let mut modes = self
            .video_devices
            .iter()
            .find(|d| d.path == self.settings.video)
            .map(|d| d.modes.clone())
            .unwrap_or_default();
        modes.sort_by_key(|m| std::cmp::Reverse(m.rank()));
        modes.dedup_by(|a, b| a.key() == b.key());
        modes
    }
    // Preserve every engine preference, then override only the GUI's managed controls.
    pub fn viewer_args(&self) -> Vec<String> {
        let s = &self.settings;
        let mut args = self.arguments.clone();
        for (flag, value) in [
            ("--latency-mode", s.latency.clone()),
            ("--video", s.video.clone()),
            ("--virtual-devices", s.virtual_devices.to_string()),
            ("--video-output", String::new()),
            ("--size", format!("{}x{}", s.width, s.height)),
            ("--fps", s.fps.to_string()),
            ("--format", s.format.clone()),
            ("--audio-input", s.audio_input.clone()),
            ("--audio-output", s.audio_output.clone()),
            ("--render-backend", s.renderer.clone()),
            ("--frame-pacing", s.pacing.clone()),
            ("--volume", s.volume.to_string()),
        ] {
            args.push(flag.into());
            args.push(value);
        }
        for (on, yes, no) in [
            (s.system_tray, "--system-tray", "--no-system-tray"),
            (s.show_preview, "--show-preview", "--hide-preview"),
            (s.audio_monitor, "--audio-monitor", "--no-audio-monitor"),
            (s.vsync, "--vsync", "--no-vsync"),
        ] {
            args.push(if on { yes } else { no }.into());
        }
        // These flags have no inverse in the engine. Remove any saved copies first.
        args.retain(|a| !matches!(a.as_str(), "--fullscreen" | "--borderless" | "--mute"));
        if s.muted {
            args.push("--mute".into());
        }
        args
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn readable_labels() {
        let d = VideoDevice {
            path: "/dev/video0".into(),
            name: "Elgato HD60 X: Elgato HD60 X".into(),
            modes: vec![],
        };
        assert_eq!(d.label(), "Elgato HD60 X");
        let m = Mode {
            width: 1920,
            height: 1080,
            fps: 60,
            format: "nv12".into(),
        };
        assert!(m.label().contains("Full HD"));
        assert!(!m.label().contains("nv12"));
    }
    #[test]
    fn overrides_preserve_unmanaged_settings() {
        let d:Description=serde_json::from_value(serde_json::json!({
          "config_path":"/tmp/config.toml","audio_error":"","arguments":["--no-load-config","--audio-delay-ms","25","--fullscreen","--mute"],
          "settings":{"video":"/dev/video0","width":1920,"height":1080,"fps":60,"format":"yuyv","audio_input":"card","audio_output":"default","audio_monitor":true,"renderer":"vulkan","latency":"low","pacing":"yield","vsync":false,"fullscreen":false,"borderless":false,"muted":false,"volume":0.5},
          "video_devices":[],"audio_devices":[]
        })).unwrap();
        let args = d.viewer_args();
        assert!(!args.contains(&"--fullscreen".into()));
        assert!(!args.contains(&"--mute".into()));
        assert!(args.windows(2).any(|p| p == ["--audio-delay-ms", "25"]));
        assert!(args.windows(2).any(|p| p == ["--audio-output", "default"]));
        assert!(args.windows(2).any(|p| p == ["--render-backend", "vulkan"]));
    }
}
