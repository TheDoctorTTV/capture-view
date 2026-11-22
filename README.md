# capture-view

![Capture View logo](docs/assets/logo-readme.png)

Native low-latency HDMI USB capture-card viewer for Linux.

Current features:

- V4L2 device discovery
- manual video format selection
- streaming V4L2 MMAP capture with 2-3 buffers
- newest-independent-frame dropping (raw/MJPEG)
- MJPEG decode through libjpeg-turbo
- low-delay H.264 capture with best-effort zero-B-frame card tuning
- YUYV/NV12 conversion
- SDL3 streaming texture preview
- optional PipeWire audio monitor
- optional PipeWire virtual audio source
- optional v4l2loopback video output
- SDL GPU-backed YUYV/NV12 texture upload path
- optional OpenGL shader render backend
- aspect-preserving bilinear fullscreen presentation
- Native Rust + GPUI interface for device selection, audio routing, viewer settings, themes, and logs
- Kirsch bitmap font for SDL diagnostics overlays

## Why use it

`capture-view` is built for live HDMI capture monitoring, not media playback or recording. It keeps only the newest frame, drops stale data intentionally, and exposes timing metrics that general-purpose players usually hide.

Use it when you want:

- predictable low-latency preview for consoles or HDMI capture cards
- direct V4L2 capture with explicit format/FPS selection
- PipeWire audio monitoring and optional virtual audio source
- optional v4l2loopback video output for Discord/OBS-style workflows
- a small native Linux tool instead of OBS, browser capture, or a full media player

## Dependencies

Arch Linux:

```sh
sudo pacman -S base-devel cmake ninja pipewire sdl3 libjpeg-turbo ffmpeg libglvnd v4l-utils rust clang libxkbcommon libxkbcommon-x11 fontconfig vulkan-icd-loader
```

## Arch Linux / CachyOS native builds

The Linux release tarball is dynamically linked to the FFmpeg libraries on the
release builder. Bundled SDL3 does not make it independent of the system FFmpeg
ABI. If a release requests an older `libavcodec.so`, `libavutil.so`, or
`libswscale.so` than your system provides, build from source against your current
libraries. Do not replace system libraries or symlink different ABI versions.

The native source build works with FFmpeg 9 and SDL 3.4. From this checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/capture-view --no-config --help
./build/capture-view-gui
```

This uses the installed libraries and requires no `LD_LIBRARY_PATH` override or
bundled SDL3. `--no-config --help` is a launch check that does not open the camera
or save settings.

For a pacman-managed installation, run `makepkg -si` from the checkout. The
PKGBUILD builds and tests the application and installs the executable, launcher,
icon, font, manual, and licenses. It records the FFmpeg library ABI dependencies
so a later incompatible FFmpeg upgrade requires a rebuild. Rebuild the package
when moving to a new FFmpeg library ABI.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

Windows with vcpkg:

```powershell
vcpkg install sdl3:x64-windows
cmake -S . -B build-windows -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build-windows
```

The Windows build is graphical-first: launching `capture-view.exe` opens the SDL
preview UI with the built-in test pattern. Linux-only V4L2 capture, PipeWire
audio, and GPUI interface features return clear unsupported messages until native
Windows backends are added.

Package locally:

```sh
makepkg -si
```

## Usage

List video devices and formats:

```sh
./build/capture-view --list-devices
```

List PipeWire audio nodes:

```sh
./build/capture-view --list-audio
```

Troubleshoot environment:

```sh
./build/capture-view --doctor
```

Interactive setup:

```sh
./build/capture-view --wizard --log-file capture-view.log
```

Rust / GPUI interface:

```sh
./build/capture-view-gui
```

The GPUI interface has Capture, Viewer, Settings, Console, and About pages. It follows the
rounded controls, centered navigation, and dark themes of Stream 24/7, Asset
Extractor, and Bigscreen Remote Desktop. The default is Amethyst; the matching
color themes and Midnight are available in Settings.

The interface is Rust, while the existing C++ process handles V4L2 video, SDL
rendering, and PipeWire audio. A structured `--gui-info` interface supplies device
choices and saved settings. Start, stop, restart, save, and refresh actions are
managed without shell commands. `capture-view --gui` also opens the Rust GUI;
`--gtk` is retained only as a command-line compatibility alias.

CMake builds both binaries on Linux. For frontend-only development, use
`cargo build --locked --target-dir build/rust`, with the engine already built in
`build/capture-view`, or set `CAPTURE_VIEW_ENGINE` to its path. Rust checks can be
run with `cargo test --locked --target-dir build/rust`.

Profiles/config:

```sh
./build/capture-view --wizard --profile console --save-config
./build/capture-view --profile console
./build/capture-view --list-profiles
./build/capture-view --init-profiles
```

Diagnostic bundle:

```sh
./build/capture-view --diagnostic-bundle capture-view-diagnostics.txt
```

Expose capture audio as a PipeWire source for Discord:

```sh
./build/capture-view --video /dev/video2 --format mjpeg --audio-monitor \
  --audio-virtual-source "Capture View" --log-file capture-view.log
