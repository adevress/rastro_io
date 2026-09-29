{
  description = "rastro-io — I/O toolkit for radio astronomy";

  inputs = {
    # Same nixpkgs release as astro-nix, for ABI-consistent dependencies.
    nixpkgs.url = "github:NixOS/nixpkgs/25.11";
    # casacore is not part of nixpkgs 25.11; astro-nix packages it.
    astro-nix.url = "github:adevress/astro-nix";
  };

  outputs =
    { self, nixpkgs, astro-nix }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      # Everything lives in default.nix; the flake only forwards the inputs.
      mkResult =
        system:
        import ./default.nix {
          inherit system;
          pkgs = nixpkgs.legacyPackages.${system};
          astro-pkgs = astro-nix.legacyPackages.${system};
        };
    in
    {
      packages = nixpkgs.lib.genAttrs systems (system: {
        default = (mkResult system).rastro_io;
        rastro_io = (mkResult system).rastro_io;
      });

      devShells = nixpkgs.lib.genAttrs systems (system: {
        default = (mkResult system).devShell;
      });
    };
}
