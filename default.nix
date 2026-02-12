{ lib
, cmake
, extra-cmake-modules
, qt6
, kf6
, stdenv
, fetchFromGitHub
, pkg-config
, sdbus-c++
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
    sdbus-c++
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
