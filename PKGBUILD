# Maintainer: Uniwill <support@uniwill.io>

pkgname=ucc
pkgbase=ucc
pkgver=0.1.0
pkgrel=1
pkgdesc='Uniwill Control Center - System control application suite'
arch=('x86_64')
url='https://github.com/nanomatters/ucc'
license=('GPL3')
depends=('qt6-base' 'systemd' 'tuxedo-drivers')
makedepends=('cmake' 'extra-cmake-modules' 'qt6-declarative' 'qt6-connectivity'
             'kf6-plasma')
source=("${pkgbase}-${pkgver}.tar.gz::https://github.com/nanomatters/ucc/archive/v${pkgver}.tar.gz")
sha256sums=('SKIP')

build() {
  cd "${srcdir}/${pkgbase}-${pkgver}"
  cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_GUI=ON \
    -DBUILD_TRAY=ON \
    -DBUILD_WIDGETS=ON \
    -Wno-dev
  cmake --build build
}


package() {
  cd "${srcdir}/${pkgbase}-${pkgver}"

  # Install everything via cmake
  DESTDIR="${pkgdir}" cmake --install build

  # Install systemd service files
  install -Dm644 "uccd/uccd.service" "${pkgdir}/usr/lib/systemd/system/uccd.service"
  install -Dm644 "uccd/uccd-sleep.service" "${pkgdir}/usr/lib/systemd/system/uccd-sleep.service"

  # Create configuration directory
  install -d "${pkgdir}/etc/ucc"

  # Install daemon executable
  install -Dm755 build/uccd/uccd "${pkgdir}/usr/bin/uccd"

  # Install D-Bus library and header
  install -Dm755 build/libucc-dbus/libucc-dbus.so* "${pkgdir}/usr/lib/"
  install -Dm644 libucc-dbus/CommonTypes.hpp "${pkgdir}/usr/include/ucc/"

  # Install GUI executable
  install -Dm755 build/ucc-gui/ucc-gui "${pkgdir}/usr/bin/ucc-gui"
  install -Dm644 ucc-gui/ucc-gui.desktop "${pkgdir}/usr/share/applications/ucc-gui.desktop"

  # Install tray executable and desktop file
  install -Dm755 build/ucc-tray/ucc-tray "${pkgdir}/usr/bin/ucc-tray"
  install -Dm644 ucc-tray/ucc-tray.desktop "${pkgdir}/usr/share/applications/ucc-tray.desktop"

  # Install icons (scalable SVG once + PNGs per size)
  install -Dm644 icons/ucc-gui.svg "${pkgdir}/usr/share/icons/hicolor/scalable/apps/ucc-gui.svg"
  install -Dm644 icons/ucc-tray.svg "${pkgdir}/usr/share/icons/hicolor/scalable/apps/ucc-tray.svg"
  for size in 16 24 32 48 64 128; do
    install -Dm644 icons/ucc-gui_${size}.png "${pkgdir}/usr/share/icons/hicolor/${size}x${size}/apps/ucc-gui.png"
    install -Dm644 icons/ucc-tray_${size}.png "${pkgdir}/usr/share/icons/hicolor/${size}x${size}/apps/ucc-tray.png"
  done
  install -Dm644 icons/ucc-gui.svg "${pkgdir}/usr/share/pixmaps/ucc-gui.svg"
  install -Dm644 icons/ucc-tray.svg "${pkgdir}/usr/share/pixmaps/ucc-tray.svg"
}
