# Microphone indicator (GTK4 layer-shell)

A 10px dot in the top-right corner that appears while an application is using
the microphone — green when the default source is unmuted, peach when muted.
It is a layer-shell overlay, so it needs no bar and reserves no screen space.

Single file: `mic-indicator.c` (detection **and** rendering in one process).
A native libpipewire client: no `pw-dump`/`pw-mon` subprocesses, no polling —
Node/Link/Metadata events drive everything, and a PipeWire restart is handled
by reconnecting automatically.

## Dependencies

- `gtk4`
- `gtk4-layer-shell`
- `libpipewire` (with SPA headers)

On NixOS, e.g.:

```nix
environment.systemPackages = with pkgs; [ gtk4 gtk4-layer-shell pipewire ];
```

## Run

From this directory:

```sh
nix run .
```

(or from the parent: `nix run ./mic-indicator`)

> If this directory lives inside a git repo, its files must be git-tracked
> for Nix to see them (`git add mic-indicator`).

Autostart under niri (`~/.config/niri/config.kdl`):

```kdl
// if `mic-indicator` is on PATH (e.g. from your system config):
spawn-at-startup "mic-indicator"
```

Or as a systemd user service (see `examples/mic-indicator.service`):

```sh
mkdir -p ~/.config/systemd/user
cp examples/mic-indicator.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now mic-indicator
```

## NixOS / Home Manager

The flake exposes `nixosModules.default` and `homeManagerModules.default`, so
there is no need to hand-write the unit:

```nix
# flake.nix
inputs.mic-indicator.url = "github:moonkaktus/mic-indicator";
inputs.mic-indicator.inputs.nixpkgs.follows = "nixpkgs";
```

```nix
# NixOS configuration (or use homeManagerModules.default for Home Manager)
{
  imports = [ inputs.mic-indicator.nixosModules.default ];
  programs.mic-indicator.enable = true;
}
```

The user service binds to `graphical-session.target`; override the target with
`programs.mic-indicator.systemdTarget` (e.g. `niri.service`) and the package
with `programs.mic-indicator.package`.

## Tweaks

- Position: change the `gtk_layer_set_anchor(...)` calls and/or add margins
  with `gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, px)`.
- Colour: `GREEN` / `PEACH` constants.
- Detection also triggers on non-default microphones; monitor sources
  (`.monitor` / sink-linked captures) are excluded.
- Footprint: the program selects GTK's cairo (software) renderer itself — a
  20x20 dot needs no GPU. That skips the Vulkan stack (~38 MB less RSS, fewer
  threads/fds) and avoids the `VK_SUBOPTIMAL_KHR` warning. Set `GSK_RENDERER`
  explicitly to override.
- State transitions are printed to stdout (`off`/`active`/`muted`) — visible
  in `journalctl --user -u mic-indicator` when run as a service.
