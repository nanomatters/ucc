{
  lib,
  cmake,
  coreutils,
  gawk,
  kdePackages,
  kf6 ? null,
  gnugrep,
  procps,
  util-linux,
  which,
  stdenv,
  nlohmann_json,
  pkg-config,
  libxrandr,
  libxcb-cursor,
  systemd,
  makeWrapper,
  tuxedo-drivers ? null,
  src ? ./., # default to the source tree containing this file
  version ? "0.0.1",
}:

stdenv.mkDerivation {
  pname = "ucc";
  inherit version;

  inherit src;

  nativeBuildInputs = [
    cmake
    kdePackages.extra-cmake-modules
    pkg-config
    makeWrapper
    kdePackages.wrapQtAppsHook
  ];

  buildInputs = [
    kdePackages.qtbase
    kdePackages.qtcharts
    kdePackages.qtdeclarative
    kdePackages.qtconnectivity
    kdePackages.qtwayland
    kdePackages.plasma-integration
    kdePackages.breeze
    kdePackages.kconfig
    kdePackages.kcoreaddons
    kdePackages.kpackage
    kdePackages.kwindowsystem
    kdePackages.libplasma
    nlohmann_json
    libxrandr
    libxcb-cursor
    systemd
  ]
  ++ lib.optionals (tuxedo-drivers != null) [
    tuxedo-drivers
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DBUILD_GUI=ON"
    "-DBUILD_TRAY=ON"
    "-DBUILD_GNOME=ON"
    "-DBUILD_CLI=ON"
    "-DUCC_AUTOSTART_DIR=share/xdg/autostart"
  ];

  postFixup = ''
    wrapProgram $out/bin/uccd \
      --prefix PATH : "${
        lib.makeBinPath [
          coreutils
          gawk
          gnugrep
          procps
          util-linux
          which
        ]
      }"

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
      if ! grep -q '^SystemdService=uccd\\.service$' "$out/share/dbus-1/system-services/com.uniwill.uccd.service"; then
        sed -i '/^Name=com\\.uniwill\\.uccd$/a SystemdService=uccd.service' \
          "$out/share/dbus-1/system-services/com.uniwill.uccd.service"
      fi
    fi
    for svc in uccd-pre-sleep.service uccd-sleep.service; do
      if [ -f "$out/lib/systemd/system/$svc" ]; then
        substituteInPlace "$out/lib/systemd/system/$svc" \
          --replace-fail "/usr/bin/systemctl" "${systemd}/bin/systemctl"
      fi
    done

    if [ -f "$out/lib/systemd/system/uccd.service" ]; then
      if ! grep -q '^Type=dbus$' "$out/lib/systemd/system/uccd.service"; then
        sed -i 's/^Type=simple$/Type=dbus/' "$out/lib/systemd/system/uccd.service"
      fi
      if ! grep -q '^BusName=com\\.uniwill\\.uccd$' "$out/lib/systemd/system/uccd.service"; then
        sed -i '/^Type=dbus$/a BusName=com.uniwill.uccd' "$out/lib/systemd/system/uccd.service"
      fi
    fi
  '';

  meta = with lib; {
    description = "Uniwill Control Center - System control application suite (daemon, GUI, CLI, Plasma applet)";
    homepage = "https://github.com/nanomatters/ucc";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    maintainers = [ ];
  };
}
