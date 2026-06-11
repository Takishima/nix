#!/usr/bin/env bash
#
# web-setup.sh — mirror of the setup script to paste into the Claude Code web
# environment dialog (Settings → environment → Setup script). It installs Nix so
# a web session can evaluate the builder flake before pushing. The result is
# cached across sessions.
#
# Requires the environment's network access to allow the Determinate installer
# host and the Nix caches: install.determinate.systems, cache.nixos.org,
# channels.nixos.org (plus api.hetzner.cloud for provision.sh).
#
set -euo pipefail

if ! command -v nix >/dev/null 2>&1; then
  curl --proto '=https' --tlsv1.2 -sSf -L \
    https://install.determinate.systems/nix \
    | sh -s -- install --no-confirm
fi

# Make nix available in non-login shells and enable flakes.
if [ -e /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh ]; then
  . /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh
fi
mkdir -p /etc/nix
grep -q 'experimental-features' /etc/nix/nix.conf 2>/dev/null \
  || echo 'experimental-features = nix-command flakes' >> /etc/nix/nix.conf

nix --version || true
