# RFC: A production-ready, introspectable remote build protocol

| | |
|------------------|------------------------------------------------|
| **Status**       | Draft / RFC                                    |
| **Target**       | `libstore` remote building, serve & worker protocols |
| **Audience**     | Nix maintainers, Hydra maintainers, operators of remote builders |
| **Supersedes**   | The build-time use of the *serve protocol* (`ssh://`) |

> This is a design document. It proposes *what* to build and *why*, with
> enough wire-level and API-level detail that it can be implemented in
> phases without further large design decisions. It deliberately does not
> change any C++ yet.

## 1. Motivation

Offloading builds to remote machines is a core Nix feature, but the
machinery behind it has accreted over many years and shows it. Three
recurring operational complaints motivate this redesign:

1. **You cannot see what a remote build is doing.** When a derivation is
   built on a remote machine through the distributed-build hook
   (`ssh://`), the actual builder output (compiler/`make`/test output)
   never reaches the client. You get the *hook's* own progress activities
   ("copying dependencies to …", "copying outputs from …") but not the
   build log itself.

2. **Diagnosing a remote failure is painful.** When a remote build fails,
   the client receives only a `BuildResult` with a short message such as
   *"builder for '…' failed with exit code 1"*. The actual log was
   discarded on the builder, so there is nothing to print and nothing to
   fetch. `nix log` does not help because the log was never persisted in a
   place the client can reach.

3. **Using a remote eval store together with a remote build store is
   awkward.** The split between `--eval-store` and the build store works
   for local stores, but the `ssh://` build store explicitly rejects
   `--eval-store`, and copying derivations/closures between the two is
   manual and easy to get wrong.

A fourth, more subtle issue motivates part of the design:

4. **A remote builder has no first-class notion of "this derivation is
   already being built".** If two clients ask the same builder to realise
   the same derivation at the same time, there is no protocol-level way to
   *attach* to the in-flight build and follow its log. Whatever
   deduplication happens today is an implementation accident of the daemon
   running behind the builder, and crucially its log/progress is **not**
   fanned out to the second client.

This RFC proposes a ground-up redesign that makes remote building
**streamable**, **introspectable**, and **dedup-aware**, while preserving
Nix's trust model and providing a clean migration path.

## 2. How it works today (and where it hurts)

This section is the result of reading the current implementation; file and
line references are given so reviewers can follow along. There are **two
distinct protocols** involved, and only one of them can stream logs.

### 2.1 The serve protocol (`ssh://`) — used for distributed builds & Hydra

* Definition: `src/libstore/include/nix/store/serve-protocol.hh`
  (current wire version **2.8**, `SERVE_PROTOCOL_VERSION = (2 << 8 | 8)`).
* Commands: `QueryValidPaths`, `QueryPathInfos`, `DumpStorePath`,
  `ImportPaths`, `BuildPaths`, `QueryClosure`, `BuildDerivation`,
  `AddToStoreNar` (`serve-protocol.hh:140-155`).
* Client side: `LegacySSHStore` (`src/libstore/legacy-ssh-store.cc`).
* Server side: `nix-store --serve` (`opServe`,
  `src/nix/nix-store/nix-store.cc:878`).

The build path is request/response with **no log channel at all**:

```
client → BuildDerivation, drvPath, drv, BuildOptions     (serve-protocol-connection.cc:68)
server   builds synchronously                            (nix-store.cc:1034)
server → BuildResult                                     (nix-store.cc:1036)
```

Two lines in the server's `getBuildSettings` lambda are the crux of the
log problem (`nix-store.cc:908-909`):

```cpp
verbosity = lvlError;                          // structured logs suppressed
settings.getLogFileSettings().keepLog = false; // build log not even persisted
```

So the builder neither streams the log nor keeps it. On failure, the
client throws using only `BuildResult::Failure::message()`
(`build-remote.cc:351`). `BuildResult` itself
(`src/libstore/include/nix/store/build-result.hh`) carries status,
message, `builtOutputs`, and timing — but **no log handle**.

