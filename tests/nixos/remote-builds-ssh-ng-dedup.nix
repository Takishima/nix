# Test the `build-coordinator` experimental feature on the deployed
# topology: a NixOS remote builder whose root `nix-daemon` has the feature
# enabled, driven over `ssh-ng://` by an *unprivileged trusted user*
# (`nixremote`). Each SSH connection runs `nix-daemon --stdio` as that
# user; those processes cannot host a coordinator under `/nix/var/nix`
# (regression: they used to try, and every build died with "opening the
# coordinator election lock ...: Permission denied"). They must instead
# forward the build to the root daemon, whose connection children relay to
# the single root-owned coordinator — so two clients with separate local
# stores building the same derivation still share ONE build.

test@{
  config,
  lib,
  hostPkgs,
  ...
}:

let
  pkgs = config.nodes.client1.nixpkgs.pkgs;

  # A build that (1) prints a marker, (2) prints a token unique to the
  # *actual build execution* (a kernel-generated uuid — the sandbox's PID
  # namespace makes `$$` deterministic, so PIDs cannot distinguish two
  # builds), then (3) stays in-flight long enough for the second client to
  # attach. Identical on both clients (same nixpkgs, same extraUtils path),
  # so it resolves to the same derivation.
  expr =
    config:
    pkgs.writeText "expr.nix" ''
      let utils = builtins.storePath ${config.system.build.extraUtils}; in
      derivation {
        name = "dedup";
        system = "i686-linux";
        PATH = "''${utils}/bin";
        builder = "''${utils}/bin/sh";
        args = [ "-c" "${
          lib.concatStringsSep "; " [
            "read -r tok < /proc/sys/kernel/random/uuid"
            "echo dedup-test-marker"
            "echo BUILDTOKEN:$tok"
            "sleep 120"
            "mkdir $out"
            "echo $tok > $out/token"
          ]
        }" ];
        outputs = [ "out" ];
      }
    '';

  # A fast, build-once derivation per lifecycle round (distinct `name`s
  # keep them uncached), for exercising coordinator death and wedging.
  quickExpr =
    config: name:
    pkgs.writeText "quick-${name}.nix" ''
      let utils = builtins.storePath ${config.system.build.extraUtils}; in
      derivation {
        name = "${name}";
        system = "i686-linux";
        PATH = "''${utils}/bin";
        builder = "''${utils}/bin/sh";
        args = [ "-c" "echo ${name}-done; mkdir $out; echo ${name} > $out/name" ];
        outputs = [ "out" ];
      }
    '';

  client =
    { config, pkgs, ... }:
    {
      nix.settings.max-jobs = 0; # force remote building
      nix.distributedBuilds = true;
      nix.buildMachines = [
        {
          hostName = "builder";
          sshUser = "nixremote";
          sshKey = "/root/.ssh/id_ed25519";
          system = "i686-linux";
          maxJobs = 1;
          protocol = "ssh-ng";
        }
      ];
      virtualisation.writableStore = true;
      virtualisation.additionalPaths = [ config.system.build.extraUtils ];
      nix.settings.substituters = lib.mkForce [ ];
      programs.ssh.extraConfig = "ConnectTimeout 30";
    };
in

