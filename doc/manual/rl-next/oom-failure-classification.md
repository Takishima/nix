---
synopsis: "Out-of-memory kills are classified as transient build failures"
---

When a builder dies from `SIGKILL` — most commonly the kernel out-of-memory
killer — the build result now carries a structured failure classification
(`failureClass = ResourceExhausted`, `killedForMemory = true`), distinguishing
"the machine ran out of memory" from "the build is genuinely broken".

The rendered build error says so too ("killed, most likely by the kernel
out-of-memory killer", with peak memory use when measured), so the
classification is visible to users and not only on the wire.

This is the signal a retry layer can key on, independent of the exit code:
such a failure is not a property of the derivation, so it may be retried
(for example on a larger machine) and is never reused as a canonical result
for the derivation. A plain compile or test failure keeps the default
build-intrinsic classification.

The classification travels only on the unstable protocol extensions (the
serve protocol's provisional diagnostic surface and the worker protocol's
`build-log-query-1` feature) and in JSON build results; the stable wire
formats are unchanged.