The distributed-build hook (`build-remote.cc`) is a `LegacySSHStore`
client. Its own JSON logger on fd 4 reports activities like "copying
dependencies", but the remote *build* output is never put on that fd
because `buildDerivation()` only returns a `BuildResult`.

### 2.2 The worker protocol (`ssh-ng://`, daemon) — already streams logs

* Definition: `src/libstore/include/nix/store/worker-protocol.hh`
  (version `1.39`).
* It has a full structured log sidechannel interleaved with operation
  results: `STDERR_NEXT`, `STDERR_START_ACTIVITY`, `STDERR_STOP_ACTIVITY`,
  `STDERR_RESULT`, `STDERR_LAST`, `STDERR_ERROR`
  (`worker-protocol.hh:22-31`), driven by `processStderr()`
  (`src/libstore/remote-store.cc`,
  `src/libstore/worker-protocol-connection.cc:34-145`).

So `ssh-ng://` *does* stream logs live, including `Activity` start/stop
and `resBuildLogLine`/`resSetPhase` results. **The capability we want
already exists in one protocol but not the other**, and the protocol the
distributed-build hook uses is the one without it.

### 2.3 Deduplication today

Within a single process, the `Worker` deduplicates goals via
`initGoalIfNeeded` over weak-pointer maps keyed by `drvPath`+output
(`src/libstore/build/worker.cc:58-104`,
`worker.hh:116-127`). This is **per-process only**. When two independent
clients reach the same builder:

* If the builder runs `nix-store --serve` directly, two ssh sessions →
  two processes → potentially two concurrent builds.
* If the builder's `--serve` process forwards to a shared `nix-daemon`,
  the daemon's `Worker` *does* share one goal — but the second client's
  connection sees no progress (server `verbosity=lvlError`), and there is
  no protocol concept of "attach", "replay the log so far", or "here is
  the build you joined".

### 2.4 Eval-store / build-store separation today

* `--eval-store` is parsed in `common-eval-args.cc:140`; `getEvalStore()`
  defaults to the build store (`command.cc:159-164`).
* `Worker` holds both `store` (build) and `evalStore`
  (`worker.hh:200-202`); inputs are copied eval→build in
  `derivation-building-goal.cc:151-162`.
* But `LegacySSHStore::buildPaths` throws
  *"building on an SSH store is incompatible with '--eval-store'"*
  (`legacy-ssh-store.cc:221`). The drv-and-closure copy in the hook is
  hand-rolled (`build-remote.cc:307,341,355,398`).

## 3. Goals and non-goals

### Goals

* **G1 — Live logs.** A client driving a remote build receives the
  builder's structured log (activities, phases, log lines) in real time,
  identical in fidelity to a local build.
* **G2 — Durable diagnostics.** On failure (and success), the full build
  log is persisted on the builder and is fetchable by the client by
  derivation, after the fact, without manual SSH.
* **G3 — Dedup & attach.** Concurrent requests for the same derivation on
  one builder coalesce into a single build, and **every** waiter follows
  the same live log (with replay for late joiners).
* **G4 — Eval/build store ergonomics.** Driving a remote build store with
  a local (or remote) eval store "just works", with explicit, observable
  closure copying.
* **G5 — Introspection.** The state of a builder (in-flight builds,
  queue, who is attached) is queryable for diagnostics and tooling.

### Non-goals

* Not changing the *store path / NAR* formats.
* Not changing Nix's trust model — the careful rules in
  `daemon.cc`'s `BuildDerivation` case (referenced from
  `build-remote.cc:324`) are preserved.
