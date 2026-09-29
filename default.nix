{
  # System to build for. The flake passes this explicitly.
  system,

  # nixpkgs for the given system.
  pkgs,

  # astro-nix package set for the given system. Provides casacore, which is not
  # part of nixpkgs 25.11.
  astro-pkgs,
}:

let
  # Dependencies provided by astro-nix (casacore, ...).
  #
  # HighFive is overridden to its latest upstream release (3.3.0); the version
  # shipped by nixpkgs 25.11 is older.
  buildInputs =
    (with pkgs; [
      (highfive.overrideAttrs (old: rec {
        version = "3.3.0";
        src = fetchFromGitHub {
          owner = "highfive-devs";
          repo = "highfive";
          rev = "v${version}";
          hash = "sha256-BuDvoQgMdZIDHYwXqigM78DQ+WtT+K0FdXERMUjmXc0=";
        };
      }))
      hdf5
      cxxopts
    ])
    ++ [ astro-pkgs.casacore ];

  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    gcc
    clang-tools
    pkg-config
  ];
in
{
  rastro_io = pkgs.stdenv.mkDerivation {
    pname = "rastro-io";
    version = "0.1.0";

    src = pkgs.lib.cleanSourceWith {
      src = ./.;
      filter =
        path: type:
        let
          base = baseNameOf (toString path);
        in
          base != "build" && !(pkgs.lib.hasPrefix "build-" base) && base != "result";
    };

    inherit nativeBuildInputs buildInputs;

    cmakeFlags = [ "-DCMAKE_BUILD_TYPE=RelWithDebInfo" ];
  };

  devShell = pkgs.mkShell {
    packages = nativeBuildInputs ++ buildInputs;
  };
}