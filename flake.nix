{
  description = "GTK4 layer-shell microphone indicator";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};

      micIndicator = pkgs.stdenv.mkDerivation {
        pname = "mic-indicator";
        version = "2.0.0";
        src = ./.;

        nativeBuildInputs = [ pkgs.pkg-config ];
        buildInputs = [
          pkgs.gtk4
          pkgs.gtk4-layer-shell
          pkgs.pipewire
        ];

        buildPhase = ''
          gcc -Wall -Wextra -o mic-indicator mic-indicator.c \
            $(pkg-config --cflags --libs gtk4 gtk4-layer-shell-0 libpipewire-0.3)
        '';

        installPhase = ''
          install -Dm755 mic-indicator -t $out/bin
        '';

        meta = {
          description = "Microphone activity dot on a GTK4 layer-shell overlay";
          mainProgram = "mic-indicator";
          platforms = [ system ];
        };
      };
    in
    {
      packages.${system}.default = micIndicator;

      apps.${system}.default = {
        type = "app";
        program = pkgs.lib.getExe micIndicator;
      };
    };
}