* Not a scheduler/queue-runner replacement (that is Hydra's job); we only
  expose enough state for Hydra to build on.
* Not mandating a flag-day migration; old peers must keep working.

## 4. Design overview

Two strategic options were considered:

* **Option A — Extend the serve protocol** with a log sidechannel,
  log-fetch, and attach semantics (bump serve to 3.x).
* **Option B — Make `ssh-ng://` (worker protocol) the one true remote
  build transport**, add dedup/attach + structured diagnostics there, and
  reduce the serve protocol to the legacy/Hydra compatibility surface.

**This RFC recommends Option B as the destination, reached via Option A as
the bridge.** Rationale:

* The worker protocol already has the hard part (structured streaming),
  is already spoken by `ssh-ng://`, and already carries the daemon's
  shared `Worker` where dedup naturally lives. Building the redesign on
  top of it avoids inventing a second streaming framing.
* But Hydra and a large installed base speak the serve protocol, so we
  cannot simply delete it. We therefore *also* give the serve protocol the
  minimum it needs (a log reference + fetch) so the existing path degrades
  gracefully and old builders remain useful.

The unifying abstraction introduced by this RFC is the **Build Session**.

### 4.1 The Build Session abstraction

A *Build Session* represents one client's view of realising one or more
derived paths on a builder. It is the object that:

1. Has a stable **session id** (so it can be referenced for introspection
   and reconnection).
2. Streams a **structured log multiplex**: activities, phases, and log
   lines, each tagged with the **derivation** they belong to (so a session
   building N drvs can be untangled, and so a late joiner can be told
   "this line belongs to the build you attached to").
3. Resolves to a set of **`BuildResult`s** — extended (§4.4) to carry a
   durable **log reference** and richer diagnostics.

A Build Session is *not* the same as the unit of build work. On the
builder, the unit of work is a **Build** (one resolved derivation). Many
sessions may be attached to one Build (§4.3). This separation is what
makes dedup observable.

### 4.2 Structured, framed log streaming (G1)

Reuse the worker protocol's proven framing rather than invent new wire
codes. Concretely:

* For `ssh-ng://`, remote building already streams via `processStderr`;
  the work is to make the distributed-build hook *use* `ssh-ng://` (or an
  equivalent `Store` that streams) instead of `LegacySSHStore`, and to
  **tag** each `Activity`/log line with its originating derivation so a
  multi-drv session is legible. This means threading a "build id" /
  derivation path through `Activity` parent fields for build-related
  activities.

* For the serve protocol bridge, add an optional **log frame** to the
  `BuildDerivation`/`BuildPaths` exchange: after the request and before
  the final `BuildResult`, the server may emit a sequence of
  length-prefixed log frames mirroring `STDERR_NEXT` /
  `STDERR_START_ACTIVITY` / `STDERR_RESULT`, terminated by a sentinel,
  then the `BuildResult`. Gated on a new serve version (§7) so old clients
  that don't understand frames never receive them.

In both cases the client feeds the frames into its existing `Logger`,
so live remote logs render exactly like local ones (progress bars,
phases, `--log-format internal-json`, etc.).

### 4.3 Dedup and attach (G3) — the heart of the redesign

Introduce a server-side **Build Registry** that lives in the builder's
shared `Worker`/daemon, not per-connection. It maps a **build key** to a
live **Build**.

**Build key.** The key must be correct for both input-addressed and
content-addressed derivations. We key on the **resolved derivation** — the
derivation with every `inputDrv` replaced by its concrete output path
(the same resolution `DerivationResolutionGoal` already computes). This
means two different `.drv` paths that resolve to the same build coalesce,
which is exactly right for CA derivations and harmless for input-addressed
ones (where the drv path already determines the resolved drv).

**Attach semantics.** When a `BuildDerivation`/realise request arrives:

1. Resolve the derivation to its build key.
2. Look it up in the Build Registry.
   * **Miss:** create a `Build`, start it (one `DerivationBuildingGoal`),
     register it, and **subscribe** this session to it.
   * **Hit:** do **not** start a second build. **Subscribe** this session
     to the existing `Build` and immediately **replay** its buffered log
     (§4.3.1) before joining the live tail.
3. When the `Build` finishes, deliver the same `BuildResult` (and log
   reference) to every subscribed session.

This generalises the existing in-process `initGoalIfNeeded` dedup
(`worker.cc:58`) from "one process" to "one builder, many connections,
with fan-out".

**Log fan-out.** Today a goal's `Activity`/log output goes to a single
ambient `logger`. The redesign introduces a per-`Build` **log
broadcaster**: a small sink that (a) appends to the in-memory replay
buffer, (b) appends to the persisted log writer, and (c) forwards to the
`Logger` of every currently-subscribed session. Sessions subscribe and
unsubscribe without affecting the build. A session detaching (client
disconnect) must **not** cancel the build if other sessions remain
attached; the build is cancelled only when its subscriber count hits zero
*and* no `keep-going`/root reason to continue exists (mirrors current
`MonitorFdHup` semantics in `nix-store.cc:1009,1032`, but reference-counted
across sessions).

#### 4.3.1 Late joiners and replay

The broadcaster keeps a bounded **replay buffer** of the structured log so
far (full log up to a configurable cap, then switch to "head + tail with a
truncation marker"). On attach, the server replays the buffer to the new
session as ordinary log frames — flagged with a `replayed=true` marker so
the client can render them dimmed / collapse them — then transitions the
session to the live stream. After the build completes, the persisted log
(§4.4) is the source of truth for any further joiners (they get the whole
thing via the log-fetch path, §4.5).

#### 4.3.2 Introspection of the registry (G5)

Add a read-only query, `QueryActiveBuilds`, returning for each in-flight
`Build`: the build key (resolved drv path), wall-clock start time, current
phase/last activity, number of attached sessions, and bytes of log so
far. This makes "what is my builder doing and who asked for it"
answerable by `nix` tooling and by Hydra, and is the basis for a future
`nix store build-status <builder>` command.

### 4.4 Extended `BuildResult` and durable diagnostics (G2)

Extend `BuildResult` (and its serialisers) with, all optional/back-compat:

* `logRef` — a stable reference by which the full log can be fetched from
  the builder after the fact (in practice: the resolved drv path, which is
  what `LogStore::getBuildLog` already keys on, plus a flag asserting the
  builder persisted it).
* `builderId` — which machine actually ran the build (for fleets/dedup,
  the answer to "where did this come from").
* `deduplicated` — whether this result came from attaching to an existing
  build rather than starting a new one (observability for G3).
* Structured failure detail on the `Failure` variant beyond the free-text
  message: the failing **phase**, **exit status**, and the **log tail**
  already computed by `fixupBuilderFailureErrorMessage`
  (`derivation-building-goal.cc:1157`) — carried as fields rather than
  baked into a string, so clients can render and machines can parse them.

Crucially, the builder must **persist** the build log
(`LogStore::addBuildLog`, `local-store.cc:1629`) instead of discarding it.
That means **removing the `keepLog = false` / `verbosity = lvlError`**
suppression in `opServe` (`nix-store.cc:908-909`) for protocol versions
that support log streaming, and ensuring the daemon path persists too
(it already can: `daemon.cc:1010`).

### 4.5 Fetching logs after the fact (G2)

Add a first-class **`FetchBuildLog`** command (serve protocol) / store
method so a client can ask a builder for a derivation's persisted log
directly, returning the same bytes `nix log` would show locally. This
closes the loop: even if the client wasn't attached during the build (or
detached early), `nix log <drv>` against the remote build store returns
the real log. `LogStore` already provides `getBuildLog`/`getBuildLogExact`
(`log-store.cc`, `local-fs-store.cc:160`); this just exposes it over the
wire and wires `nix log`'s store resolution to consult the build store.

### 4.6 Eval-store / build-store ergonomics (G4)

* Make a remote build store accept an eval store: replace the hard error
  in `legacy-ssh-store.cc:221` (and the equivalent on the `ssh-ng://`
  path) with the **explicit closure-copy** that the hook already performs
  by hand (`build-remote.cc:307,355`), surfaced as observable activities
  ("copying derivation closure eval→build", "copying outputs build→eval")
  so the data movement is no longer invisible.
* Provide a single high-level entry point — conceptually
  `realiseRemote(evalStore, buildStore, derivedPaths)` — that:
  1. copies the drv closure from eval store to build store,
  2. opens a Build Session and streams logs (§4.2),
  3. copies outputs back to wherever the caller wants them, and
  4. returns extended `BuildResult`s.
  The distributed-build hook becomes a thin caller of this, instead of
  re-implementing the choreography inline.
* Document and test the matrix of {local, ssh, ssh-ng} × {eval-store,
  build-store} so the supported combinations are explicit.

## 5. End-to-end: what a remote build looks like after this RFC

```
$ nix build .#thing --builders 'ssh-ng://builder x86_64-linux'

# eval store = local; build store = builder
copying 1 derivation closure to 'builder'                 [G4, observable]
builder: building '/nix/store/…-thing.drv'                [G1, live]
builder:   unpacking sources
builder:   configuring
builder:   building            (cc -O2 …)                  [real builder output, live]
builder: built '/nix/store/…-thing'  (deduplicated=false)  [G3/G4]
copying 1 path from 'builder'

# second, concurrent invocation on another laptop, same builder, same drv:
$ nix build .#thing --builders 'ssh-ng://builder x86_64-linux'
builder: attaching to in-flight build of '…-thing.drv'     [G3 attach]
builder:   …replaying log…                                 [G3 replay]
builder:   building            (cc -O2 …)                   [same live stream]
builder: built '/nix/store/…-thing'  (deduplicated=true)   [G3]

# later, on failure:
$ nix log .#thing            # works against the remote build store  [G2/§4.5]
```

## 6. Trust model (unchanged, restated)

The redesign preserves the existing rules:

* An untrusted client may request a build only where today's code allows
  it: CA derivations, or where the builder trusts the client. The big
  comment in `daemon.cc`'s `BuildDerivation` handler (referenced from
  `build-remote.cc:324-329`) remains authoritative.
