{ pkgs ? import <nixpkgs> { } }:

# Non-flake entrypoint for `nix-build` / `nix build -f`.
#
# Uses the local working tree as `src`, so it doesn't need a fixed-output hash.
pkgs.callPackage ./package.nix { src = ./.; }
