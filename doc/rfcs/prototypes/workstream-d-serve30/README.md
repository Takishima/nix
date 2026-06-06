# Workstream D prototype — serve 3.0 diagnostic core

> **Status:** throw-away validation prototype.
> **Gates:** freeze **F-SERVE30** (bump `SERVE_PROTOCOL_VERSION` → `3.0`).
> **Independent of A/B/C** (the coordinator): this is about the *serve protocol*
> between Hydra and Nix, which is orthogonal to dedup/attach.
> **Parent:** [`../../remote-build-protocol-redesign.validation.md`](../../remote-build-protocol-redesign.validation.md)
> (Workstream D) · [`../../remote-build-protocol-redesign.decisions.md`](../../remote-build-protocol-redesign.decisions.md)
> (Blocker 3) · [`../../remote-build-protocol-redesign.hydra-coordination.md`](../../remote-build-protocol-redesign.hydra-coordination.md)
> (D4 opening post)

This models **D1** (the diagnostic core behind an unstable version) and **D2**
(the golden / characterisation tests) of Workstream D. It is a faithful,
standalone model of the existing version-gated `BuildResult` serializer ladder in
`src/libstore/serve-protocol.cc` (the `>= {2,3}/{2,6}/{2,8}` pattern), extended
exactly as Blocker 3 §3 specifies — so the back-compat behaviour can be proven in
isolation before any of it touches the real serializer. It is **not** production
code and is meant to be deleted once the layout is validated and Hydra has
signed off.

## What it implements (D1, decisions B3 §3)

The **frozen diagnostic core**, appended **after** the 2.8 `builtOutputs`
block under a `>= {2,9}` guard, in **binary length-prefixed** form (not JSON):

> **Compatibility correction (2026-06).** The core ships as a **minor bump within
> major 2** (`{2,9}`), *not* a `{3,0}` major bump. The serve **client** handshake
> rejects a server with a different major *before* `min()`
> (`serve-protocol-connection.cc:18`), so a `{3,0}` builder would break every
> already-deployed client (old client → upgraded builder). `serve.hh` now models
> that guard (`clientAcceptsServer`) and gates the core at `{2,9}`; `{3,0}` is
> kept only as the constant the regression test proves is rejected. See decisions
> Blocker 3 ("Compatibility correction").

- `logRef` — the resolved drv path the builder asserts it persisted the log under
  (the `LogStore::getBuildLog` key);
- `failurePhase` + `exitCode` + `logTail` — structured failure detail as *fields*
  (fail-loud / G8), not baked into the message string;
- **`QueryBuildLog` as a new `Command = 10`** (the next free value after
  `AddToStoreNar = 9`), never sent unless the negotiated version supports it.

The **deferred set** — `builderId`, `deduplicated` — lives behind an explicitly
*unstable* version (`{2,99}` here — also within major 2 so it stays reachable
past the client guard); its byte layout is **not** a back-compat promise, because
its semantics depend on the still-spiking Phase 3 coordinator.

`SERVE_PROTOCOL_VERSION` is **not** bumped: the core lives behind a provisional
gate until the four freeze criteria are met (see below).

## Build & run

```sh
make check   # build + run the golden/characterisation suite (needs g++/clang++, C++20)
make dump    # print golden hex (to regenerate the constants in tests.cc)
```

## The guarding tests (D2, decisions B3 §4)

`make check` runs, in `tests.cc`:

| Test | Asserts |
|---|---|
| round-trip @ 2.3 / 2.6 / 2.8 / 3.0 | each version serializes and reads back exactly (consumes all bytes) |
| golden bytes @ each version | exact byte layout is stable (characterisation guard — any layout drift fails the test, which is the review signal a freeze needs) |
| additive layout | the 3.0 bytes are the 2.8 bytes **+ appended tail**: no existing field changes meaning |
| **2.8-reads-3.0-bytes** | a 2.8 reader consumes exactly the 2.8 fields and stops at the boundary; the leftover bytes are exactly the 3.0 diagnostic-core tail |
| negotiated-down emits no tail | with the `min()` handshake, a 3.0 Nix talking to a 2.8 peer serializes at 2.8 — no 3.0 tail, `QueryBuildLog` not offered |
| back-compat matrix (both directions) | old Hydra↔new Nix → 2.8; new Hydra↔old Nix → 2.8; new↔new → 3.0 with the core active |
| `QueryBuildLog` round-trip | a 3.0 client fetches the **real persisted log** (`nix log` over serve works — Gap A / §4.5); an unknown drv yields empty, not an error |
| deferred set stays unstable | `builderId`/`deduplicated` append **after** the frozen 3.0 core, leaving the frozen prefix undisturbed |

## Back-compat matrix (decisions B3 §3)

| Client | Server | Negotiated | Behaviour |
|---|---|---|---|
| old Hydra (≤2.8) | new Nix builder ({2,9}) | 2.8 | today's exchange; no new fields/op |
| new Hydra ({2,9}) | old Nix builder (≤2.8) | 2.8 | Hydra falls back to out-of-band log capture; `QueryBuildLog` not sent |
| new | new | {2,9} | full diagnostic core active |
| any client | builder advertising `{3,0}` | — | **client rejects at handshake (`major != 2`)** — why the core is `{2,9}`, not `{3,0}` |

## What this does NOT do (out of scope / external)

The four **freeze criteria** for serve 3.0 are gates this code cannot satisfy by
itself — they are the point of the workstream's external dependency:

- **D3.1** — a *named* Hydra queue-runner maintainer signs off on the exact field
  set and byte order. (The single biggest external dependency; the opening ask is
  drafted in [`hydra-coordination.md`](../../remote-build-protocol-redesign.hydra-coordination.md), **D4**.)
- **D3.2** — a `hydra-queue-runner` branch consumes `QueryBuildLog` + the
  structured fields and drops its out-of-band log capture, validated against a
  new-Nix builder.
- **D3.3** — these golden tests, ported into `src/libstore-tests`, prove the
  back-compat matrix both ways. (This prototype is exactly that proof, standalone.)
- **D3.4** — the field set soaks on the **unstable** version for ≥1 release cycle
  with no layout change.

Only when all four hold does `SERVE_PROTOCOL_VERSION` bump to `(3 << 8 | 0)` and
the layout become a back-compat promise.

## Faithful vs. modelled

**Faithful:** the version-gated ladder structure mirrors `serve-protocol.cc`
exactly (status, errorMsg, the `>= {2,3}` time fields, the `>= {2,8}` binary
`builtOutputs` map with the `>= {2,6}` JSON-hack fallback, then the `>= {2,9}`
core); the wire primitives mirror Nix's `serialise.hh` (8-byte LE integers,
length-prefixed strings padded to 8), so the golden bytes are directly
comparable in shape to the real output; `Command = 10`; the `min()` handshake;
**and the client handshake's pre-`min()` major-version guard
(`clientAcceptsServer`, faithful to `serve-protocol-connection.cc:18`) — the
check that makes a `{3,0}` major bump a back-compat break and forces the `{2,9}`
choice.** (An earlier revision modelled only `min()`, which masked this.)

**Modelled / simplified:** `BuildResult` carries a representative subset of
fields (not the full realisation/`UnkeyedRealisation` graph — modelled as a
`map<outputName, outPath>`); the 2.6 JSON hack is a hand-built string rather than
a real `nlohmann::json`; `QueryBuildLog` is a single request/response over an
in-memory `LogStore` (the stand-in for `LogStore::getBuildLog`), not the full
serve framing. The point under test is the **layout, gating, and back-compat
matrix**, which are reproduced exactly.
