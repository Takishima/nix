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
   built on a remote machine through the distributed-build hook with an
   `ssh://` (serve protocol) builder, the actual builder output
   (compiler/`make`/test output) never reaches the client. You get the
   *hook's* own progress activities ("copying dependencies to …", "copying
   outputs from …") but not the build log itself. **This is hop-specific,
   not universal** — see §2.6 for the validated matrix. `ssh-ng://`
   builders and `ssh-ng://` build *stores* do stream live logs today; the
   serve (`ssh://`) hop is the one that drops them.

2. **You can watch a remote build but cannot retrieve its log
   afterwards.** With `--store ssh-ng://builder`, logs stream live via the
   daemon's tunnel logger — but `nix log` against that store throws
   `unsupported("getBuildLogExact")` (`src/libstore/ssh-store.cc:64-67`,
   carrying a standing `FIXME: extend daemon protocol`). So if a build
   failed while you weren't looking, or you want the log after the fact,
   there is no client-reachable copy. This — not a copying bug — is the
   dominant pain in the "remote eval store + remote build store" setup
   (§2.6, Gap A).

3. **The eval-store / build-store split mostly works — except for logs.**
   Driving a remote build store alongside a (local or remote) eval store
   is functionally fine: derivations and closures are copied and builds
   succeed just as in the non-split case. The remaining gap in this
   configuration is the *same* one as everywhere else — you still cannot
   get the build logs. (There is one narrow sharp edge: the `ssh://` build
   store rejects `--eval-store` outright at `legacy-ssh-store.cc:222`. But
   where that is avoided — e.g. with `ssh-ng://` — the split is not the
   problem; logs are.) In other words, this is not really a fourth
   problem: it is problems 1–2 again, and the design must simply not treat
   "build store ≠ eval store" as a special case for logging.

Three more issues motivate the rest of the design:

4. **A remote builder has no first-class notion of "this derivation is
   already being built".** If two clients ask the same builder to realise
   the same derivation at the same time, there is no protocol-level way to
   *attach* to the in-flight build and follow its log. Whatever
   deduplication happens today is an implementation accident of the daemon
   running behind the builder, and crucially its log/progress is **not**
   fanned out to the second client.

