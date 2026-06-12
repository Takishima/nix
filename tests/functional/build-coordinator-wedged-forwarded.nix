{ busybox, name }:

with import ./config.nix;

# One fresh, never-cached derivation per recovery round (`name` differs),
# buildable inside the shared daemon's chroot store (busybox builder).
derivation {
  inherit name system;
  builder = busybox;
  args = [
    "sh"
    "-e"
    "-c"
    "echo ${name}-done; echo ${name} > $out"
  ];
}
