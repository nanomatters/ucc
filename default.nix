{ lib
, cmake
, extra-cmake-modules
, qt6
, kf6
, stdenv
, fetchFromGitHub
, pkg-config
, libxrandr
, systemd
, makeWrapper
}:

stdenv.mkDerivation rec {
  pname = "ucc";
  version = "0.1.0";

  src = fetchFromGitHub {
    owner = "nanomatters";
    repo = "ucc";
    rev = "v${version}";
    hash = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
  };

  nativeBuildInputs = [
    cmake
    extra-cmake-modules
    pkg-config
    makeWrapper
  ];

  buildInputs = [
    qt6.full
    kf6.plasma-framework
    kf6.kwindowsystem
    libxrandr
    systemd
    # Runtime kernel/user drivers required for hardware control
    tuxedo-drivers
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DBUILD_GUI=ON"
    "-DBUILD_TRAY=ON"
    "-DBUILD_WIDGETS=ON"
  ];

  postInstall = ''
    # Create configuration directory
    mkdir -p $out/etc/ucc
    
    # Install systemd service files
    mkdir -p $out/lib/systemd/system
    cp uccd/uccd.service $out/lib/systemd/system/
    cp uccd/uccd-sleep.service $out/lib/systemd/system/

    # Install D-Bus service and policy files
    mkdir -p $out/share/dbus-1/system-services
    cp uccd/com.uniwill.uccd.service $out/share/dbus-1/system-services/
    mkdir -p $out/share/dbus-1/system.d
    cp uccd/com.uniwill.uccd.conf $out/share/dbus-1/system.d/

    # Install desktop files and icons
    mkdir -p $out/share/applications
    cp ucc-gui/ucc-gui.desktop $out/share/applications/
    cp ucc-tray/ucc-tray.desktop $out/share/applications/

    mkdir -p $out/share/icons/hicolor/16x16/apps $out/share/icons/hicolor/24x24/apps \
             $out/share/icons/hicolor/32x32/apps $out/share/icons/hicolor/48x48/apps \
             $out/share/icons/hicolor/64x64/apps $out/share/icons/hicolor/128x128/apps \
             $out/share/icons/hicolor/scalable/apps
    cp icons/ucc-gui_16.png $out/share/icons/hicolor/16x16/apps/ucc-gui.png
    cp icons/ucc-gui_24.png $out/share/icons/hicolor/24x24/apps/ucc-gui.png
    cp icons/ucc-gui_32.png $out/share/icons/hicolor/32x32/apps/ucc-gui.png
    cp icons/ucc-gui_48.png $out/share/icons/hicolor/48x48/apps/ucc-gui.png
    cp icons/ucc-gui_64.png $out/share/icons/hicolor/64x64/apps/ucc-gui.png
    cp icons/ucc-gui_128.png $out/share/icons/hicolor/128x128/apps/ucc-gui.png
    cp icons/ucc-gui.svg $out/share/icons/hicolor/scalable/apps/ucc-gui.svg

    cp icons/ucc-tray_16.png $out/share/icons/hicolor/16x16/apps/ucc-tray.png
    cp icons/ucc-tray_24.png $out/share/icons/hicolor/24x24/apps/ucc-tray.png
    cp icons/ucc-tray_32.png $out/share/icons/hicolor/32x32/apps/ucc-tray.png
    cp icons/ucc-tray_48.png $out/share/icons/hicolor/48x48/apps/ucc-tray.png
    cp icons/ucc-tray_64.png $out/share/icons/hicolor/64x64/apps/ucc-tray.png
    cp icons/ucc-tray_128.png $out/share/icons/hicolor/128x128/apps/ucc-tray.png
    cp icons/ucc-tray.svg $out/share/icons/hicolor/scalable/apps/ucc-tray.svg

    mkdir -p $out/share/pixmaps
    cp icons/ucc-gui.svg $out/share/pixmaps/ucc-gui.svg
    cp icons/ucc-tray.svg $out/share/pixmaps/ucc-tray.svg
  '';

  postFixup = ''
    wrapProgram $out/bin/ucc-gui \
      --set QT_QPA_PLATFORM_PLUGIN_PATH "${qt6.qtbase.bin}/lib/qt-6/plugins"
    wrapProgram $out/bin/ucc-tray \
      --set QT_QPA_PLATFORM_PLUGIN_PATH "${qt6.qtbase.bin}/lib/qt-6/plugins"
  '';

  meta = with lib; {
    description = "Uniwill Control Center - System control application suite";
    homepage = "https://github.com/nanomatters/ucc";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    maintainers = [ ];
  };
}