* **Attach does not leak.** A session may only attach to (and thus read
  the log of) a Build whose derivation it is itself authorised to build.
  Dedup is keyed on the resolved drv, and authorisation is checked
  per-session *before* subscribing — a client cannot fish for other
  tenants' logs by guessing drv paths it could not itself build.
* `QueryActiveBuilds` (§4.3.2) is subject to the same check: untrusted
  clients see only builds they could have requested, or an aggregate
  count, never other tenants' derivation names.

## 7. Wire compatibility and versioning

* **Serve protocol** bumps to **3.0**. New capabilities (log frames,
  `FetchBuildLog`, `QueryActiveBuilds`, extended `BuildResult` fields,
  attach/`deduplicated`) are gated on the negotiated version. The
  handshake already negotiates `min(client, server)` versions
  (`serve-protocol-connection.cc:8-33`); a 3.0 client talking to a 2.8
  builder transparently falls back to today's behaviour (no live log, no
  fetch — but still correct builds). A 2.x client talking to a 3.0 builder
  never receives 3.0 frames.
* **Worker protocol** gains the dedup/attach + `QueryActiveBuilds`
  operations and the extended `BuildResult` fields behind its own version
  bump; streaming already exists.
* **`BuildResult` serialisers** add fields conditionally, exactly as the
  current code already does for versions 2.3/2.6/2.7/2.8
  (`serve-protocol.cc:17-110`). No existing field changes meaning.