{
  name = "remote-builds-ssh-ng-dedup";

  nodes = {
    builder =
      { config, pkgs, ... }:
      {
        services.openssh.enable = true;
        virtualisation.writableStore = true;
        nix.settings.sandbox = true;
        nix.settings.substituters = lib.mkForce [ ];
        # The deployed shape: the feature on the daemon, a non-root SSH
        # user in `trusted-users`. No extra directories or groups — the
        # coordinator socket is root-owned under /nix/var/nix, created by
        # the root daemon's connection children.
        nix.settings.experimental-features = [ "build-coordinator" ];
        nix.settings.trusted-users = [ "nixremote" ];
        users.users.nixremote = {
          isNormalUser = true;
        };
      };

    # Two clients, so the two builds come from separate local stores over
    # separate SSH connections (= separate `nix-daemon --stdio` processes).
    client1 = client;
    client2 = client;
  };

  testScript =
    { nodes }:
    ''
      # fmt: off
      import subprocess

      start_all()

      # Create an SSH key and install it on both clients.
      subprocess.run([
        "${hostPkgs.openssh}/bin/ssh-keygen", "-t", "ed25519", "-f", "key", "-N", ""
      ], capture_output=True, check=True)
      for client in [client1, client2]:
        client.succeed("mkdir -p -m 700 /root/.ssh")
        client.copy_from_host("key", "/root/.ssh/id_ed25519")
        client.succeed("chmod 600 /root/.ssh/id_ed25519")

      # Authorize it for the unprivileged builder user.
      builder.succeed("mkdir -p -m 700 /home/nixremote/.ssh")
      builder.copy_from_host("key.pub", "/home/nixremote/.ssh/authorized_keys")
      builder.succeed("chown -R nixremote /home/nixremote/.ssh")
      builder.wait_for_unit("sshd")
      builder.wait_for_unit("multi-user.target")
      builder.wait_for_unit("network-addresses-eth1.service")

      for client in [client1, client2]:
        client.wait_for_unit("network-addresses-eth1.service")
        client.succeed(f"ssh -o StrictHostKeyChecking=no nixremote@{builder.name} 'echo hello world'")

      # Client 1 starts the build in the background; its log streams back
      # over ssh-ng.
      client1.succeed(
        "(nix-build ${expr nodes.client1} --no-out-link 2> /tmp/build.log; echo $? > /tmp/build.rc) >/dev/null 2>&1 &"
      )

      # Wait until the (single) build is in-flight on the builder.
      client1.wait_until_succeeds("grep -q dedup-test-marker /tmp/build.log", timeout=120)

      # The coordinator is alive on the builder, hosted by the *root*
      # daemon: root-owned socket in /nix/var/nix, nothing world-writable.
      builder.succeed("test -S /nix/var/nix/coordinator.socket")
      builder.succeed("[ \"$(stat -c %U /nix/var/nix/coordinator.socket)\" = root ]")

      # Client 2 — separate store, separate SSH connection, separate
      # `nix-daemon --stdio` — requests the same derivation and must attach
      # to the in-flight build (the marker reaches it via log replay).
      client2.succeed(
        "(nix-build ${expr nodes.client2} --no-out-link 2> /tmp/build.log; echo $? > /tmp/build.rc) >/dev/null 2>&1 &"
      )
      client2.wait_until_succeeds("grep -q dedup-test-marker /tmp/build.log", timeout=110)

      # Both builds finish successfully (the shared build runs ~120s).
      for client in [client1, client2]:
        client.wait_until_succeeds("test -f /tmp/build.rc", timeout=420)
        rc = client.succeed("cat /tmp/build.rc").strip()
        log = client.succeed("sed -e 's/^/build-log:/' /tmp/build.log")
        print(log)
        assert rc == "0", f"build on {client.name} failed (rc={rc})"

      # Exactly one build ran: both clients observed the same
      # per-execution token. Different tokens would mean two builds (no
      # dedup); a missing token means the second request never attached.
      tok1 = client1.succeed("grep -o 'BUILDTOKEN:[0-9a-f-]*' /tmp/build.log | head -n1").strip()
      tok2 = client2.succeed("grep -o 'BUILDTOKEN:[0-9a-f-]*' /tmp/build.log | head -n1").strip()
      print(f"token client1={tok1} client2={tok2}")
      assert tok1 != "BUILDTOKEN:" and tok1, "no build token on client1"
      assert tok1 == tok2, "tokens differ => two builds ran (no dedup across stdio daemons)"

      # And nothing degraded to an uncoordinated build behind our back.
      for client in [client1, client2]:
        client.fail("grep -q 'without build dedup' /tmp/build.log")
        client.fail("grep -q 'coordinator election lock' /tmp/build.log")

      # ------------------------------------------------------------------
      # Coordinator lifecycle (the "wedged builder" regression): a
      # coordinator killed without cleanup (the NixOS-activation cgroup
      # teardown shape) or wedged (alive, listening, never accepting) must
      # never wedge — or fail — later unprivileged builds.

      # A fresh quick build spawns a fresh coordinator on the builder; while
      # it idles, the election lock file names the live coordinator process.
      client1.succeed("timeout 120 nix-build ${quickExpr nodes.client1 "lifecycle-a"} --no-out-link >&2")
      coord_pid = builder.succeed("cat /nix/var/nix/coordinator.socket.lock").strip()
      builder.succeed(f"kill -0 {coord_pid}")

      # SIGKILL it: no cleanup code runs, socket and lock files both stay.
      builder.succeed(f"kill -9 {coord_pid}")
      builder.succeed("test -e /nix/var/nix/coordinator.socket")
      builder.succeed("test -e /nix/var/nix/coordinator.socket.lock")

      # The next unprivileged build re-elects over the stale files promptly
      # and stays fully coordinated (this used to wedge the machine until
      # the lock file was removed by hand).
      client2.succeed("timeout 120 nix-build ${quickExpr nodes.client2 "lifecycle-b"} --no-out-link 2> /tmp/lifecycle.log >&2")
      client2.fail("grep -q 'without build dedup' /tmp/lifecycle.log")

      # Now a *wedged* coordinator: stopped, so it keeps its socket and the
      # flock but never accepts. The relay must bound its attach handshake
      # (default 30s), degrade with a warning, and complete the build — the
      # pre-fix behaviour was an indefinite hang of the daemon session,
      # ending only in client disconnect.
      coord_pid = builder.succeed("cat /nix/var/nix/coordinator.socket.lock").strip()
      builder.succeed(f"kill -STOP {coord_pid}")
      client1.succeed("timeout 240 nix-build ${quickExpr nodes.client1 "lifecycle-c"} --no-out-link 2> /tmp/lifecycle.log >&2")
      client1.succeed("grep -q 'without build dedup' /tmp/lifecycle.log")
      builder.succeed(f"kill -9 {coord_pid}")

      # Coordination recovers as soon as the wedge is gone...
      client2.succeed("timeout 120 nix-build ${quickExpr nodes.client2 "lifecycle-d"} --no-out-link 2> /tmp/lifecycle.log >&2")
      client2.fail("grep -q 'without build dedup' /tmp/lifecycle.log")

      # ...and a cleanly idle-exiting coordinator leaves nothing behind (a
      # stale root/0600 lock used to outlive every process and daemon
      # restart on the deployed builder).
      builder.wait_until_succeeds(
        "test ! -e /nix/var/nix/coordinator.socket -a ! -e /nix/var/nix/coordinator.socket.lock",
        timeout=120
      )
    '';
}
