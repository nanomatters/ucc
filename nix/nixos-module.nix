{ overlay ? null }:
{ config, lib, pkgs, ... }:

let
  cfg = config.services.uccd;
  extraArgsString =
    lib.concatStringsSep " " (map lib.escapeShellArg cfg.extraArgs);
in
{
  options.services.uccd = {
    enable = lib.mkEnableOption "Uniwill Control Center daemon (uccd)";

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

    systemd.tmpfiles.rules = [
      "d /etc/ucc 0755 root root - -"
      "d /etc/ucc/autosave 0755 root root - -"
    ];

    systemd.services.uccd = {
      description = "Uniwill Control Center Daemon";
      documentation = [ "man:uccd(8)" ];
      after = [ "dbus.service" ];
      requires = [ "dbus.service" ];
      wantedBy = [ "multi-user.target" ];

      path = [
        pkgs.coreutils
        pkgs.gawk
        pkgs.gnugrep
        pkgs.procps
        pkgs.util-linux
        pkgs.which
      ];

      serviceConfig = {
        Type = "simple";
        ExecStart =
          "${cfg.package}/bin/uccd --start"
          + lib.optionalString (cfg.extraArgs != [ ]) " ${extraArgsString}";
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
      description = "Uniwill Control Center Daemon Sleep Handler";
      documentation = [ "man:uccd(8)" ];
      after = [
        "suspend.target"
        "hibernate.target"
        "hybrid-sleep.target"
        "suspend-then-hibernate.target"
      ];
      requires = [ "uccd.service" ];
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