```

Discord should then be able to select `Capture View` as an audio source/microphone while you cast the screen.

Expose capture video as a v4l2loopback virtual camera:

```sh
sudo modprobe v4l2loopback video_nr=10 card_label="Capture View" exclusive_caps=1
./build/capture-view --video /dev/video2 --video-output /dev/video10 --format auto
```

The virtual camera can write `yuyv`, `nv12`, `rgba`, or passthrough `mjpeg` frames:

```sh
./build/capture-view --video /dev/video2 --video-output /dev/video10 --video-output-format nv12
```

`mjpeg` output requires MJPEG capture input and forwards compressed frames without re-encoding. Match capture size/FPS and output format to what the receiving app expects.

Write diagnostics to a file while also logging to stderr:

```sh
./build/capture-view --video /dev/video0 --format auto --latency-mode ultra --log-file capture-view.log --verbose
```

Choose frame pacing explicitly:

```sh
./build/capture-view --video /dev/video0 --frame-pacing immediate
./build/capture-view --video /dev/video0 --frame-pacing yield
./build/capture-view --video /dev/video0 --frame-pacing sleep --vsync
./build/capture-view --video /dev/video0 --frame-pacing adaptive
```

Use the explicit OpenGL shader backend if SDL-native texture conversion or presentation is poor on a target GPU. On AMD/Mesa this path uploads raw YUYV/NV12 planes and does color conversion in a fragment shader, avoiding the CPU RGBA conversion path for raw capture modes:

```sh
./build/capture-view --video /dev/video0 --render-backend opengl
```

Best latency options are raw `yuyv`/`nv12` if USB bandwidth allows, `--latency-mode ultra`, `--frame-pacing immediate`, and `--no-vsync`. The ultra preset uses two V4L2 buffers and starts PipeWire monitoring at 3 ms; audio autotune raises that buffer only when underruns require it.

If the card only exposes H.264, select `--format h264`. The viewer asks compatible V4L2 hardware encoders for zero B-frames, constrained-baseline/CAVLC, one reference picture, and no hierarchical coding, then uses FFmpeg's low-delay slice-threaded decoder. Unsupported card controls are logged and ignored. H.264 remains behind raw and MJPEG in automatic mode because card-side encoding and inter-frame dependencies normally add latency; unlike independent frames, H.264 packets are decoded in order so references are not corrupted.

Test only audio output with a quiet 440 Hz tone:

```sh
./build/capture-view --test-pattern --audio-test-tone --audio-output "YOUR_SINK_NAME" --log-file audio-test.log
```

Open a capture card:

```sh
./build/capture-view --video /dev/video0 --size 1920x1080 --fps 60 --format auto --no-vsync
```

Open without a titlebar/window controls for cleaner screen sharing or OBS capture:

```sh
./build/capture-view --video /dev/video0 --borderless
```

Enable direct PipeWire monitoring:

```sh
./build/capture-view --video /dev/video0 --audio-monitor --audio-input "alsa_input.usb..." --audio-output "alsa_output..." --audio-buffer-ms 5 --audio-delay-ms 0
```

Run without hardware:

```sh
./build/capture-view --test-pattern
```

Shortcuts:

- `F`: fullscreen
- `Alt+B`: toggle borderless/titlebar
- `V`: toggle vsync
- `S`: stats overlay
- `G`: GUI overlay
- `?`: keybinding help overlay
- `R`: restart capture
- `A`: restart audio monitor
- `M`: mute audio monitor
- `+` / `-`: audio monitor volume
- `Esc`: exit fullscreen or quit
- `Q`: quit

Config is saved to `~/.config/lowlat-capture-viewer/config.toml`. Use `--no-config` for temporary runs. Use `--no-load-config` to start from defaults and CLI options while still saving to the selected config/profile.

Most V4L2 capture cards can only be opened by one app at a time. Close OBS, Discord, browser tabs, or other viewers if the device is busy.

## Third-party Assets

The SDL overlay font uses Kirsch by molarmanful, bundled under the SIL Open Font License 1.1 in `data/kirsch.OFL.txt`.

### Audio routing and renderers

The launcher's **System default** audio input/output choices follow desktop audio
policy instead of pinning a device. For HDMI game sound, select your capture card
as the audio input; the system default input is often your microphone. Select
**System default** output to follow your usual speakers/headphones. Enable
**Play capture audio** to hear it locally.

The embedded Viewer page uses GPUI's GPU renderer. Standalone CLI viewing still
supports SDL, Vulkan, and OpenGL through `--render-backend`. Display timing for
the embedded viewer follows the desktop compositor.
CLI audio routes also accept `--audio-input default --audio-output default`.

### Build an AppImage

On x86_64 CachyOS / Arch, use the installed development dependencies listed above
and run:

```sh
./build-appimage.sh
```

The script builds only `capture-view`, stages its installed resources, bundles
both the Rust GPUI interface and C++ capture engine, linked libraries, and PipeWire/SPA runtime modules, and packages
`dist/Capture_View-<version>-x86_64.AppImage`. The first run downloads linuxdeploy
and its AppImage output plugin from their official GitHub releases into
`build/appimage/tools/`. Subsequent runs reuse those tools.

Double-click the AppImage to open the GUI. Command-line arguments are also
supported. The script does not install anything, run tests, or launch the result.
It is intended for the build system and compatible Linux systems; it does not
bundle the host graphics drivers or kernel capture/loopback modules.

The initial Rust/GPUI migration was built and checked locally. Subsequent builds
and tests are user-run unless explicitly requested. The AppImage script never
runs tests automatically. GPUI is pinned in `Cargo.lock`; a small vendored xattr
compatibility patch keeps its legacy archive dependency compatible with current
Linux libc (see `third-party/xattr/CAPTURE-VIEW-PATCH.md`).

### System tray

Enable **System tray** in Settings to close the window while capture and audio
keep running. Click the tray icon to show or hide the window; right-click for
**Quit**. Opening the AppImage again shows the existing instance.

### Embedded viewer and virtual cameras

**Start viewer** begins capture without changing the current tab. Select Viewer
to see the preview inside the existing GPUI window. On the Capture page, turn off
**Show video in viewer** to hide the local picture while keeping audio playback
and virtual video devices running. It applies immediately. Settings are saved when changed.
The capture engine runs without an SDL window and publishes the newest BGRA
frame through a three-slot shared-memory transport. Overwritten/incomplete
frames are discarded instead of queued. GPUI preserves the video aspect ratio.
The Viewer page has a fullscreen button; **F**, **F11**, and **Escape** operate
fullscreen playback. Other pages remain available while capture and sharing run.

In Settings, choose **Virtual video devices** from 0 (preview only) to 8. With two
outputs, OBS can use **Capture View 1** and Discord can use **Capture View 2**,
while the Viewer page displays the same physical capture. A single prepared
video buffer is written to all outputs. They use v4l2loopback with exclusive
capability reporting for WebRTC compatibility. For sound, select **Capture View
Audio** in the receiving apps. Virtual audio remains available when local
**Play capture audio** is disabled.

The first setup/count change requests system authorization through polkit. Only
`capture-view-device-helper` runs elevated; the GUI and capture engine remain
unprivileged. Setup loads v4l2loopback without unloading other virtual cameras,
creates the named outputs, and grants the desktop user access. It installs a
root-owned helper at `/usr/local/libexec/capture-view-device-helper` and a
`capture-view-virtual-devices-<uid>.service` system service to restore those
outputs after reboot. Subsequent starts reuse them without another prompt.
Choosing zero removes this user's startup service and the unused named outputs.
Close receiving apps before reducing the count if they still hold an output.
The host needs systemd, polkit, and a v4l2loopback module matching its kernel.

The AppImage build script includes the provisioning helper and the updated
Rust frontend/capture engine. Fullscreen/borderless setup switches and the
standalone renderer/VSync controls are not shown for the embedded viewer.
These embedded-viewer changes have not been built or tested by the agent;
validation is user-run as requested.

The windowed player fits a 16:9 video area within the available width and height,
leaving 24 pixels around the player. Source frames use contain scaling rather
than cropping. Fullscreen removes the windowed status/control bar. Its only
overlay is Exit fullscreen: pointer movement or interaction reveals it for two
seconds, followed by a 250 ms fade. F/F11 and Escape continue to work while the
overlay is hidden. Clicking an open dropdown's own trigger closes it; outside
clicks still dismiss its menu.
