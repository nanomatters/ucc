{ overlay ? null }:
{ config, lib, pkgs, ... }:

let
  cfg = config.services.uccd;
  extraArgsString =
    lib.concatStringsSep " " (map lib.escapeShellArg cfg.extraArgs);
  videoDrivers = config.services.xserver.videoDrivers or [ ];
  nvidiaPackage = lib.attrByPath [ "hardware" "nvidia" "package" ] null config;
  nvidiaBinPath =
    lib.optionalString (nvidiaPackage != null && lib.elem "nvidia" videoDrivers)
      ":${lib.makeBinPath [ nvidiaPackage ]}";
  uccdToolsPath = lib.makeBinPath [
    pkgs.coreutils
    pkgs.gawk
    pkgs.gnugrep
    pkgs.procps
    pkgs.util-linux
    pkgs.which
  ];
in
{
  options.services.uccd = {
    enable = lib.mkEnableOption "Unified Control Center daemon (uccd)";

    package = lib.mkOption {
      type = lib.types.package;
      default =
        if pkgs ? ucc then
          pkgs.ucc
        else
          pkgs.callPackage ../package.nix { src = ../.; };
      defaultText = "pkgs.ucc (or callPackage ../package.nix)";
      description = "The `ucc` package providing `uccd`.";
    };

    extraArgs = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
      example = [ "--verbose" ];
      description = "Extra arguments passed to `uccd --start`.";
    };

    enableSleepHandler = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Restart `uccd` on suspend/hibernate via a small systemd helper unit.";
    };
  };

  config = lib.mkIf cfg.enable {
    nixpkgs.overlays = lib.mkIf (overlay != null) [ overlay ];

    # Add the package to the system profile so plasmashell can discover the
    # Plasma applet .so (via QT_PLUGIN_PATH) and the QML module
    # (com.uniwill.ucc.private, via QML2_IMPORT_PATH / QT_QML_IMPORT_PATH).
    environment.systemPackages = [ cfg.package ];

    # Install the Polkit policy so that unprivileged GUI users can
    # authenticate for privileged D-Bus operations.
    security.polkit.enable = true;

    # Make the D-Bus service config and Polkit policy discoverable at the
    # system level.  environment.systemPackages alone is not sufficient for
    # system-bus configs and Polkit action definitions.
    services.dbus.packages = [ cfg.package ];
    environment.pathsToLink = [ "/share/polkit-1" ];

    systemd.tmpfiles.rules = [
      "d /etc/ucc 0755 root root - -"
    ];

    systemd.services.uccd = {
      description = "Unified Control Center Daemon";
      documentation = [ "man:uccd(8)" ];
      after = [ "dbus.service" ];
      requires = [ "dbus.service" ];
      wantedBy = [ "multi-user.target" ];

      serviceConfig = {
        Type = "dbus";
        BusName = "com.uniwill.uccd";
        ExecStartPre = "-${cfg.package}/bin/uccd --stop";
        ExecStart =
          "${cfg.package}/bin/uccd --start"
          + lib.optionalString (cfg.extraArgs != [ ]) " ${extraArgsString}";
        Environment = [
          "PATH=/run/wrappers/bin:/run/current-system/sw/bin:${uccdToolsPath}${nvidiaBinPath}"
        ];
        Restart = "on-failure";
        RestartSec = "5s";
        TimeoutStopSec = "10s";
        User = "root";
        StandardOutput = "journal";
        StandardError = "journal";
        SyslogIdentifier = "uccd";

        PrivateTmp = true;
        ProtectSystem = "strict";
        ProtectHome = true;
        NoNewPrivileges = true;
        ReadWritePaths = [ "/etc/ucc" "/run" ];
      };
    };

    systemd.services.uccd-sleep = lib.mkIf cfg.enableSleepHandler {
      description = "Unified Control Center Daemon Sleep Handler";
      documentation = [ "man:uccd(8)" ];
      after = [
        "suspend.target"
        "hibernate.target"
        "hybrid-sleep.target"
        "suspend-then-hibernate.target"
      ];
      wantedBy = [
        "suspend.target"
        "hibernate.target"
        "hybrid-sleep.target"
        "suspend-then-hibernate.target"
      ];

      serviceConfig = {
        Type = "oneshot";
        ExecStart = "${config.systemd.package}/bin/systemctl restart uccd.service";
      };
    };
  };
}
