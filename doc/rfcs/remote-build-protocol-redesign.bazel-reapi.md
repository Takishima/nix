# Companion: can this design support the Bazel Remote APIs (REAPI)?

> Companion to [the remote-build-protocol-redesign RFC](./remote-build-protocol-redesign.md).
> Question answered here: *given the APIs defined at
> <https://github.com/bazelbuild/remote-apis>, does our current implementation —
> following the RFC — let us support those, or do we need changes?*

## TL;DR

**Not as-is — supporting REAPI needs new code — but the RFC and the code that
landed under it are unusually well-positioned for it, and every change is
*additive* and *off the frozen client wire*.**

* The **hard semantic decisions** REAPI forces — a content-addressed action
  key, in-flight de-duplication of identical actions, re-attach to a running
  execution, a separate "is it building now" layer vs. a durable result cache,
  pluggable authentication, capability negotiation, architecture-safe routing,
  and "don't cache a transient/infra failure" — are **exactly the decisions the
  RFC already made and the implementation already encodes behind interfaces**
  (`BuildRegistry`, `BuildRegistryKey`, `BuildLease`, `BuildAuthPolicy`, the
  extended `BuildResult`). A REAPI executor slots into the §4.3.4
  "distributed backend behind the endpoint" role the RFC deliberately carved
  out, and the §8.1 guardrails were written precisely so such a backend "stays
  a pure addition."
* What is **missing** is the **transport and the content model**: REAPI is
  gRPC/protobuf with a CAS Merkle-tree of per-file blobs; Nix speaks a bespoke
  framed-binary protocol over SSH with NAR closures keyed by store path. Those
  are real, non-trivial adapters — but they are *new* code beside the existing
  wire, not modifications to (let alone breaks of) the surface the RFC freezes
  (§7).

So: **yes, changes are required; no, the RFC does not have to change to allow
them.** The design bought exactly the seams this needs.

## What REAPI is (the part that matters here)

