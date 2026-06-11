# Project notes for Claude Code

This is the upstream Nix package-manager source. Build/test instructions live in
`HACKING.md` (→ `doc/manual/source/development/building.md`).

## Hetzner remote-builder fleet

The canonical fleet config lives in the separate repo
[`Takishima/nix-builder-infra`](https://github.com/Takishima/nix-builder-infra)
(`infra/hetzner/` here is a reference copy only). See that repo's README for
the full runbook. Builders run `comin` polling that repo's `main` branch.

**Hard constraint for web sessions:** the Claude Code web sandbox has **no
outbound SSH** (only HTTPS through Anthropic's proxy). Do **not** try to deploy
with `nixos-anywhere`, `nixos-rebuild --target-host`, `colmena`, or `deploy-rs`
from a web session — they all need port 22.

Deploy instead via the **GitOps pull** loop:

1. Edit `infra/hetzner/builder.nix` (or hostnames in `infra/hetzner/flake.nix`).
2. Validate: `nix flake check` / `nix eval .#nixosConfigurations.<host>...`.
3. `git push` to the deploy branch — `comin` on each builder pulls and rebuilds.
4. Verify convergence by reading the commit status for the pushed SHA via the
   GitHub MCP (`mcp__github__get_commit`); the sandbox can't SSH in to check.

Provisioning (`infra/hetzner/provision.sh`) uses only the Hetzner HTTPS API.