5. **The protocol does not scale to elastic, multi-tenant backends.**
   Services like [nixbuild.net](https://nixbuild.net) present a *single*
   endpoint backed by an autoscaling pool ("infinite CPUs"), do their own
   scheduling, and reuse build results globally across an account. Nix's
   distributed-build *hook*, by contrast, schedules on the client with
   per-machine, per-slot **file locks** and a fixed `maxJobs`
   (`build-remote.cc:40-43,168-177`), and the serve protocol does one
   synchronous build per connection. The model fights an elastic backend
   instead of cooperating with it. (§4.7)

6. **Hydra must keep working.** Hydra's queue runner is the largest
   consumer of the serve protocol, via the deliberately stripped-down
   `BasicClientConnection`/`BasicServerConnection`
   (`serve-protocol.hh:96-101`). Any redesign has to be *additive* for
   Hydra — extending what it can do (structured logs, log fetch, richer
   results) without breaking the surface it depends on. (§4.8)

This RFC proposes a ground-up redesign that makes remote building
**streamable**, **introspectable**, **dedup-aware**, and **friendly to
elastic multi-tenant backends**, while preserving Nix's trust model, Hydra
compatibility, and a clean migration path.

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

The distributed-build hook
(`src/nix/build-remote/build-remote.cc` — note it lives under `src/nix`,
not `src/libstore`; later bare `build-remote.cc` references mean this file)
is a `LegacySSHStore` client. Its own JSON logger on fd 4 reports activities
like "copying dependencies", but the remote *build* output is never put on
that fd because `buildDerivation()` only returns a `BuildResult`.

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

Within a *single process*, the `Worker` deduplicates goals via
`initGoalIfNeeded` over weak-pointer maps keyed by `drvPath`+output
(`src/libstore/build/worker.cc:58-104`, `worker.hh:116-127`). This matters
for one client that asks for the same drv twice in one invocation, but it
**does not extend across connections**, because the daemon is
**fork-per-connection**:

* `daemonLoop` forks a separate child process for every accepted connection
  (`src/nix/unix/daemon.cc`, `startProcess(...)`, "Fork a child to handle
  the connection"); each child calls `storeConfig->openStore()` and runs
  its own `processConnection` with its **own `Worker` in its own address
  space**. There is no `Worker`, goal map, or buffer shared between two
  connections.
* What actually coalesces two concurrent builds of the same derivation is
  **filesystem `PathLocks` on the output paths**, not goal-sharing: a build
  acquires output locks in `acquireResources`
  (`derivation-building-goal.cc:437-497`, `outputLocks.lockPaths(...)`); a
  second process blocks on the same lock and, on waking, finds the outputs
  already valid and skips the rebuild.

The consequence is important for this RFC: cross-connection coalescing today
serializes redundant *work* but provides **no log fan-out** — the second
client sees nothing of the first build, and there is no protocol concept of
"attach", "replay the log so far", or "here is the build you joined". The
"share one build, fan out its log" mechanism this RFC wants therefore does
**not** exist even latently in the stock daemon; building it requires
cross-process coordination (§4.3.3), not merely exposing an existing shared
`Worker`.

### 2.4 Eval-store / build-store separation today

This already works for the common cases; it is included here only because
the design must not regress it and must extend logging to cover it.

* `--eval-store` is parsed in `common-eval-args.cc:140`; `getEvalStore()`
  defaults to the build store (`command.cc:159-164`).
* `Worker` holds both `store` (build) and `evalStore`
  (`worker.hh:200-202`); inputs are copied eval→build in
  `derivation-building-goal.cc:151-162`. The drv-and-closure copy in the
  hook is hand-rolled but correct (`build-remote.cc:307,341,355,398`).
* The one real sharp edge is that `LegacySSHStore::buildPaths` throws
  *"building on an SSH store is incompatible with '--eval-store'"*
  (`legacy-ssh-store.cc:222`). Outside that specific `ssh://`+`--eval-store`
  combination, the split behaves like the non-split case — **the missing
  piece is purely the logs (§2.1), not the copying.**

For a remote (`ssh-ng://`) build store, the `.drv` closure is copied from
the eval store automatically by `RemoteStore::copyDrvsFromEvalStore`
(`remote-store.cc:548-566`), invoked from `buildPaths`/
`buildPathsWithResults` (`:571,584`) when `evalStore.get() != this`. Only
the `.drv` files are copied; build inputs are expected to come from the
remote's substituters or be uploaded. The `evalStore` argument is *not*
sent over the wire — it only drives this client-side copy.

### 2.5 What `--eval-store auto --store ssh-ng://builder` does today

This is nixbuild.net's recommended invocation, so it is worth tracing end
to end:

1. **Evaluate locally** (`--eval-store auto` → local store); `.drv`s land
   in the local eval store.
2. **Copy the `.drv` closure** to the remote build store
   (`copyDrvsFromEvalStore`, `remote-store.cc:548-566`).
3. **Build on the remote daemon** over the worker protocol; its `Worker`
   runs the build.
4. **Live logs stream back** via the daemon's `TunnelLogger`
   (`daemon.cc:50,1054-1059`) → client `processStderr()`
   (`remote-store.cc:577,593,658`). ✅
5. **Outputs are fetched back** as needed.

So live logs work. What does *not* work is `nix log` afterwards
(`ssh-store.cc:64-67`, Gap A below), and — if that remote build store
itself offloads to `ssh://` builders — the nested build's live log (Gap B
below).

### 2.6 Validated log-flow matrix (as of this writing)

| Configuration | Live logs to client | `nix log` after the fact |
|---|---|---|
| Local store | ✅ direct logger | ✅ `LocalFSStore` |
| `unix://` daemon | ✅ `TunnelLogger` | ✅ (daemon persists) |
| `--store ssh-ng://` (direct daemon build) | ✅ `TunnelLogger` | ❌ `getBuildLogExact` `unsupported` (`ssh-store.cc:64-67`) |
| `--eval-store … --store ssh-ng://` | ✅ | ❌ same |
| `mounted-ssh-ng://` | ✅ | ✅ via mounted FS (`ssh-store.cc:181-184`) |
| `--builders 'ssh://…'` (serve hook) | ❌ inner build log not forwarded upstream | ⚠️ persisted only on that daemon |
| `--builders 'ssh-ng://…'` (hook) | ✅ hook forwards JSON (`derivation-building-goal.cc:739-746`) | ⚠️ persisted only on that daemon |

Two precise defects fall out of this matrix:

* **Gap A — `nix log` is unsupported over `ssh-ng`.** You can watch a build
  live but cannot retrieve its log later, because `SSHStore` does not
  implement `getBuildLogExact` (`ssh-store.cc:64-67`, with the standing
  `FIXME: extend daemon protocol, move implementation to RemoteStore`).
  This is the dominant complaint in the remote-eval + remote-build-store
  configuration.
* **Gap B — the serve (`ssh://`) hop drops live logs upstream.** When a
  daemon offloads to an `ssh://` builder, `buildWithHook` persists the
  inner log to a local `LogFile` but does **not** re-emit it to the
  ambient (tunnel) logger, so an upstream worker-protocol client sees
  nothing live for that sub-build. `ssh-ng://` sub-builders do propagate,
  via the JSON forwarding at `derivation-building-goal.cc:739-746`.

### 2.7 Gap C — logs are off by default, and remote failures are quiet

This gap is what users actually hit first, and it is a UX/discoverability
problem rather than a protocol one. Validated against the code:

* **`nix build` suppresses builder output unless `-L`.** The progress-bar
  logger only prints `resBuildLogLine`/`resPostBuildLogLine` when
  `printBuildLogs` is set (`progress-bar.cc:345-362`), which is enabled by
  `-L` / `--print-build-logs` (`main.cc:117`); otherwise log lines are
  shown transiently next to the activity and discarded. So **without `-L`,
  no build output is ever written to stdout/stderr** — for local *or*
  remote builds. A `… | tee build.log` therefore captures the progress UI
  and the final error, but not the build log.
* **A remote (`ssh-ng`) failure carries no log tail.** For a *local* build,
  the failure message embeds the last N log lines via
  `fixupBuilderFailureErrorMessage` (`derivation-building-goal.cc:1157`).
  For a build that ran on a remote daemon, the error string is produced
  remotely and returned over `STDERR_ERROR`; whether it includes a tail
  depends on the remote. In practice users see a bare:

  ```
  error: Cannot build '/nix/store/…-whisper-cpp-1.7.5.drv'.
         Reason: builder failed with exit code 127.
         Output paths: /nix/store/…-whisper-cpp-1.7.5
  ```

  with **no log lines and no "run `nix log …`" hint** — and `nix log`
  wouldn't work anyway over `ssh-ng` (Gap A). The result is a build that
  failed for an unknowable reason (here, exit 127 = a missing command in
  the builder), which is exactly the CI pain point.

The redesign must close Gaps A and B (make the matrix uniformly ✅/✅) **and**
fix Gap C so that a failed build is never silent: see **G8**.

## 3. Goals and non-goals

### Goals

* **G1 — Live logs.** A client driving a remote build receives the
  builder's structured log (activities, phases, log lines) in real time,
  identical in fidelity to a local build.
* **G2 — Durable diagnostics.** On failure (and success), the full build
  log is persisted on the builder and is fetchable by the client by
  derivation, after the fact, without manual SSH. Concretely this means
  closing **Gap A**: `nix log` must work over `ssh-ng://`
  (`getBuildLogExact` implemented over the worker protocol), and **Gap B**:
  a sub-build behind a serve hop must surface its log upstream.
* **G3 — Dedup & attach.** Concurrent requests for the same derivation on
  one builder coalesce into a single build, and **every** waiter follows
  the same live log (with replay for late joiners).
* **G4 — Logs are store-split-agnostic.** Live streaming (G1) and durable
  fetch (G2) work identically whether or not the build store differs from
  the eval store, and whether either or both are remote. The split is
  already functional today; the only requirement is that logging not treat
  it as a special case. (Removing the narrow `ssh://`+`--eval-store`
  rejection at `legacy-ssh-store.cc:222` is a minor cleanup, not a
  headline goal.)
* **G5 — Introspection.** The state of a builder (in-flight builds,
  queue, who is attached) is queryable for diagnostics and tooling.
* **G6 — Friendly to elastic, multi-tenant backends.** The protocol
  cooperates with a single endpoint that fronts an autoscaling pool and
  does its own scheduling and global build reuse (the nixbuild.net model):
  concurrency is backend-negotiated rather than gated by client-side slot
  locks, many builds can be in flight per endpoint, and dedup/reuse is a
  first-class result, not an accident. (§4.7)
* **G7 — Hydra stays first-class.** Everything new is additive over the
  serve protocol's `BasicClientConnection`/`BasicServerConnection`
  (`serve-protocol.hh:96-101`); Hydra can adopt structured logs and log
  fetch incrementally and is never forced to. (§4.8)
* **G8 — Fail loud (CI-friendly).** A failed build must never be silent. On
  failure the failing derivation's log is surfaced to the client without
  extra round-trips and without requiring `-L` up front: the error carries
  a log tail (for remote builds too), and a `print-build-logs = on-failure`
  mode dumps the full failing log automatically. The target is that a CI
  job running `nix build --store ssh-ng://…` shows *why* a build failed by
  default. (§4.9)

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
  is already spoken by `ssh-ng://`, and is where the daemon's `Worker`
  already runs builds. Building the redesign on top of it avoids inventing
  a second streaming framing. (Note: this gives us streaming for free, but
  **not** cross-connection dedup — the daemon is fork-per-connection, §2.3
  — which is why §4.3/§4.3.3 are scoped separately.)
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
  **tag** each `Activity`/log line with its originating build.

  **Tagging is the load-bearing detail, not a footnote.** When one session
  drives N derivations, their activity trees interleave on the wire, and a
  `resBuildLogLine` result only carries an `ActivityId` and the line text —
  not "which drv". Reliable attribution requires that every build-related
  sub-activity be rooted at a per-build top-level activity carrying the
  resolved drv (build id), so the client can map any `ActivityId` back to a
  build by walking `parent` links it already receives via
  `STDERR_START_ACTIVITY` (which includes `parent`,
  `worker-protocol`/`daemon.cc:161`). Concretely: introduce a `Build`
  activity type whose fields include the build id; ensure
  `actBuild`/phase/log activities created under it set `parent` to that
  activity; and have the client maintain an `ActivityId → build id` index
  from the start/stop frames. Without this, a multi-build session is
  unreadable "log soup"; with it, a UI can group, filter, and label per
  build. This index is also what lets the on-failure dump (§4.9) select
  *the failing build's* lines out of an interleaved stream.

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

> **Scope, stated up front.** Unlike the rest of this RFC, this feature is
> **not** a thin exposure of an existing capability — §2.3 establishes that
> the stock daemon is fork-per-connection with no shared `Worker`, so there
> is no in-process registry to "lift". This section describes the *target*
> semantics, and is explicitly **conditional on a cross-process
> coordination mechanism (§4.3.3)**. Two deployment classes get it on very
> different timelines:
>
> * **Single-process multiplexed backends** — a nixbuild.net-style endpoint,
>   or a single `RemoteStore` connection driving many builds — can implement
>   the registry/broadcaster natively in their own address space and get
>   dedup/attach/replay essentially "for free". This is where the feature
>   lands first and most cleanly.
> * **The stock fork-per-connection `nix-daemon`** gets cross-client
>   dedup/fan-out **only** once §4.3.3 is built; until then it keeps today's
>   behaviour (work coalesced by output `PathLocks`, no log fan-out). The
>   operational wins of this RFC (Gaps A/B/C) do **not** depend on this and
>   ship first (§8).

Introduce a server-side **Build Registry** that maps a **build key** to a
live **Build**, owned by whatever component actually schedules builds for
the endpoint (the backend's scheduler, or the coordinator of §4.3.3 — *not*
a per-connection `Worker`).

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

Within a single process this resembles `initGoalIfNeeded`
(`worker.cc:58`) extended with subscribers; **across** processes it is a
genuinely new mechanism (§4.3.3), not a generalisation of existing code.

**Log fan-out.** A goal's `Activity`/log output goes to a single ambient
`logger`. The redesign introduces a per-`Build` **log broadcaster**: a sink
that (a) appends to the replay buffer, (b) appends to the persisted log
writer, and (c) forwards to the `Logger` of every currently-subscribed
session. In a single-process backend this is an ordinary in-memory
fan-out; in the fork-per-connection daemon the buffer and broadcaster must
live in the coordinator (§4.3.3), and "forward to a session's `Logger`"
becomes "write a log frame down that session's socket". Sessions subscribe
and unsubscribe without affecting the build.

**Cancellation is reference-counted — and that is the hard part across
processes.** A session detaching (client disconnect) must **not** cancel
the build if other sessions remain attached; the build is cancelled only
when its subscriber count hits zero *and* no `keep-going`/root reason to
continue exists. Today a client disconnect trips `MonitorFdHup`
(`nix-store.cc:1009,1032`) and the handling child simply exits — there is
no shared refcount, so this semantics is **not** achievable in the stock
daemon without the coordinator (§4.3.3). In a single-process backend it is
a straightforward counter.

#### 4.3.1 Late joiners and replay

The broadcaster keeps a bounded **replay buffer** of the structured log so
far (full log up to a configurable cap, then switch to "head + tail with a
truncation marker"). On attach, the server replays the buffer to the new
session as ordinary log frames — flagged with a `replayed=true` marker so
the client can render them dimmed / collapse them — then transitions the
session to the live stream. After the build completes, the persisted log
(§4.4) is the source of truth for any further joiners (they get the whole
thing via the log-fetch path, §4.5).

The buffer's *location* is determined by §4.3.3: in a single-process
backend it is plain process memory; in the fork-per-connection daemon it
must be owned by the coordinator (a shared ring buffer or the coordinator's
own memory), because the forked child handling the first build cannot
expose its memory to the child handling a late joiner. This is why Q1
(replay policy) and the coordination mechanism cannot be decided
independently.

#### 4.3.2 Introspection of the registry (G5)

Add a read-only query, `QueryActiveBuilds`, returning for each in-flight
`Build`: the build key (resolved drv path), wall-clock start time, current
phase/last activity, number of attached sessions, and bytes of log so
far. This makes "what is my builder doing and who asked for it"
answerable by `nix` tooling and by Hydra, and is the basis for a future
`nix store build-status <builder>` command. Note that in the
fork-per-connection daemon this query *also* requires the coordinator
(§4.3.3): a single connection's child has no view of builds running in
sibling children.

#### 4.3.3 Cross-process build coordination (the prerequisite)

For the stock `nix-daemon`, everything above presupposes a component that
outlives and is shared across the per-connection children. There are three
candidate mechanisms, in rough order of increasing scope:

1. **A coordinator process (recommended starting point).** A long-lived
   per-store coordinator (a new role of `nix-daemon`, or a sidecar) owns the
   Build Registry, the replay buffers, and the subscriber refcounts. The
   forked connection children become thin: on a build request a child asks
   the coordinator to *start-or-attach* a build keyed on the resolved drv,
   then relays the coordinator's log frames down its own socket and
   forwards cancellation/disconnect. The coordinator runs the actual
   `Worker`. This keeps the wire protocol changes (Build Sessions, §4.1)
   the same for both backend classes and confines the new complexity to one
   process. It is also the natural home for `QueryActiveBuilds`.
2. **Shared memory + a published log ring buffer**, keyed on the resolved
   drv, with the building child as writer and attaching children as
   readers, plus a small shared registry/refcount table. Avoids a new
   daemon but pushes lifecycle, cleanup-on-crash, and backpressure into
   shared-memory bookkeeping that is easy to get wrong.
3. **Re-architect the daemon to single-process multiplexed**, abandoning
   fork-per-connection. This makes the in-process design of §4.3 literally
   true but is the largest change and has its own isolation/robustness
   trade-offs (one crashing build can no longer be contained to a child).

This RFC does **not** pick one here; it flags the choice as a **design
spike** that must precede freezing any Phase 3 wire surface (§8, §10),
because the mechanism bounds what session re-attach (§4.7.4) and
`QueryActiveBuilds` can promise. Single-process backends (nixbuild.net,
single-`RemoteStore` drivers) need none of this and can proceed in
parallel.

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

### 4.5 Fetching logs after the fact (G2 / Gap A)

Add a first-class **`QueryBuildLog`** operation to **both** protocols so a
client can ask a builder for a derivation's persisted log directly,
returning the same bytes `nix log` would show locally. This closes the
loop: even if the client wasn't attached during the build (or detached
early), `nix log <drv>` against the remote build store returns the real
log.

* **Worker protocol (`ssh-ng://`) — closes Gap A.** Implement the standing
  `FIXME` at `ssh-store.cc:64-67`: add a worker-protocol op so
  `RemoteStore`/`SSHStore::getBuildLogExact` forwards to the daemon's
  `LogStore` instead of throwing `unsupported`. After this, `nix log` works
  against any `ssh-ng://` store, which is the single most-requested fix in
  the validated matrix (§2.6).
* **Serve protocol.** Add the analogous command for `ssh://` builders and
  Hydra.

`LogStore` already provides `getBuildLog`/`getBuildLogExact` server-side
(`log-store.cc`, `local-fs-store.cc:160`); this work is purely exposing it
over the two wires and wiring `nix log`'s store resolution
(`libcmd/get-build-log.cc:7-29`) to consult the build store.

To also close **Gap B**, `buildWithHook` must re-emit a sub-build's log
lines to the *ambient* logger (not only to the local `LogFile`) so they
tunnel upstream — i.e. feed the parsed `resBuildLogLine`/`resSetPhase`
JSON (`derivation-building-goal.cc:737-762`) into `worker.act`/`logger`,
not just `logFile->sink`.

### 4.6 Logs across the eval/build store split (G4)

The store split already works functionally, so this section is about
**not** making logs a special case, plus one small cleanup.

* The Build Session (§4.1) and log-fetch (§4.5) are keyed on the
  **derivation**, independent of which store happens to be the eval store
  and which is the build store. Streaming and `nix log` must therefore
  behave identically in the split configuration with no extra wiring — the
  log frames and `logRef` carry through unchanged. This is the actual
  requirement behind G4: a user running a remote build store with a
  separate eval store should get the same live + fetchable logs as anyone
  else.
* Minor cleanup (not required for logs): remove the hard error in
  `legacy-ssh-store.cc:222` so `ssh://`+`--eval-store` no longer aborts,
  reusing the **explicit closure-copy** the hook already performs by hand
  (`build-remote.cc:307,355`). While there, surface those copies as
  observable activities ("copying derivation closure eval→build", "copying
  outputs build→eval") so the (already-correct) data movement is at least
  visible.
* Optionally fold the hook's choreography behind a single entry point —
  conceptually `realiseRemote(evalStore, buildStore, derivedPaths)` — that
  copies the drv closure, opens a Build Session and streams logs (§4.2),
  copies outputs back, and returns extended `BuildResult`s. This is a
  refactor for clarity, not a fix for a functional defect.

### 4.7 Scaling to elastic, multi-tenant backends (G6)

The reference point here is [nixbuild.net](https://nixbuild.net). Its
public behaviour and documented design tell us what a production backend
needs from the protocol:

* It is configured as an ordinary builder/store but recommends
  **`ssh-ng://`** explicitly ("use `ssh-ng://` instead of `ssh://`"), with
  the canonical invocation `nix build --eval-store auto --store
  ssh-ng://eu.nixbuild.net`.
* It presents **one endpoint over an autoscaling pool** ("infinite CPUs")
  and does its **own scheduling** — "you can set [max-jobs] to anything
  really, since nixbuild.net will take care of the scheduling and scaling
  on its own", and it "will not let multiple Nix clients step on each
  other's toes."
* It does aggressive **global build reuse**: it checks binary caches first
  (`cache.nixos.org`), and "if a user tries to build a derivation that
  already has been built by any user of the same account, the build result
  will simply [be reused]." Its reuse key is **not** the bare store path
  but the derivation **together with the content of its inputs** (because a
  store path encodes dependencies, not contents), scoped per account with
  trust boundaries (uploaded inputs are usable only by their uploader
  unless cache-signed or inter-account trust is configured).
* It offers **introspection**: an SSH "shell" (`list builds --running`,
  build history) and an HTTP API.

This validates the RFC's direction and sharpens four requirements:

1. **Backend-negotiated concurrency (not client slot locks).** The
   distributed-build *hook*'s per-machine, per-slot file locking and fixed
   `maxJobs` (`build-remote.cc:40-43,151-177`) is the wrong model for an
   elastic backend — it caps parallelism on the client. The `ssh-ng://`
   *store* path already avoids this (concurrency is just "many requests,
   scheduled by the backend"), which is exactly why nixbuild.net steers
   users to `--store ssh-ng://`. The redesign should: (a) let a builder
   **advertise** that it self-schedules / has elastic capacity, so the hook
   does not gate on a local slot count; and (b) treat `maxJobs` for such a
   builder as a client-side concurrency *hint*. **This must be strictly
   opt-in.** Operators rely on `maxJobs` in existing `/etc/nix/machines`
   files as a hard cap (to protect fixed-size machines); changing its
   meaning by default would silently overcommit those builders. The "elastic
   capacity" behaviour is therefore enabled only when a builder explicitly
   advertises it (or an operator sets a new per-machine flag), and the
   default semantics of `maxJobs` are unchanged.

2. **Many concurrent builds (and log streams) per endpoint.** Today the
   serve protocol is one synchronous build per connection, so N parallel
   builds means N SSH connections (`LegacySSHStore` pools up to
   `max-connections`, default 1 — `legacy-ssh-store.hh:39`). The worker
   protocol already multiplexes work and tunnels interleaved `STDERR_*`
   frames, but build-related activities/log lines must be **tagged with
   their originating derivation/build id** (§4.2) so many simultaneous
   builds over one connection remain legible. The **Build Session** (§4.1)
   is the unit that carries this multiplexing.

3. **Reuse as a first-class, observable result.** nixbuild.net's "don't
   build if we can reuse" is the same shape as G3 dedup plus substitution.
   The dedup key should therefore be the **resolved, content-addressed**
   derivation (resolved drv + input content hashes), not the input-
   addressed drv path — this matches both CA derivations and nixbuild.net's
   reuse semantics. The extended `BuildResult` (§4.4) reports
   `deduplicated` and the existing `Substituted`/`AlreadyValid`/
   `ResolvesToAlreadyValid` statuses so the client can *see* that a result
   was reused rather than built.

4. **Reconnection for long builds.** Against an elastic service, a client's
   connection may drop while a long build continues server-side. Build
   Sessions with **stable ids** (§4.1) let a client **re-attach** to an
   in-flight build and resume following its log (replay + live, §4.3.1),
   instead of losing visibility or re-queuing work. **This depends entirely
   on §4.3.3:** a build outliving its connection is *impossible* in the
   stock fork-per-connection daemon, where a client disconnect trips
   `MonitorFdHup` (`nix-store.cc:1009`) and the handling child exits and
   tears the build down. Re-attach therefore works natively only on
   single-process backends (which already keep the build alive), and on the
   stock daemon only once a coordinator (§4.3.3) owns the build. The RFC
   should not promise re-attach for the stock daemon before that spike
   lands.

None of this requires Nix to *become* a scheduler (that stays a non-goal,
and Hydra's job): it requires the protocol to (a) not impose client-side
scheduling where the backend already does it, (b) multiplex many tagged
build/log streams, (c) report reuse, and (d) support re-attach. A backend
like nixbuild.net then "just works" as `--store ssh-ng://`, at full log
fidelity, instead of being a clever workaround.

### 4.8 Hydra compatibility (G7)

Hydra's `hydra-queue-runner` is the primary serve-protocol consumer; it
uses the stripped-down `BasicClientConnection`/`BasicServerConnection`
shared for exactly this purpose (`serve-protocol.hh:96-101`), and
`BuildDerivation` is explicitly annotated as "Used by hydra-queue-runner"
(`nix-store.cc:1020`). The redesign is therefore **strictly additive** on
the serve side:

* The existing serve `BuildDerivation`/`BuildResult` exchange keeps working
  byte-for-byte at version ≤ 2.8. All new capabilities (log frames §4.2,
  `QueryBuildLog` §4.5, extended `BuildResult` fields §4.4,
  `QueryActiveBuilds` §4.3.2, attach/`deduplicated`) are gated behind the
  serve 3.0 version bump (§7) and the `min(client, server)` handshake
  (`serve-protocol-connection.cc:8-33`). A 2.x Hydra against a 3.0 builder
  sees today's behaviour; a 3.0-aware Hydra opts in.
* The new structured log channel and `QueryBuildLog` are designed so Hydra
  can **retire its out-of-band log handling** and consume logs the same way
  the `nix` CLI does — but only when it chooses to.
* The extended `BuildResult` (`builderId`, `deduplicated`, structured
  failure, `logRef`) is directly useful to Hydra's result accounting, so
  the field set should be agreed with Hydra maintainers before freezing the
  serve 3.0 serialisation. (Tracked as an open question, §10.)

The guiding constraint: **no change may require a coordinated Hydra/Nix
flag day.** Old Hydra ↔ new Nix and new Hydra ↔ old Nix must both work,
which the version-gated, additive approach guarantees.

### 4.9 Fail loud: logs on failure, especially in CI (G8 / Gap C)

A failed build must explain itself by default. Three coordinated changes:

1. **Remote failures carry a log tail.** When a build runs on a remote
   daemon, the returned `BuildResult::Failure` must include the structured
   log tail (the same data `fixupBuilderFailureErrorMessage` embeds for
   local builds, `derivation-building-goal.cc:1157`) as a field (§4.4), so
   the client can render "last N log lines" regardless of where the build
   ran. Today that tail is local-only.

2. **A `print-build-logs = on-failure` mode.** `--print-build-logs` is
   currently a boolean gating live output (`progress-bar.cc:350`,
   `main.cc:117`). Add a third mode that keeps live output quiet but, when
   a build fails, dumps that build's **full** captured log (fetched via
   `QueryBuildLog`/Gap-A if the build was remote). This is the CI-friendly
   default: quiet on success, complete log on failure, no need to decide to
   pass `-L` *before* you know something will break. It composes with G2:
   if the live log wasn't retained client-side, the full log is fetched
   from the build store on demand.

3. **Actionable hint when the log can't be inlined.** If neither tail nor
   fetch is available (e.g. an old builder), the error must say *how* to
   get the log — `nix log <drv> --store <build-store>` once Gap A is fixed,
   or the builder's own log endpoint — instead of failing silently.

Interim guidance (works today, documented for users): pass `-L`, redirect
to a file, and print it on non-zero exit; do **not** rely on `nix log`
against an `ssh-ng://` store (Gap A) — use the builder's native log access
(for nixbuild.net, its SSH `shell` / HTTP API) as the post-hoc source.

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

# a failing remote build in CI, with print-build-logs = on-failure:   [G8]
$ nix build "${drv}^*" --store ssh-ng://builder
builder: building '/nix/store/…-whisper-cpp-1.7.5.drv'
error: builder for '/nix/store/…-whisper-cpp-1.7.5.drv' failed (exit 127)
  last 25 log lines (from builder):                          [G8 tail, remote]
  > configure: error: 'cmake' not found in PATH
  > …
  full log:                                                  [G8 on-failure dump]
  > … entire whisper-cpp build log fetched from the builder …

# or, after the fact:
$ nix log "${drv}" --store ssh-ng://builder   # works now        [G2/Gap A/§4.5]
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
  `QueryBuildLog`, `QueryActiveBuilds`, extended `BuildResult` fields,
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
* **Do not freeze the serve 3.0 wire until the Hydra field set is agreed
  (Q4).** Once a 3.0 serialisation ships and is in the wild, its layout is a
  back-compat promise. Phases 1/2 may *prototype* the extended `BuildResult`
  and log frames behind an unstable/experimental version, but the byte
  layout must not be declared stable as 3.0 until the Hydra coordination
  (§4.8) lands — otherwise a later Hydra-driven field change would break the
  very compatibility this section promises.

## 8. Phased implementation plan

Each phase is independently shippable and testable. **Crucially, Phases
0–2 and 4 deliver Gaps A/B/C — the bulk of the operator pain — without any
shared registry or daemon redesign.** They are the "even if dedup never
lands" core. The dedup/attach work (the spike + Phase 3) is a separate
sub-project gated on the §4.3.3 process-model decision and must not block
the log fixes.

* **Phase 0 — `nix log` over `ssh-ng` + persist remote logs (smallest
  win, biggest relief).** Two parts, both pure log-retrieval:
  - **Close Gap A (highest leverage):** implement `getBuildLogExact` over
    the worker protocol (the `FIXME` at `ssh-store.cc:64-67`) by adding a
    `QueryBuildLog` op that forwards to the daemon's `LogStore`, so
    `nix log` works against any `ssh-ng://` store. This alone fixes the
    most-reported case in the §2.6 matrix.
  - On the serve path, stop discarding the log (make `keepLog`/`verbosity`
    suppression conditional at `nix-store.cc:908-909`, persist via
    `addBuildLog`) and add the serve `QueryBuildLog` command; wire
    `nix log`'s store resolution (`libcmd/get-build-log.cc:7-29`) to consult the
    build store.
  Delivers **G2/Gap A** with no streaming work.
  *Touches:* `ssh-store.cc`, `remote-store.{cc,hh}`, `worker-protocol*`,
  `nix-store.cc` (`opServe`/`getBuildSettings`), `serve-protocol*.{hh,cc}`,
  `legacy-ssh-store.cc`, `nix/log` command.

* **Phase 1 — Extended `BuildResult` + fail-loud (Gap C).** Add `logRef`,
  `builderId`, `deduplicated`, and **structured failure detail including
  the log tail** so remote failures are as informative as local ones;
  serialise conditionally; have `build-remote.cc` and the `ssh-ng` path
  print the tail + a `nix log`/builder-log hint on failure. Add the
  `print-build-logs = on-failure` mode (`progress-bar.cc`, `main.cc`,
  `globals.hh`) that stays quiet on success and dumps the failing build's
  full log on failure (fetched via Phase 0 when remote). Delivers the rest
  of **G2** and **G8/Gap C** — the CI requirement "print the logs on
  failure".
  *Touches:* `build-result.{hh,cc}`, `serve-protocol.cc`,
  `worker-protocol*`, `build-remote.cc`, `libmain/progress-bar.cc`,
  `nix/main.cc`, `globals.hh`.

* **Phase 2 — Live log streaming over serve (bridge) + close Gap B.** Add
  the optional log-frame sequence to serve `BuildDerivation`/`BuildPaths`
  and feed frames into the client `Logger` (delivers **G1** for `ssh://`).
  In the same phase, fix **Gap B**: have `buildWithHook` re-emit a
  sub-build's parsed log lines to the ambient logger
  (`derivation-building-goal.cc:737-762`) so nested builds surface upstream.
  *Caveat:* avoid double-logging — when the same daemon both drives the hook
  and is itself the ambient (tunnel) logger, a re-emitted line must not also
  reach the client via a second path. Tag re-emitted lines or emit them only
  through the build's activity so each line appears once.
  *Touches:* `serve-protocol*.{hh,cc}`, `nix-store.cc`,
  `legacy-ssh-store.cc`, `build/derivation-building-goal.cc`.

* **Spike — Cross-process build coordination (§4.3.3), gates Phase 3.**
  *Not code-complete; a design decision with a prototype.* Choose among
  coordinator-process / shared-memory / single-process-daemon, because the
  choice bounds what `QueryActiveBuilds` and session re-attach can promise
  and what the Phase 3 wire surface must look like. Prototype start-or-attach
  + log fan-out for the chosen mechanism on the stock daemon. **No Phase 3
  wire surface is frozen until this lands.** Single-process backends
  (nixbuild.net, single-`RemoteStore` drivers) do not need this and can
  implement §4.3 in parallel.

* **Phase 3 — Build Registry + attach/replay + fan-out.** Implement the
  registry keyed on the resolved drv, the per-Build log broadcaster, replay
  buffer, reference-counted cancellation, and `deduplicated` reporting, on
  top of the spike's mechanism. **Lands first for single-process backends**
  (in-address-space registry); lands for the stock daemon only via the
  coordinator from the spike. Delivers **G3**.
  *Touches (single-process):* backend store implementation, `build-result`.
  *Touches (stock daemon):* the coordinator from the spike, daemon/serve
  handlers, `build/derivation-building-goal.cc`; note there is **no** simple
  in-process `build/worker.{cc,hh}` structure that suffices (see §2.3/§4.3.3).

* **Phase 4 — Store-split log parity + cleanup (low priority).** Verify
  and test that streaming (Phase 2) and fetch (Phase 0) behave identically
  when build store ≠ eval store and when either is remote — in practice
  this should already hold because logs are keyed on the derivation, so
  the phase is mostly a test matrix. Bundle the minor cleanup of lifting
  the `ssh://`+`--eval-store` rejection and making the closure copies
  observable; optionally introduce `realiseRemote(...)`. Delivers **G4**.
  *Touches:* `legacy-ssh-store.cc`, `build-remote.cc`,
  `libcmd/installables.cc`, `store-api`, `tests/functional/`.

* **Phase 5 — Introspection.** `QueryActiveBuilds` + a `nix` subcommand to
  render it; authorisation per §6. Delivers **G5**.

* **Phase 6 — Elastic-backend friendliness.** Let a builder advertise
  self-scheduling / elastic capacity so the hook does not gate on local
  slot locks (`build-remote.cc:151-177`); treat `maxJobs` as a hint for
  such builders; ensure many tagged build/log streams multiplex cleanly
  over one connection; key dedup on the resolved/CA derivation so reuse
  matches the nixbuild.net model; and support session **re-attach** after a
  dropped connection. Delivers **G6**.
  *Touches:* `machines.{cc,hh}`, `build-remote.cc`,
  `build/derivation-building-goal.cc`, registry from Phase 3,
  serve/worker protocol version negotiation.

* **Phase 7 — Converge on `ssh-ng://`.** Make `ssh-ng://` the default,
  fully-featured distributed-build transport (native streaming + dedup),
  keep the serve protocol as the documented compatibility/Hydra surface
  (kept additive throughout per §4.8 / **G7**).

## 9. Testing strategy

* **Functional tests** (`tests/functional/`): extend the existing
  `build-remote*.sh` / `build-hook*.sh` tests with assertions that
  (a) live builder output appears on the client with `-L`, (b) `nix log`
  against the remote (`ssh-ng://`) returns the real log after success and
  after failure (Gap A), and (c) the structured failure carries phase +
  exit status + tail even for a remote build.
* **Fail-loud test (G8 / Gap C):** a *failing* `nix build --store
  ssh-ng://…` with `print-build-logs = on-failure` and **no `-L`** must
  print the failing derivation's full log to stderr and exit non-zero —
  the exact CI scenario from §2.7. Also assert the default (no `-L`, no
  on-failure) still shows at least the failure tail, never a bare
  exit-code-only message.
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

0. **Cross-process coordination mechanism (the blocker, §4.3.3).** Which of
   coordinator-process / shared-memory / single-process-daemon does the
   stock `nix-daemon` adopt for cross-connection dedup/attach? This is the
   prerequisite for Phase 3, §4.7.4 re-attach, and registry introspection on
   the stock daemon, and bounds every promise those make. Listed first
   because it gates the others.
1. **Replay buffer policy.** Full log vs. head+tail cap, and the
   truncation-marker UX for very long builds — its *location* is decided by
   Q0 (process memory vs. coordinator-owned ring buffer).
2. **Cancellation across tenants.** Exact semantics when the *originating*
   session detaches but late joiners remain — confirmed reference-counted
   here, but interactions with `--keep-going` and timeouts need a test
   matrix.
3. **CA resolution timing.** The build key needs the resolved drv; for
   deep CA graphs the resolution itself can race. Does the registry key on
   the resolved drv only, or also expose a "resolving" pre-state that
   later joiners can attach to?
4. **Hydra field set.** The extended `BuildResult` and `QueryBuildLog`
   should be designed *with* Hydra (§4.8) so it can drop its out-of-band
   log handling; the exact serve 3.0 field set must be frozen with Hydra
   maintainers. Coordination is out of scope for this document.
5. **`QueryActiveBuilds` privacy.** Default verbosity for untrusted
   callers (aggregate counts only vs. nothing).
6. **Elastic-capacity advertisement.** How a builder declares "I
   self-schedule / have elastic capacity" — a new field in the machines
   spec / store config, or negotiated in the handshake? And how `maxJobs`
   degrades to a hint without breaking existing `/etc/nix/machines` files.
7. **Reuse key vs. trust.** Keying dedup/reuse on the resolved + content-
   addressed derivation (the nixbuild.net model) interacts with the trust
   model (§6): inputs uploaded by one client must not be reusable by
   another unless cache-signed or explicitly trusted. The exact key and its
   authorisation check need to be specified together.

## 11. Summary

Build logs already stream over `ssh-ng://` and persist via `LogStore` — but
only in fragments. The validated matrix (§2.6–2.7) shows three concrete
holes: **`nix log` is unsupported over `ssh-ng` (Gap A)**, the **serve
(`ssh://`) hop drops logs upstream (Gap B)**, and **build output is hidden
by default so remote failures are silent (Gap C)** — the last is what users
hit first, e.g. a CI `nix build --store ssh-ng://…` that reports only
`exit code 127` with no log.

These three gaps — the bulk of the operator pain — are closed by Phases
0–2 and 4 with **no shared-state work at all**, and that is the spine of
this RFC. The headline dedup/attach feature is more ambitious and honestly
scoped: the stock `nix-daemon` is fork-per-connection (§2.3), so there is
**no shared `Worker`** to lift; cross-client dedup with log fan-out and
re-attach needs a new cross-process coordination mechanism (§4.3.3), which
this RFC frames as a gated design spike. It lands natively and first on
single-process / elastic backends (nixbuild.net-style endpoints), and on
the stock daemon only once that spike resolves.

The whole is unified behind a **Build Session** abstraction and a
**Build Registry** owned by whatever schedules builds, delivered in a
phased, backward-compatible order: `nix log` over ssh-ng (Gap A) → enrich
results + fail-loud (Gap C / G8) → stream + close Gap B → *(spike)* →
dedup/attach → introspection → elastic-backend friendliness → converge on
`ssh-ng://`. Throughout, the serve protocol stays additive so **Hydra keeps
working** (G7), the design **cooperates with elastic multi-tenant backends
like nixbuild.net** (G6), and the eval-store/build-store split — already
functional — simply inherits the same uniform logging as everything else.
