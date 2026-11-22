#!/usr/bin/env bash
# Build on the target Linux system. No sudo, installation, or test execution.
# Packaging reference: https://docs.appimage.org/packaging-guide/from-source/linuxdeploy-user-guide.html
set -Eeuo pipefail
trap 'printf "AppImage build failed at line %s.\n" "$LINENO" >&2' ERR

if [[ ${1:-} == --help ]]; then
  cat <<'HELP'
Usage: ./build-appimage.sh

Builds capture-view and packages a GUI-launching AppImage in dist/.
Downloads linuxdeploy and its AppImage output plugin on the first run.
Does not install packages, run tests, or launch the result.

Optional environment variables:
  JOBS=4             Parallel compilation jobs (default: 4)
  APPIMAGE_VERSION   Override the version embedded in the filename
HELP
  exit 0
fi
[[ $# == 0 ]] || { printf 'Use --help for usage.\n' >&2; exit 2; }
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || {
  printf 'This script targets x86_64 Linux.\n' >&2; exit 1;
}

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work_dir="$repo_dir/build/appimage"
tools_dir="$work_dir/tools"
output_dir="$repo_dir/dist"
jobs=${JOBS:-4}
export CARGO_BUILD_JOBS="$jobs"
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { printf 'JOBS must be a positive integer.\n' >&2; exit 2; }

for tool in cmake ninja c++ cargo pkg-config curl install cp find sha256sum; do
  command -v "$tool" >/dev/null || { printf 'Missing build tool: %s\n' "$tool" >&2; exit 1; }
done
pkg-config --exists sdl3 libturbojpeg libpipewire-0.3 libspa-0.2 libavcodec libavutil libswscale || {
  printf 'Missing development libraries. On CachyOS/Arch install:\n' >&2
  printf 'sudo pacman -S --needed base-devel cmake ninja pkgconf curl pipewire sdl3 libjpeg-turbo ffmpeg libglvnd rust clang libxkbcommon libxkbcommon-x11 fontconfig vulkan-icd-loader\n' >&2
  exit 1
}

version=${APPIMAGE_VERSION:-$(sed -n '/^[[:space:]]*VERSION /{s/^[[:space:]]*VERSION //;p;q;}' "$repo_dir/CMakeLists.txt")}
[[ $version =~ ^[A-Za-z0-9][A-Za-z0-9._+-]*$ ]] || {
  printf 'Invalid AppImage version: %s\n' "$version" >&2; exit 2;
}
mkdir -p "$work_dir" "$tools_dir" "$output_dir"
# A fresh staging directory prevents stale libraries surviving later rebuilds.
app_dir=$(mktemp -d "$work_dir/AppDir.XXXXXX")
printf 'Staging directory: %s\n' "$app_dir"

fetch_tool() {
  local name=$1 url=$2 destination="$tools_dir/$1"
  if [[ ! -x $destination ]]; then
    printf 'Downloading %s\n' "$name"
    curl --fail --location --retry 3 "$url" -o "$destination.download"
    chmod +x "$destination.download"
    mv -- "$destination.download" "$destination"
  fi
}
fetch_tool linuxdeploy-x86_64.AppImage \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch_tool linuxdeploy-plugin-appimage.AppImage \
  https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/continuous/linuxdeploy-plugin-appimage-x86_64.AppImage

cmake -S "$repo_dir" -B "$work_dir/cmake" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
# Build only the app; do not build or run the project's test targets.
cmake --build "$work_dir/cmake" --target capture-view capture-view-gpui capture-view-device-helper --parallel "$jobs"
DESTDIR="$app_dir" cmake --install "$work_dir/cmake"

# PipeWire loads these at runtime rather than through normal ELF dependencies.
pipewire_modules=$(pkg-config --variable=moduledir libpipewire-0.3)
spa_plugins=$(pkg-config --variable=plugindir libspa-0.2)
[[ -d $pipewire_modules && -d $spa_plugins ]] || {
  printf 'Cannot locate PipeWire / SPA runtime modules.\n' >&2; exit 1;
}
mkdir -p "$app_dir/usr/lib" "$app_dir/usr/share"
cp -a -- "$pipewire_modules" "$app_dir/usr/lib/pipewire-0.3"
cp -a -- "$spa_plugins" "$app_dir/usr/lib/spa-0.2"
pipewire_data=$(pkg-config --variable=prefix libpipewire-0.3)/share/pipewire
cp -a -- "$pipewire_data" "$app_dir/usr/share/pipewire"

cat > "$work_dir/AppRun" <<'APPRUN'
#!/usr/bin/env bash
set -e
bundle_dir=$(cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
export LD_LIBRARY_PATH="$bundle_dir/usr/lib:$bundle_dir/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_DATA_DIRS="$bundle_dir/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export PIPEWIRE_MODULE_DIR="$bundle_dir/usr/lib/pipewire-0.3"
export SPA_PLUGIN_DIR="$bundle_dir/usr/lib/spa-0.2"
export PIPEWIRE_CONFIG_DIR="$bundle_dir/usr/share/pipewire"
# The authorization helper re-enters the archive rather than the user-only FUSE mount.
if [[ ${1:-} == --setup-virtual-devices ]]; then
  [[ $# == 2 ]] || exit 2
  exec "$bundle_dir/usr/bin/capture-view-device-helper" --count "$2"
fi
# Double-click opens the Rust / GPUI interface; explicit arguments target the engine.
if [[ $# == 0 ]]; then
  exec "$bundle_dir/usr/bin/capture-view-gui"
fi
exec "$bundle_dir/usr/bin/capture-view" "$@"
APPRUN
chmod +x "$work_dir/AppRun"

export PATH="$tools_dir:$PATH"
export ARCH=x86_64
export VERSION="$version"
export OUTPUT="$output_dir/Capture_View-$version-x86_64.AppImage"
# Packaging tools can run without FUSE; leave the user's app runtime untouched.
export APPIMAGE_EXTRACT_AND_RUN=1
# Use unstripped libraries: older bundled strip tools may reject modern ELF files.
export NO_STRIP=1

cd -- "$output_dir"
"$tools_dir/linuxdeploy-x86_64.AppImage" \
  --appdir "$app_dir" \
  --executable "$app_dir/usr/bin/capture-view" \
  --executable "$app_dir/usr/bin/capture-view-gui" \
  --executable "$app_dir/usr/bin/capture-view-device-helper" \
  --desktop-file "$app_dir/usr/share/applications/capture-view.desktop" \
  --icon-file "$app_dir/usr/share/icons/hicolor/512x512/apps/capture-view.png" \
  --custom-apprun "$work_dir/AppRun" \
  --deploy-deps-only "$app_dir/usr/lib/pipewire-0.3" \
  --deploy-deps-only "$app_dir/usr/lib/spa-0.2" \
  --output appimage
chmod +x "$OUTPUT"
sha256sum "$(basename -- "$OUTPUT")" > "$OUTPUT.sha256"
printf '\nCreated: %s\nDouble-click it to open Capture View.\n' "$OUTPUT"
printf 'No tests were run. Staging files remain at: %s\n' "$app_dir"
