#!/bin/sh
# Builder pod entrypoint: set up sshd with the shared host key and the
# client's authorized key, then run sshd in the foreground.
set -eu

mkdir -p /var/empty /run /etc/ssh /root/.ssh /usr/bin

# docker.nix writes "root:!:..." into /etc/shadow; sshd treats '!' as a
# locked account and refuses pubkey login. '*' = no password, not locked.
# (no sed in the image, hence the grep dance)
{ echo 'root:*:1::::::'; grep -v '^root:' /etc/shadow; } > /tmp/shadow.new
cat /tmp/shadow.new > /etc/shadow
rm -f /tmp/shadow.new

# sshd wants a privilege-separation user; the docker.nix image has none.
grep -q '^sshd:' /etc/passwd || echo 'sshd:x:998:998:sshd privsep:/var/empty:/bin/false' >> /etc/passwd
grep -q '^sshd:' /etc/group || echo 'sshd:x:998:' >> /etc/group

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