* Old builders remain fully usable at reduced fidelity; old clients are
  unaffected. There is no flag day.

## 8. Phased implementation plan

Each phase is independently shippable and testable.

* **Phase 0 — Persist remote logs (smallest win, biggest relief).**
  Stop discarding the log on the serve path when the client is new enough:
  make `keepLog`/`verbosity` suppression conditional, persist via
  `addBuildLog`, and add `FetchBuildLog` + wire `nix log` to consult the
  build store. Delivers **G2** with no streaming work.
  *Touches:* `nix-store.cc` (`opServe`/`getBuildSettings`),
  `serve-protocol*.{hh,cc}`, `legacy-ssh-store.cc`, `nix/log` command.

* **Phase 1 — Extended `BuildResult`.** Add `logRef`, `builderId`,
  `deduplicated`, structured failure detail; serialise conditionally; have
  `build-remote.cc` print the log tail + `nix log` hint on failure using
  the structured fields instead of a bare message. Delivers the rest of
  **G2**.
  *Touches:* `build-result.{hh,cc}`, `serve-protocol.cc`,
  `worker-protocol*`, `build-remote.cc`.

* **Phase 2 — Live log streaming over serve (bridge).** Add the optional
  log-frame sequence to `BuildDerivation`/`BuildPaths`; feed frames into
  the client `Logger`. Delivers **G1** for `ssh://`.
  *Touches:* `serve-protocol*.{hh,cc}`, `nix-store.cc`,
  `legacy-ssh-store.cc`.

