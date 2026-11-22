# GPUI 0.2.2: Linux tray lifetime

The Wayland and X11 clients no longer stop the event loop when the last native
window is removed. Capture View retains its root entity independently, recreates
the native window on activation, and calls App::quit explicitly on normal close
or tray Quit. This keeps capture/audio alive without a hidden dummy window.
Upstream license and all other platform behavior are retained.
