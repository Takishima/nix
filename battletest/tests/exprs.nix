# Test derivations for the battletest matrix.
#
# These are built remotely with sandbox = false inside the builder pods, so
# they may use the builder image's /bin/sh and the profile coreutils. `salt`
# is threaded into the derivation environment to defeat caching between runs.
{ salt ? "0" }:
let
  profileBin = "/nix/var/nix/profiles/default/bin";
  mk =
    name: script:
    derivation {
      inherit name salt;
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [
        "-c"
        ''
          export PATH=${profileBin}:/usr/bin:/bin
          ${script}
        ''
      ];
    };
in
{
  # Trivial fast build. Prints a per-execution token (shell PID).
  quick = mk "bt-quick" ''
    echo "bt-quick-token:$$ on $(uname -n)"
    echo "ok-$salt" > $out
  '';

  # Output with a file inside, for `nix store cat` over ssh://.
  quickFile = mk "bt-quickfile" ''
    mkdir -p $out
    echo "payload-$salt" > $out/payload.txt
  '';

  # ~30s build emitting one log line per second. Used for live-streaming,
  # dedup and elastic tests. Prints a per-execution token early.
  slow = mk "bt-slow" ''
    echo "bt-slow-start token:$$ salt:$salt"
    i=0
    while [ $i -lt 30 ]; do
      echo "bt-slow-line-$i"
      sleep 1
      i=$((i+1))
    done
    echo "bt-slow-done"
    echo "done-$salt" > $out
  '';

  # Like `slow` but ~60s, for the cancellation test (needs to outlive several
  # orchestration round-trips).
  slower = mk "bt-slower" ''
    echo "bt-slower-start token:$$ salt:$salt"
    i=0
    while [ $i -lt 60 ]; do
      echo "bt-slower-line-$i"
      sleep 1
      i=$((i+1))
    done
    echo "bt-slower-done"
    echo "done-$salt" > $out
  '';

  # Deterministic failure with recognizable last log lines (fail-loud check).
  fail = mk "bt-fail" ''
    echo "bt-fail-marker-line-1"
    echo "bt-fail-marker-line-2"
    echo "bt-fail-last-words"
    exit 1
  '';

  # Balloons memory by doubling a shell string until the cgroup OOM killer
  # SIGKILLs the build process. Capped at 512 MiB so an environment that does
  # NOT enforce the pod memory limit fails loudly instead of eating the host.
  memhog = mk "bt-memhog" ''
    echo "bt-memhog-start"
    s=$(head -c 1048576 /dev/zero | tr "\0" x)
    while [ ''${#s} -lt 536870912 ]; do s="$s$s"; done
    echo "bt-memhog-survived (memory limit not enforced?)"
    exit 1
  '';
}