The [remote-apis](https://github.com/bazelbuild/remote-apis) Remote Execution
API (REAPI v2) is a **gRPC/protobuf** API built on a content-addressed Merkle
DAG. Four services:

| Service | RPCs | Role |
|---|---|---|
| **Execution** | `Execute`, `WaitExecution` (both → *stream* `google.longrunning.Operation`) | run an action; re-attach to a running one |
| **ActionCache** | `GetActionResult`, `UpdateActionResult` | durable result reuse |
| **ContentAddressableStorage** | `FindMissingBlobs`, `BatchUpdate/ReadBlobs`, `GetTree` (+ the `ByteStream` `Read`/`Write` API) | upload/download blobs, stream logs |
| **Capabilities** | `GetCapabilities` | negotiate versions / limits / worker properties |

Core model:

* An **`Action`** = `command_digest` + `input_root_digest` + `platform` +
  `timeout`; everything is a **`Digest`** (`hash` + `size_bytes`). The
  `action_digest` is *the* identity of a unit of work.
* The **input root** is a `Directory` Merkle tree of `FileNode`s, each a blob
  addressed by content digest, staged into the CAS (clients call
  `FindMissingBlobs` then `ByteStream.Write`/`BatchUpdateBlobs`).
* A **`Command`** = `arguments` (argv) + `environment_variables` +
  `output_paths` (relative paths declared **up front**) + `working_directory`.
* **`ActionResult`** = `output_files`/`output_directories` (by digest, optional
  inline), `exit_code`, `stdout_raw`/`stdout_digest`,
  `stderr_raw`/`stderr_digest`, `execution_metadata` (incl. `worker`).
* **Execution is asynchronous and streamed.** `Execute` returns a stream of
  `Operation`s carrying `ExecuteOperationMetadata` with a `stage`
  (`CACHE_CHECK` → `QUEUED` → `EXECUTING` → `COMPLETED`); live output is a
  `ByteStream` named by `stdout_stream_name`; the final `ExecuteResponse`
  carries `cached_result` and a `server_logs` map of `LogFile`s (by digest).
  **`WaitExecution(operation_name)` re-attaches** to a running execution.
* **De-dup is first-class:** concurrent `Execute`s for the same `action_digest`
  **may be merged** by the server, *unless* the `Action` sets `do_not_cache`
  (which also bars caching the result).
* **`Platform.properties`** route an action to a matching worker (OS/ISA/etc.).

## The mapping: REAPI concept → what already exists here

This is the striking part. The implementation that landed under the RFC is close
to an isomorphism of REAPI's *execution semantics*:

| REAPI concept | Already in this design / code | Where |
|---|---|---|
| `action_digest` — content key folding command+inputs+platform | `BuildRegistryKey = printStorePath(resolved drv)` — `hash(resolved derivation)`, which folds in inputs *and* `system`/features → **architecture-safe**, never the input-addressed `.drv` or an output path | `build-registry.hh` `BuildRegistryKey`; RFC Blocker 1, §8.1 g1 |
| In-flight merge of identical `Execute`s | `BuildRegistry::startOrAttach` → HIT/MISS, `BuildAttachment::deduplicated`, "at most one live build per key" invariant | `build-registry.hh` `startOrAttach`, `BuildRegistry` invariant |
| `do_not_cache` ⇒ no merge / no caching; transient failures not cached | RFC rule: transient/infra failures **must not be cached or reused** (§4.4); registry HIT only for genuinely shared work | RFC §4.4, §4.7 req. 5 |
| `WaitExecution(operation_name)` — re-attach to a running execution | Stable session id + **re-attach** to an in-flight build; `BuildLease` epoch is the fencing token a server uses to detect a builder that vanished and re-dispatch | RFC §4.7.4; `build-registry.hh` `BuildLease` |
| `ActionCache` (`Get/UpdateActionResult`) — durable reuse | The **durable reuse cache** layer, kept *separate* from the live coalescing registry, with a CA-vs-signature reuse-verification policy | RFC §4.3.5 seams 3–4 |
| `ServerCapabilities` / `GetCapabilities` | Version handshake (`min(client,server)`) + elastic-capacity advertisement (O7) | RFC §7, O7 |
| `Platform.properties` worker routing; heterogeneous pools | Resolved-drv key carries `system`+features ⇒ x86/aarch64 builds are distinct keys that never coalesce; routing is the orchestrator's job below the wire | RFC §4.7 "heterogeneous pools", §8.1 g1 |
| `ExecuteOperationMetadata.stage` + `stdout_stream_name` | Build Session structured-log multiplex with per-build tagging + replay/live stream | RFC §4.1, §4.2; `build-registry.hh` `BuildLogFrame` |
| `ExecuteResponse.server_logs` (LogFile by digest); `stdout_digest` | `BuildResult::logRef` + `QueryBuildLog` (fetch the persisted log by key) | `build-result.hh` `logRef`; RFC §4.5 |
| `ActionResult.exit_code` / `stderr` / `execution_metadata.worker` / `cached_result` | `BuildResult::exitCode`, `logTail`, `builderId`, `deduplicated` | `build-result.hh` |
| Network auth (mTLS/identity) vs. local peer-cred | `BuildAuthPolicy` is **pluggable**; `BuildAuth.identity` is opaque; no call site inlines `SO_PEERCRED` | `build-registry.hh` `BuildAuthPolicy`; RFC §4.3.4, §8.1 g2 |
| Lost worker / eviction ⇒ re-dispatch under same identity | `BuildLease` CAS+fence seam; the §4.3.4 "builder evicted while endpoint lives" case | `build-registry.hh` `BuildLease`; RFC §4.3.4, §4.7.4 |

The single most important structural fact: **the RFC already treats "a service
that owns the build behind its endpoint" as a first-class deployment class
(§4.3.4) and forbids the implementation from foreclosing it (§8.1 guardrails).**
A REAPI executor *is* such a service. So adopting REAPI does not fight the
design — it occupies a slot the design reserved.

## What is genuinely missing (the changes you'd need)

All of these are **net-new, additive** components. None requires editing the
frozen serve/worker wire (§7); they live in the "backend implementer's space"
the RFC ships no turnkey for (§4.10).

1. **A gRPC/protobuf transport.** The RFC deliberately chose *Option B* — reuse
   the worker-protocol framing — and explicitly did **not** adopt gRPC. The tree
   has **no** protobuf, gRPC, `ByteStream`, or REAPI code (verified). To either
   *expose* Nix as a REAPI server, or *drive* a REAPI backend (Buildbarn,
   BuildBuddy/Buildfarm, EngFlow, nixbuild.net's REAPI, …), you need a new gRPC
   adapter. Most naturally this is **a new `Store`/executor backend** that the
   orchestrator/coordinator dispatches to, or a gateway that fronts Nix as
   `ssh-ng://` — *interop at the §4.3.4 orchestrator boundary*, not a
   replacement for the client wire.

2. **A NAR/closure ↔ CAS-Merkle-tree bridge — the deepest gap.** REAPI inputs
   are a `Directory` DAG of individually content-addressed file blobs, staged
   via `FindMissingBlobs` + `ByteStream`/`BatchUpdateBlobs`. Nix ships closures
   as **NARs keyed by store path**, copied with `AddToStoreNar` /
   `copyDrvsFromEvalStore`. A NAR is not a REAPI `Directory`, and a Nix
   store-path hash is not a REAPI file-content digest. Bridging needs: (a)
   walking a derivation's input closure into `Directory`/`FileNode` protos; (b)
   a blob-store ↔ CAS shim (or running the Nix store *as* a CAS); (c) mapping
   results back. This is the largest single piece and it does not exist in any
   form today.

3. **A `Derivation` ↔ `Action`/`Command` translator.** A REAPI `Command`
   declares `output_paths` **relative and up front**; a Nix `.drv` produces
   **store-path outputs**, and for CA derivations the output paths are *not
   known up front* (they're computed from build output). Mapping the sandbox /
   fixed-output / structured-attrs machinery onto `Command` + `Platform` is a
   genuine impedance match, not a field rename.

4. **Finish the reserved failure-classification fields.** REAPI distinguishes
   `RESOURCE_EXHAUSTED` / `UNAVAILABLE` (retryable, infra) from a real build
   error, which is precisely the **transient/`failure-class` + resource-hint**
   the RFC specced in §4.4. Those fields are **not yet in `BuildResult`** — only
   the legacy `TransientFailure` *status* exists (verified in
   `build-result.hh`). The diagnostic core that *did* land
   (`logRef`/`failurePhase`/`exitCode`/`logTail`/`deduplicated`/`builderId`) is
   the right shape but is the diagnostic half, not the retry-classification
   half. This one is in-scope for the RFC's own roadmap and is the cheapest of
   the four.

## Bottom line

* **Can we point Nix at a `grpc://…` REAPI endpoint today?** No. There is no
  protobuf/gRPC/CAS code, and the content models differ. **Changes are
  required.**
* **Does the RFC/implementation foreclose REAPI or make it expensive to
  retrofit?** No — the opposite. The semantic core REAPI demands
  (content-addressed action key, in-flight merge, re-attach + lease/fencing,
  live-registry vs. durable-reuse-cache split, pluggable auth, capability
  negotiation, arch-safe routing, no-cache-on-transient-failure) is **already
  decided and already behind interfaces**. A REAPI executor is a §4.3.4
  distributed backend, and §8.1's guardrails exist so it "stays a pure
  addition."
* **The work, all additive and off the frozen wire:** (1) a gRPC/protobuf REAPI
  adapter (new executor `Store` / orchestrator backend); (2) a NAR/closure ↔ CAS
  Merkle-tree + blob/ByteStream bridge; (3) a `Derivation` ↔ `Action`/`Command`
  translator; (4) land the reserved transient/resource-class result fields
  (§4.4). The first three are backend-implementer work the RFC explicitly
  declines to ship turnkey (§4.10); the fourth is on the RFC's own roadmap.

In one sentence: **REAPI interop is something this design *enables* rather than
*provides* — the right place for it is a build-executor backend behind the
endpoint (§4.3.4), reached through the registry interface and the resolved-drv
key the implementation already commits to, with no change to the frozen client
wire.**
