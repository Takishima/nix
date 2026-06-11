{
  config,
  pkgs,
  lib,
  self,
  ...
}:
let
  # Canonical GitHub repo this fleet pulls from. comin (on each builder) polls
  # this over HTTPS; the web session only has to `git push` to the deploy branch.
  repo = "Takishima/nix";
  deployBranch = "claude/hetzner-nix-builder-setup-07r5ev";

  # The deployed git revision, surfaced so the reporter below can post a commit
  # status the web sandbox can read back via the GitHub MCP (it cannot SSH in).
  rev = if self ? rev then self.rev else (self.dirtyRev or "unknown");
in
{
  # --- Boot / disk --------------------------------------------------------
  # Filesystems AND the GRUB device come from disko.nix (the EF02 partition sets
  # boot.loader.grub.devices). Hetzner Cloud boots legacy BIOS.
  boot.loader.grub.enable = true;

  # --- Networking ---------------------------------------------------------
  # Hetzner Cloud hands out IPv4 over DHCP; that is enough for the load-test
  # client to reach the builder over ssh-ng://. (IPv6 is static per-server and
  # can be added via cloud-init if needed.)
  networking.useDHCP = lib.mkDefault true;
  networking.firewall.allowedTCPPorts = [ 22 ];

  # --- Remote-builder role ------------------------------------------------
  # Mirrors the proven topology in tests/nixos/remote-builds-ssh-ng.nix: a
  # trusted SSH build user, sandboxed builds, and the system-features the Nix
  # scheduler matches against requiredSystemFeatures.
  nix.settings = {
    experimental-features = [
      "nix-command"
      "flakes"
    ];
    trusted-users = [
      "root"
      "nixremote"
    ];
    max-jobs = "auto";
    cores = 0;
    sandbox = true;
    system-features = [
      "big-parallel"
      "kvm"
      "nixos-test"
      "benchmark"
    ];
    # Let builders fetch their own substitutes instead of receiving every input
    # over the protocol from the client.
    builders-use-substitutes = true;
  };

  users.groups.nixremote = { };
  users.users.nixremote = {
    isNormalUser = true;
    group = "nixremote";
    description = "Nix remote-build SSH user";
    # Populate keys/ci-builder.pub with the load-test client's public key.
    openssh.authorizedKeys.keyFiles = [ ./keys/ci-builder.pub ];
  };

  services.openssh = {
    enable = true;
    settings = {
      PasswordAuthentication = false;
      PermitRootLogin = "prohibit-password";
    };
  };

  # --- GitOps deploy (pull model) ----------------------------------------
  # Each builder polls the repo and rebuilds itself to
  # nixosConfigurations.<networking.hostName>. No SSH, no inbound deploy path.
  services.comin = {
    enable = true;
    # Build the flake in this subdirectory, not the repo root (which is the Nix
    # package-manager flake).
    repositorySubdir = "infra/hetzner";
    remotes = [
      {
        name = "origin";
        url = "https://github.com/${repo}.git";
        branches.main.name = deployBranch;
        # If Takishima/nix is private, drop a fine-grained read-only token here
        # (delivered out-of-band, e.g. via cloud-init user-data):
        # auth.access_token_path = "/run/keys/comin-github-token";
      }
    ];
  };

  # Record which commit this build came from, so the reporter (and `nixos-version
  # --configuration-revision`) can identify the deployed revision.
  system.configurationRevision = rev;

  # --- Observability: report convergence back to GitHub -------------------
  # The web sandbox cannot SSH in to check a deploy, so each builder POSTs a
  # commit status for the deployed SHA. Claude then reads it via the GitHub MCP
  # (mcp__github__get_commit). Best-effort: it no-ops unless a status token is
  # present at /run/keys/github-status-token (state:write scope on the repo).
  systemd.services.report-deploy-status = {
    description = "Report active NixOS configurationRevision to the GitHub commit status API";
    after = [
      "network-online.target"
      "comin.service"
    ];
    wants = [ "network-online.target" ];
    path = [
      pkgs.curl
      pkgs.coreutils
    ];
    serviceConfig.Type = "oneshot";
    script = ''
      token_file=/run/keys/github-status-token
      if [ ! -r "$token_file" ]; then echo "no status token; skipping report"; exit 0; fi
      rev="${rev}"
      if [ "$rev" = "unknown" ]; then echo "no revision to report; skipping"; exit 0; fi
      curl --fail --silent --show-error \
        -X POST \
        -H "Authorization: Bearer $(cat "$token_file")" \
        -H "Accept: application/vnd.github+json" \
        "https://api.github.com/repos/${repo}/statuses/$rev" \
        -d '{"state":"success","context":"comin/${config.networking.hostName}","description":"deployed on ${config.networking.hostName}"}'
    '';
  };

  # Fire the reporter whenever the active system generation changes.
  systemd.paths.report-deploy-status = {
    wantedBy = [ "multi-user.target" ];
    pathConfig.PathChanged = "/run/current-system";
  };

  # --- Misc ---------------------------------------------------------------
  time.timeZone = "UTC";
  environment.systemPackages = [ pkgs.git ];
  system.stateVersion = "25.11";
}
