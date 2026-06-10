#!/bin/sh
# Client pod entrypoint: install the ssh key, pre-seed known_hosts with the
# shared builder host key (automated host-key acceptance), then idle.
set -eu

mkdir -p /root/.ssh
install -m 600 /ssh/id_ed25519 /root/.ssh/id_ed25519
install -m 644 /ssh/id_ed25519.pub /root/.ssh/id_ed25519.pub

hostpub="$(cat /ssh/host_key.pub)"
: > /root/.ssh/known_hosts
for h in builder-0.builders builder-1.builders builder-oom-0.builders builder-stock-0.builders; do
    echo "$h $hostpub" >> /root/.ssh/known_hosts
done

cat > /root/.ssh/config <<'EOF'
Host *
  User root
  IdentityFile /root/.ssh/id_ed25519
  StrictHostKeyChecking accept-new
  ConnectTimeout 10
  ServerAliveInterval 15
  LogLevel ERROR
EOF
chmod 600 /root/.ssh/config

echo "battletest client $(uname -n) ready ($(nix --version 2>/dev/null || echo 'nix --version failed'))"
exec sleep infinity
