# Hetzner NixOS builder fleet

A self-contained flake that defines a fleet of NixOS remote builders on Hetzner
Cloud, deployed entirely from Claude Code web sessions. It will later be used to
load-test the Nix `ssh-ng://` daemon worker protocol with real builds.

## Why it's built this way

Claude Code web sessions run in a sandbox whose only egress is Anthropic's
HTTP/HTTPS proxy — **there is no outbound SSH (port 22)**. So the usual
SSH-based deploy tools (`nixos-anywhere`, `nixos-rebuild --target-host`,
`colmena`, `deploy-rs`) can't run from the sandbox.

Instead this uses a **GitOps pull model**: each builder runs
[`comin`](https://github.com/nlewo/comin), which polls this repo over HTTPS and
rebuilds itself to `nixosConfigurations.<hostname>` on every new commit. The web
session only has to edit files and `git push` — which already works through the
GitHub proxy.

```
web session  --git push-->  GitHub  <--HTTPS poll--  comin on each builder --> nixos-rebuild switch
web session  <--commit status (GitHub MCP)--  report-deploy-status on each builder
```

## Layout

| File            | Purpose                                                              |
| --------------- | -------------------------------------------------------------------- |
| `flake.nix`     | `nixosConfigurations.<host>` for each builder; inputs (nixpkgs/comin/disko) |
| `builder.nix`   | The deployable config: nix-daemon builder role, `comin`, SSH, status reporter |
| `disko.nix`     | Deterministic disk layout (BIOS/GRUB, ext4 root) for reproducible installs |
| `keys/ci-builder.pub` | Authorized key for the `nixremote` ssh-ng:// build user (populate this) |
| `provision.sh`  | Sandbox-runnable bootstrap via the Hetzner HTTPS API (no SSH)        |

## One-time setup

1. **Hetzner**: create a read/write API token (Console → Security → API Tokens)
   and upload an SSH key to the project (used only for provisioning/recovery).
2. **Web environment** (in the claude.ai/code environment dialog):
   - Network access **Custom**: add `api.hetzner.cloud` and keep the default
     package-manager allowlist. (`git push` needs nothing extra.)
   - Environment variable `HCLOUD_TOKEN=<token>`. Note: environment variables are
     visible to anyone who can edit the environment — there is no secret store yet.
   - Setup script: install Nix + enable flakes (see `web-setup.sh`) so the
     sandbox can evaluate the config before pushing.
3. **Builder keys/secrets** (delivered to the machines, not committed):
   - Populate `keys/ci-builder.pub` with the load-test client's public key.
   - If `Takishima/nix` is private, give `comin` a read-only token at
     `/run/keys/comin-github-token` and uncomment `auth.access_token_path` in
     `builder.nix`.
   - Optional: a `state:write` token at `/run/keys/github-status-token` so the
     builder reports deploy convergence back as a GitHub commit status.

## Bootstrap (turn fresh servers into NixOS builders)

Recommended: create one golden image, then clone it.

```bash
# First time: convert a fresh server to NixOS, finish the flake switch once,
# then snapshot it in the Hetzner console.
HETZNER_SSH_KEY=my-key COUNT=1 ./provision.sh           # infect mode

# Steady state: clone the snapshot — instant, reproducible builders.
HETZNER_SSH_KEY=my-key HETZNER_IMAGE=<snapshot-id> COUNT=2 ./provision.sh
```

`provision.sh` uses only the Hetzner HTTPS API, so it runs from a web session.

## Deploy config changes (the everyday loop)

1. Edit `builder.nix` (or add/remove hostnames in `flake.nix`).
2. Validate locally: `nix flake check` and
   `nix eval .#nixosConfigurations.nix-builder-01.config.system.build.toplevel.drvPath`
   (evaluation is light; the full build runs on the builder).
3. `git push` to the deploy branch.
4. Within the poll interval (~60s) each builder rebuilds itself.
5. Confirm convergence by reading the commit status for the pushed SHA via the
   GitHub MCP (`mcp__github__get_commit`) — the sandbox can't SSH in to check.

## Scaling the fleet

Add or remove hostnames in `builderHosts` in `flake.nix`, push, and provision
matching servers (their `networking.hostName` must equal a configuration name).
