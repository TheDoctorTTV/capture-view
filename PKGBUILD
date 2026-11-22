pkgname=capture-view
pkgver=0.2.1
pkgrel=5
pkgdesc='Low-latency HDMI USB capture-card viewer'
arch=('x86_64')
url='https://github.com/D35P4C1T0/capture-view'
license=('MIT')
# makepkg records the linked FFmpeg SONAME versions in the generated package.
# A future ABI change will require rebuilding instead of leaving a broken binary.
depends=('pipewire' 'sdl3' 'libjpeg-turbo' 'libglvnd' 'ffmpeg' 'libxkbcommon' 'libxkbcommon-x11' 'fontconfig' 'vulkan-icd-loader' 'curl'
         'libavcodec.so' 'libavutil.so' 'libswscale.so')
makedepends=('cmake' 'ninja' 'pkgconf' 'rust' 'clang' 'vulkan-headers' 'shaderc')
optdepends=('v4l2loopback-dkms: virtual capture devices' 'polkit: authorize virtual-device setup')
source=()
sha256sums=()

build() {
  cmake -S "$startdir" -B "$srcdir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build "$srcdir/build"
}

check() {
  ctest --test-dir "$srcdir/build" --output-on-failure
}

package() {
  DESTDIR="$pkgdir" cmake --install "$srcdir/build"
  install -Dm644 "$startdir/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
