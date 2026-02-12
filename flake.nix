{
  description = "Uniwill Control Center (UCC)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { self, nixpkgs, flake-utils }:
    let
      overlay = final: prev: {
        ucc = final.callPackage ./package.nix { src = self; };
      };
    in
    rec {
      overlays.default = overlay;
      nixosModules.uccd = import ./nix/nixos-module.nix { inherit overlay; };
      nixosModules.default = nixosModules.uccd;
    }
    // flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ overlay ];
        };
      in
      {
        packages.ucc = pkgs.ucc;
        packages.default = pkgs.ucc;

        devShells.default = pkgs.mkShell {
          inputsFrom = [ pkgs.ucc ];
          packages = with pkgs; [
            cmake
            extra-cmake-modules
            pkg-config
          ];
        };
      }
    );
}
