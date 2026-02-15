{ lib
, cmake
, coreutils
, extra-cmake-modules
, gawk
, kdePackages
, kf6 ? null
, gnugrep
, procps
, stdenv
, nlohmann_json
, pkg-config
, libxrandr
, systemd
, makeWrapper
, tuxedo-drivers ? null
, src ? ./.   # default to the source tree containing this file
, version ? "0.0.1"
}:

stdenv.mkDerivation {
  pname = "ucc";
  inherit version;

  inherit src;

  dontWrapQtApps = true;

  nativeBuildInputs = [
    cmake
    extra-cmake-modules
    pkg-config
    makeWrapper
  ];

  buildInputs = [
    kdePackages.qtbase
    kdePackages.qtdeclarative
    kdePackages.qtconnectivity
    nlohmann_json
    libxrandr
    systemd
  ] ++ lib.optionals (kdePackages ? qtquickcontrols2) [
    kdePackages.qtquickcontrols2
  ] ++ lib.optionals (kf6 != null) [
    kf6.plasma-framework
    kf6.kwindowsystem
  ] ++ lib.optionals (tuxedo-drivers != null) [
    tuxedo-drivers
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DBUILD_GUI=ON"
    "-DBUILD_TRAY=ON"
    "-DBUILD_WIDGETS=ON"
  ];

  postFixup = ''
    wrapProgram $out/bin/ucc-gui \
      --set QT_QPA_PLATFORM_PLUGIN_PATH "${kdePackages.qtbase}/lib/qt-6/plugins"
    wrapProgram $out/bin/ucc-tray \
      --set QT_QPA_PLATFORM_PLUGIN_PATH "${kdePackages.qtbase}/lib/qt-6/plugins"
    wrapProgram $out/bin/uccd \
      --prefix PATH : "${lib.makeBinPath [ coreutils gawk gnugrep procps ]}"

    # The upstream unit/DBus activation files use /usr/bin paths which do not
    # exist on NixOS. Keep the files installed by CMake, but patch paths to the
    # store so they are usable via `systemd.packages` / `services.dbus.packages`.
    if [ -f "$out/lib/systemd/system/uccd.service" ]; then
      substituteInPlace "$out/lib/systemd/system/uccd.service" \
        --replace-fail "/usr/bin/uccd" "$out/bin/uccd"
    fi
    if [ -f "$out/share/dbus-1/system-services/com.uniwill.uccd.service" ]; then
      substituteInPlace "$out/share/dbus-1/system-services/com.uniwill.uccd.service" \
        --replace-fail "/usr/bin/uccd" "$out/bin/uccd"
    fi
    if [ -f "$out/lib/systemd/system/uccd-sleep.service" ]; then
      substituteInPlace "$out/lib/systemd/system/uccd-sleep.service" \
        --replace-fail "/usr/bin/systemctl" "${systemd}/bin/systemctl"
    fi
  '';

  meta = with lib; {
    description = "Uniwill Control Center - System control application suite";
    homepage = "https://github.com/nanomatters/ucc";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    maintainers = [ ];
  };
}
