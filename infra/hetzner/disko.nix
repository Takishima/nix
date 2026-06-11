{
  # Deterministic, in-repo disk layout so every builder is reproducible and no
  # host-specific hardware-configuration.nix is needed. disko generates the
  # `fileSystems` entries from this spec, so the NixOS config is self-contained.
  #
  # This is applied at install time by `nixos-anywhere` (see provision.sh). It
  # targets a single virtio disk, which is what Hetzner Cloud presents.
  #
  # BIOS/GRUB boot: Hetzner Cloud servers boot in legacy BIOS mode by default,
  # so we use a small BIOS boot partition plus an ext4 root.
  disko.devices.disk.main = {
    device = "/dev/sda";
    type = "disk";
    content = {
      type = "gpt";
      partitions = {
        boot = {
          size = "1M";
          type = "EF02"; # BIOS boot partition for GRUB on GPT
        };
        root = {
          size = "100%";
          content = {
            type = "filesystem";
            format = "ext4";
            mountpoint = "/";
          };
        };
      };
    };
  };
}
