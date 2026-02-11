# Maintainer: Uniwill <support@uniwill.io>

pkgbase=ucc
pkgname=(ucc ucc-gui ucc-tray)
pkgver=0.1.0
pkgrel=1
pkgdesc='Uniwill Control Center - System control application suite'
arch=('x86_64')
url='https://github.com/nanomatters/ucc'
license=('GPL3')
depends=('qt6-base' 'systemd' 'sdbus-c++' 'tuxedo-drivers')
makedepends=('cmake' 'extra-cmake-modules' 'qt6-declarative' 'qt6-connectivity'
             'kf6-plasma')
source=("${pkgbase}-${pkgver}.tar.gz::https://github.com/nanomatters/ucc/archive/v${pkgver}.tar.gz")
sha256sums=('SKIP')

build() {
  cd "${srcdir}/${pkgbase}-${pkgver}"
  
  cmake -B build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_GUI=ON \
    -DBUILD_TRAY=ON \
    -DBUILD_WIDGETS=ON \
    -Wno-dev
  
  cmake --build build
}

package_ucc() {
  pkgdesc='Uniwill Control Center - System control daemon'
  depends=('systemd' 'sdbus-c++' 'qt6-base')
  # Hardware kernel/user-mode drivers required for proper hardware control
  depends+=('tuxedo-drivers')
  
  cd "${srcdir}/${pkgbase}-${pkgver}"
  
  DESTDIR="${pkgdir}" cmake --install build --component uccd
  
  # Install systemd service files
  install -Dm644 "uccd/uccd.service" "${pkgdir}/usr/lib/systemd/system/uccd.service"
  install -Dm644 "uccd/uccd-sleep.service" "${pkgdir}/usr/lib/systemd/system/uccd-sleep.service"
  
  # Create configuration directory
  install -d "${pkgdir}/etc/ucc"
  
  # Install daemon executable
  install -Dm755 build/uccd/uccd "${pkgdir}/usr/bin/uccd"
  
  # Install D-Bus library
  install -Dm755 build/libucc-dbus/libucc-dbus.so* "${pkgdir}/usr/lib/"
  install -Dm644 libucc-dbus/CommonTypes.hpp "${pkgdir}/usr/include/ucc/"
}

package_ucc-gui() {
  pkgdesc='Uniwill Control Center - Graphical user interface'
  depends=('ucc' 'qt6-base' 'kf6-kwindowsystem')
  
  cd "${srcdir}/${pkgbase}-${pkgver}/build"
  
  install -Dm755 ucc-gui/ucc-gui "${pkgdir}/usr/bin/ucc-gui"
}

package_ucc-tray() {
  pkgdesc='Uniwill Control Center - System tray applet'
  depends=('ucc' 'qt6-base')
  
  cd "${srcdir}/${pkgbase}-${pkgver}/build"
  
  install -Dm755 ucc-tray/ucc-tray "${pkgdir}/usr/bin/ucc-tray"
  install -Dm644 ../ucc-tray/ucc-tray.desktop "${pkgdir}/usr/share/applications/ucc-tray.desktop"
}
