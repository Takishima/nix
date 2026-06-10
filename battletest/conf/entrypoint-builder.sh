#!/bin/sh
# Builder pod entrypoint: set up sshd with the shared host key and the
# client's authorized key, then run sshd in the foreground.
set -eu

mkdir -p /var/empty /run /etc/ssh /root/.ssh /usr/bin

install -m 600 /ssh/host_key /etc/ssh/ssh_host_ed25519_key
install -m 644 /ssh/host_key.pub /etc/ssh/ssh_host_ed25519_key.pub
install -m 600 /ssh/id_ed25519.pub /root/.ssh/authorized_keys

# Make nix tools resolvable for non-interactive ssh sessions even if sshd's
# compiled-in default PATH wins over SetEnv.
for b in /nix/var/nix/profiles/default/bin/*; do
    ln -sf "$b" "/usr/bin/$(basename "$b")" || true
done

echo "battletest builder $(uname -n): starting sshd ($(nix --version 2>/dev/null || echo 'nix --version failed'))"
exec "$(command -v sshd)" -D -e -f /conf/sshd_config
