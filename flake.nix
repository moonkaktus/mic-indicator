{
  description = "GTK4 layer-shell microphone indicator";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      mkPackage =
        pkgs:
        pkgs.stdenv.mkDerivation {
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
            platforms = pkgs.lib.platforms.linux;
          };
        };

      # Options shared by the NixOS and Home Manager modules.
      mkOptions =
        { lib, pkgs, ... }:
        {
          enable = lib.mkEnableOption "the microphone activity indicator";
          package = lib.mkOption {
            type = lib.types.package;
            default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
            defaultText = lib.literalExpression "inputs.mic-indicator.packages.\${pkgs.stdenv.hostPlatform.system}.default";
            description = "mic-indicator package to use.";
          };
          systemdTarget = lib.mkOption {
            type = lib.types.str;
            default = "graphical-session.target";
            description = "graphical-session target the indicator is bound to.";
          };
        };

      nixosModule =
        { config, lib, pkgs, ... }:
        let
          cfg = config.programs.mic-indicator;
        in
        {
          options.programs.mic-indicator = mkOptions { inherit lib pkgs; };
          config = lib.mkIf cfg.enable {
            environment.systemPackages = [ cfg.package ];
            systemd.user.services.mic-indicator = {
              description = "Microphone activity indicator (GTK4 layer-shell dot)";
              after = [ cfg.systemdTarget ];
              partOf = [ cfg.systemdTarget ];
              wantedBy = [ cfg.systemdTarget ];
              serviceConfig = {
                ExecStart = lib.getExe cfg.package;
                Restart = "on-failure";
                Slice = "session.slice";
              };
            };
          };
        };

      homeManagerModule =
        { config, lib, pkgs, ... }:
        let
          cfg = config.programs.mic-indicator;
        in
        {
          options.programs.mic-indicator = mkOptions { inherit lib pkgs; };
          config = lib.mkIf cfg.enable {
            home.packages = [ cfg.package ];
            systemd.user.services.mic-indicator = {
              Unit = {
                Description = "Microphone activity indicator (GTK4 layer-shell dot)";
                After = [ cfg.systemdTarget ];
                PartOf = [ cfg.systemdTarget ];
              };
              Service = {
                ExecStart = lib.getExe cfg.package;
                Restart = "on-failure";
                Slice = "session.slice";
              };
              Install.WantedBy = [ cfg.systemdTarget ];
            };
          };
        };
    in
    {
      packages = nixpkgs.lib.genAttrs systems (system: {
        default = mkPackage nixpkgs.legacyPackages.${system};
      });

      apps = nixpkgs.lib.genAttrs systems (system: {
        default = {
          type = "app";
          program = nixpkgs.lib.getExe self.packages.${system}.default;
        };
      });

      nixosModules.default = nixosModule;
      homeManagerModules.default = homeManagerModule;
    };
}
