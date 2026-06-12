{
  busybox,
  name,
  marker,
  holdIters,
  bulkLines,
}:

with import ./config.nix;

# One execution that (1) prints a marker and a kernel-uuid build token,
# (2) stays in-flight long enough for staggered attachers to join, then
# (3) floods deterministic, mixed-length `NLOG:` lines faster than any
# client drains them, ending with an `NLOGEND:` count. The test asserts
# byte fidelity by digest, so the output must depend only on the loop
# counters — never on time, pid, or environment.
derivation {
  inherit name system;
  builder = busybox;
  args = [
    "sh"
    "-e"
    "-c"
    ''
      echo "${marker}"
      read -r buildtoken < /proc/sys/kernel/random/uuid
      echo "BUILDTOKEN:$buildtoken"
      # Attach window: this sandbox-shell busybox is shell-only (no
      # `sleep`), so burn wall-clock with a builtin loop. Attachers that
      # join later than this still see full fidelity via replay, as long
      # as the total output stays under the replay-buffer caps.
      i=0
      while [ "$i" -lt ${toString holdIters} ]; do i=$((i + 1)); done
      # Bulk: ~16/64/512-byte payloads, cycling. Emitted in one tight
      # builtin loop — far faster than the fan-out drains — so the
      # coordinator's buffering, not the builder's pace, sets the rhythm.
      chunk=0123456789abcdef
      med=$chunk$chunk$chunk$chunk
      long=$med$med$med$med
      long=$long$long
      n=0
      while [ "$n" -lt ${toString bulkLines} ]; do
        case $((n % 3)) in
          0) echo "NLOG:$n:$chunk" ;;
          1) echo "NLOG:$n:$med" ;;
          *) echo "NLOG:$n:$long" ;;
        esac
        n=$((n + 1))
      done
      echo "NLOGEND:$n"
      echo ok > $out
    ''
  ];
}