* **Phase 3 — Build Registry + attach/replay + fan-out.** Implement the
  server-side registry keyed on resolved drv, the per-Build log
  broadcaster, replay buffer, reference-counted cancellation, and
  `deduplicated` reporting. Delivers **G3**.
  *Touches:* `build/worker.{cc,hh}`, `build/derivation-building-goal.cc`,
  new `build/build-registry.{cc,hh}`, daemon/serve handlers.

* **Phase 4 — Eval/build store entry point.** Implement
  `realiseRemote(...)`, lift the `--eval-store` restriction, make the
  closure copies observable, refactor `build-remote.cc` onto it.
  Delivers **G4**.
  *Touches:* `legacy-ssh-store.cc`, `build-remote.cc`,
  `libcmd/installables.cc`, `store-api`.

* **Phase 5 — Introspection.** `QueryActiveBuilds` + a `nix` subcommand to
  render it; authorisation per §6. Delivers **G5**.

* **Phase 6 — Converge on `ssh-ng://`.** Make `ssh-ng://` the default,
  fully-featured distributed-build transport (native streaming + dedup),
  keep the serve protocol as the documented compatibility/Hydra surface.

## 9. Testing strategy

* **Functional tests** (`tests/functional/`): extend the existing
  `build-remote*.sh` / `build-hook*.sh` tests with assertions that
  (a) live builder output appears on the client, (b) `nix log` against the
  remote returns the real log after success and after failure, and
  (c) the structured failure carries phase + exit status + tail.
* **Dedup test:** start a slow build on a builder, fire a second
  concurrent request for the same drv, assert exactly one build runs
  (e.g. via a marker file / build counter) and that the second client
  receives the replayed + live log and `deduplicated=true`.
* **Protocol characterisation tests** (`src/libstore-tests`,
  `src/json-schema-checks`): golden serialisations of the new
  `BuildResult` fields and log frames at each negotiated version, proving
  back-compat with 2.x.
* **Trust tests:** an untrusted client cannot attach to or enumerate a
  build it could not itself request.

## 10. Open questions

1. **Replay buffer policy.** Full log vs. head+tail cap, and the
   truncation-marker UX for very long builds.
2. **Cancellation across tenants.** Exact semantics when the *originating*
   session detaches but late joiners remain — confirmed reference-counted
   here, but interactions with `--keep-going` and timeouts need a test
   matrix.
3. **CA resolution timing.** The build key needs the resolved drv; for
   deep CA graphs the resolution itself can race. Does the registry key on
   the resolved drv only, or also expose a "resolving" pre-state that
   later joiners can attach to?
4. **Hydra.** Hydra is the largest serve-protocol consumer; the extended
   `BuildResult` and `FetchBuildLog` should be designed *with* Hydra so it
   can drop its out-of-band log handling, but that coordination is out of
   scope for this document.
5. **`QueryActiveBuilds` privacy.** Default verbosity for untrusted
   callers (aggregate counts only vs. nothing).

## 11. Summary

The capability we want — streamed, structured, persisted build logs — and
the dedup mechanism we want — share one build, fan out its log — both
already exist in fragments: streaming in the worker protocol, dedup in the
in-process `Worker`, log persistence in `LogStore`. The serve protocol,
which is what distributed builds actually use, has none of it and actively
throws the log away. This RFC unifies these fragments behind a **Build
Session** abstraction and a server-side **Build Registry**, delivers the
operational wins in a phased, backward-compatible order (persist → enrich
results → stream → dedup/attach → store ergonomics → introspection), and
sets `ssh-ng://` up as the single production-grade remote build transport.
