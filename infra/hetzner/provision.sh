#!/usr/bin/env bash
#
# provision.sh — create Hetzner Cloud builder servers from a Claude Code web
# session, using ONLY the Hetzner HTTPS API (api.hetzner.cloud). The web sandbox
# has no SSH egress, so this script never opens port 22; ongoing config is then
# delivered by comin (GitOps pull), not by this script.
#
# Two modes:
#   * snapshot (recommended, reproducible): set HETZNER_IMAGE to a NixOS snapshot
#     ID. New servers boot straight into the comin-managed builder config.
#   * infect (first-time golden image): leave HETZNER_IMAGE unset. Servers are
#     created from ubuntu-24.04 and converted to NixOS via nixos-infect through
#     cloud-init. Finish the conversion into the flake/comin config once, then
#     snapshot the result and switch to snapshot mode for the rest of the fleet.
#
# Required env:
#   HCLOUD_TOKEN        Hetzner Cloud API token (read/write)
#   HETZNER_SSH_KEY     Name of an SSH key already uploaded to the project
#                       (used only for provisioning/recovery access)
# Optional env:
#   COUNT=2             Number of builders to create
#   SERVER_TYPE=cpx51   Hetzner server type (pick CPU/RAM-heavy for real load)
#   LOCATION=nbg1       Hetzner location
#   NAME_PREFIX=nix-builder
#   HETZNER_IMAGE       NixOS snapshot ID (enables snapshot mode)
#   NO_INFECT=1         Create plain Ubuntu servers with no user_data; pair with
#                       the bootstrap-builder GitHub Actions workflow, which
#                       installs the flake via nixos-anywhere (recommended path)
#
set -euo pipefail

: "${HCLOUD_TOKEN:?set HCLOUD_TOKEN}"
: "${HETZNER_SSH_KEY:?set HETZNER_SSH_KEY (an SSH key name in the Hetzner project)}"
COUNT="${COUNT:-2}"
SERVER_TYPE="${SERVER_TYPE:-cpx51}"
LOCATION="${LOCATION:-nbg1}"
NAME_PREFIX="${NAME_PREFIX:-nix-builder}"
API="https://api.hetzner.cloud/v1"

api() {
  local method="$1" path="$2"
  shift 2
  curl --fail --silent --show-error \
    -H "Authorization: Bearer ${HCLOUD_TOKEN}" \
    -H "Content-Type: application/json" \
    -X "$method" "${API}${path}" "$@"
}

# cloud-init user-data for the infect path: convert the base image to NixOS.
# nixos-infect reboots into a channel-based NixOS; finishing the switch into the
# flake (which pulls in comin) is a one-time step documented in README.md.
infect_user_data() {
  cat <<'YAML'
#cloud-config
runcmd:
  - curl -L https://raw.githubusercontent.com/elitak/nixos-infect/master/nixos-infect > /root/nixos-infect
  - NIX_CHANNEL=nixos-25.11 PROVIDER=hetznercloud bash /root/nixos-infect 2>&1 | tee /root/infect.log
YAML
}

# Resolve the numeric ID of the named SSH key.
ssh_key_id="$(api GET "/ssh_keys?name=${HETZNER_SSH_KEY}" \
  | python3 -c 'import sys,json; ks=json.load(sys.stdin)["ssh_keys"]; print(ks[0]["id"] if ks else "")')"
[ -n "$ssh_key_id" ] || { echo "SSH key '${HETZNER_SSH_KEY}' not found in project" >&2; exit 1; }

for i in $(seq 1 "$COUNT"); do
  # Zero-pad to match the flake's hostnames (nix-builder-01, ...).
  name="$(printf '%s-%02d' "$NAME_PREFIX" "$i")"
  echo ">> creating ${name} (${SERVER_TYPE} @ ${LOCATION})" >&2

  if [ -n "${HETZNER_IMAGE:-}" ]; then
    image="${HETZNER_IMAGE}"
    user_data=""
  elif [ -n "${NO_INFECT:-}" ]; then
    image="ubuntu-24.04"
    user_data=""
  else
    image="ubuntu-24.04"
    user_data="$(infect_user_data)"
  fi

  body="$(python3 - "$name" "$SERVER_TYPE" "$LOCATION" "$image" "$ssh_key_id" "$user_data" <<'PY'
import json, sys
name, stype, loc, image, key, user_data = sys.argv[1:7]
req = {"name": name, "server_type": stype, "location": loc, "image": image,
       "ssh_keys": [int(key)], "start_after_create": True}
if user_data:
    req["user_data"] = user_data
print(json.dumps(req))
PY
)"

  api POST "/servers" -d "$body" \
    | python3 -c 'import sys,json; s=json.load(sys.stdin)["server"]; print("   ", s["name"], "ip=" + s["public_net"]["ipv4"]["ip"], "id=" + str(s["id"]))'
done

echo ">> done. List the fleet with: api GET /servers (or hcloud server list)" >&2
echo ">> For each builder, capture its SSH host key for the load-test client:" >&2
echo "     ssh-keyscan -t ed25519 <ip> | awk '{print \$3}'   # base64 host key, machines-file field 8" >&2
