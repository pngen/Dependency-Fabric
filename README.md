# Dependency Fabric

**Dependency Fabric is an open-source, vendor-neutral C++20 runtime for governing dependency identity, readiness, generations, invalidation, propagation, recovery, and execution eligibility across data, state, models, artifacts, resources, and distributed AI infrastructure.**

**It answers one systems question:**

> **What does this work depend on, which dependency generations are authoritative and ready now, what became invalid, and what must be recomputed, recovered, or blocked as a result?**

## The systems boundary

Infrastructure dependencies are not static configuration. They are live, generation-bound runtime relationships whose readiness, invalidation, and recovery must remain explicit as upstream state changes.

Dependency Fabric sits **beside Workload Fabric and above lower-level state/artifact/resource runtimes.** It owns the *explicit dependency graph and its current authority/readiness semantics* - and nothing else. Adjacent boundaries are explicit:

- **Workload Fabric** governs durable workload lifecycle.
- **Execution Fabric** governs authoritative execution attempts and logical commit.
- **Artifact Fabric** governs artifact lifecycle.
- **State Provenance** governs derivation history.
- **Compatibility Registry** governs compatibility decisions.
- **Resource Broker** governs resource arbitration.
- **Dependency Fabric** governs the graph that determines whether all required prerequisites are current enough for downstream work to proceed.

The defining thesis:

> **Infrastructure dependencies are not static configuration. They are live, generation-bound runtime relationships whose readiness, invalidation, and recovery must remain explicit as upstream state changes.**

## What Dependency Fabric is not

It is not a workflow engine, a scheduler, a provenance store, a compatibility registry, an artifact manager, or a workload runtime. It does not own execution-attempt authority, artifact lifecycles, provenance history, compatibility decisions, resource arbitration, or the full workload lifecycle. Integration with Workload/Execution/Artifact/Provenance/Compatibility/Resource runtimes happens through narrow, read-only **adapters**.

## Core model

### Strongly typed identities and generations

Every identity and generation is a distinct C++ type (`dependency_fabric::Strong<Tag>`). Numerically equal values in different authority domains are **not** interchangeable. The runtime models at least:

- identities: `DependencyNodeId`, `DependencyEdgeId`, `DependencySetId`, `CoordinatorEpoch`, `SourceId`, `SourceBootId`, `ProducerId`, `ConsumerId`
- generations: `DependencyNodeGeneration`, `DependencyEdgeGeneration`, `DependencySetGeneration`, `ArtifactGeneration`, `StateGeneration`, `ResourceGeneration`, `ReadinessGeneration`, `InvalidationGeneration`, `RecoveryGeneration`, `PolicyGeneration`

Generation axes for distinct authority domains are never folded into a single generic integer.

### Nodes, edges, dependency sets

- **Nodes** have a typed kind (`DATA`, `TENSOR`, `KV_STATE`, `CHECKPOINT`, `MODEL`, `ADAPTER`, `KERNEL`, `EXECUTION_GRAPH`, `ARTIFACT`, `SERVICE`, `RESOURCE`, `TOPOLOGY`, `CAPABILITY`, `EXECUTION_RESULT`, `WORKLOAD_PHASE`, `POLICY`, `UNKNOWN`), an append-only generation history, a current authoritative generation, per-domain generation axes, and a readiness state.
- **Edges** have a typed kind (`REQUIRES`, `OPTIONAL`, `PRODUCES`, `DERIVES_FROM`, `INVALIDATES_WITH`, `COMPATIBLE_WITH`, `REQUIRES_RESOURCE`, `REQUIRES_CAPABILITY`, `REQUIRES_READY`, `REQUIRES_FRESH`, `REQUIRES_GENERATION`), carry declarative predicates (generation exact/minimum/compatible-set, required readiness level, capability, resource availability, freshness, integrity, provenance confidence, completion state), and are required or optional.
- **Dependency sets** group a consumer prerequisites under `ALL_OF`, `ANY_OF`, `AT_LEAST_N`, or `OPTIONAL_GROUP`.

Readiness is explicit and derived, never assumed from existence alone. A node may be `UNKNOWN`, `DECLARED`, `DISCOVERED`, `PRESENT`, `VALIDATING`, `READY`, `DEGRADED`, `STALE`, `INVALIDATED`, `REVALIDATION_REQUIRED`, `MISSING`, `FAILED`, `RECOVERING`, `BLOCKED`, `SUPERSEDED`, or `RETIRED`. **UNKNOWN never becomes READY.**

### Cycles

The graph is a DAG. Cycles are rejected by default: self-cycles, multi-node cycles, duplicate edges, and stale generation regressions are all refused with typed outcomes (`REJECT_CYCLE`, `REJECT_DUPLICATE_ID`, `REJECT_STALE_NODE_GENERATION`, `REJECT_STALE_EDGE_GENERATION`, `REJECT_UNKNOWN_DEPENDENCY`).

### Invalidation, supersession, recovery

Invalidation distinguishes *why* a node became non-ready: upstream gone, upstream generation changed, compatibility changed, resource disappeared, freshness expired, integrity failed, or authority advanced. Invalidation can be direct, recursive, bounded (max depth), or generation-based, and is policy-controlled with respect to optional-edge propagation. Generations are **append-only**; historical generations are never mutated to simulate current validity.

Recovery is explicit and returned as structured intent to the owning runtime (Checkpoint Store, Artifact Fabric, Workload Fabric, Execution Fabric, Resource Broker, and so on). Dependency Fabric determines *what became dependent-invalid and what recovery obligations now exist*; it never performs the actions itself.

Cascading recovery is modeled: a model generation change leads to adapter compatibility stale, execution graph stale, compiled kernel plan stale, and workload phase BLOCKED; a recovery plan identifies exactly which nodes require rebuild/revalidation.

### Stale-authority rejection

Every mutation carries a `(source, boot, epoch)` authority tuple. A source registers at a `SourceBootId`; a fresh boot replaces the prior incarnation. Messages with an old epoch, old boot id, or a non-monotonic generation are rejected before they can mutate current graph state. A new upstream generation never automatically validates descendants built against an older generation.

### Persistence

The graph persists via a versioned binary format with strong integrity checking: magic, version, self-describing lengths, and a content digest. On load it rejects truncation, corruption, malformed lengths, invalid enums, duplicate ids, generation regressions, duplicate authority, broken edge endpoints, self-cycles, cyclic reconstruction, and trailing garbage. **Recovered dynamic readiness is conservative**: dynamic runtime evidence becomes `REVALIDATION_REQUIRED` after a coordinator restart, while static durable facts may remain durable. Only republishing current evidence restores `READY`.

## Deterministic, inspectable explanations

A caller can ask why a node is `READY` or `BLOCKED`, which dependency blocks it, which generation mismatch caused the rejection, which upstream invalidation affected it, what changed since it was last READY, which descendants are affected, and which recovery actions are required. Public outcomes include `READY`, `BLOCKED`, `DEGRADED`, `REVALIDATION_REQUIRED`, `RECOMPUTE_REQUIRED`, `RECOVERY_REQUIRED`, `REJECT_STALE_EPOCH`, `REJECT_STALE_BOOT`, `REJECT_STALE_NODE_GENERATION`, `REJECT_STALE_EDGE_GENERATION`, `REJECT_CYCLE`, `REJECT_DUPLICATE_ID`, `REJECT_INVALID_TRANSITION`, `REJECT_UNKNOWN_DEPENDENCY`, `REJECT_MALFORMED`, `UNKNOWN`, and `ACCEPTED`.

## Performance

The graph uses indexed adjacency structures, so common operations are near-linear. Benchmarks report exact graph size/density and per-operation cost (node registration, edge registration, readiness evaluation, blocker queries, ancestor/descendant traversal, invalidation and recovery propagation, generation supersession, dependency-set evaluation, persistence save/recover, and concurrent read-heavy queries) for graphs up to tens of thousands of nodes and hundreds of thousands of edges.

## Concurrency and correctness

The graph is thread-safe: mutations take an exclusive lock, queries take a shared lock. Property-based tests generate randomized DAGs and event sequences and prove invariants (acyclic, generations never move backward, current authoritative generation is unique, stale sources cannot mutate current state, invalid required dependencies block readiness, optional loss obeys policy, descendants never become READY while a required ancestor is invalid, superseded generations cannot satisfy current exact-generation requirements, recovery does not bypass revalidation, persistence round-trips preserve topology, dynamic evidence is not silently fresh after recovery, affected closure matches reference traversal). Genuine multi-threaded tests race invalidation vs. recovery, publication vs. supersession, edge addition vs. readiness query, and concurrent reads during generation advancement.

## Multiprocess proof

A real multiprocess distributed proof runs over loopback TCP with a framed, checksummed protocol and explicit `HELLO`/`REGISTER`, bounded decoding, and clean shutdown. It uses actual OS processes: one dependency coordinator, two producer/source worker processes, a consumer/controller process, and a fresh worker incarnation. It publishes the graph, kills Worker A as a real OS process, advances coordinator/source authority, replays a stale Worker A message (rejected), publishes Model gen2 from a fresh Worker A, verifies the artifact goes STALE and the consumer BLOCKS, rebuilds/rebinds, restores readiness, persists, **restarts the coordinator, reconstructs the graph, requires dynamic readiness revalidation, republishes current evidence, and returns to READY**.

## CUDA-backed dependency-gated execution proof

On a CUDA machine (`sm_120` on the RTX 5090) the CUDA proof demonstrates that dependency readiness gates real accelerator work using `cudaMalloc`, host-to-device copies, a real kernel, synchronization, device-to-host copies, CPU-reference verification, and `cudaFree`, with device memory returning to baseline. Scenarios cover a valid dependency chain (CUDA allowed, parity passes), a stale model generation (CUDA rejected before launch, then fresh generations allow), invalidation during a segmented workflow (future segments blocked until recovery), a real resource/capability dependency (a real measured `sm_120` profile is READY while an intentionally incompatible SYNTHETIC capability stays blocked), and recovery (persist, restart, conservative revalidation, real device evidence republished, CUDA succeeds). Synthetic evidence is labelled **SYNTHETIC**; the proof makes no claims about multi-GPU, multi-node accelerator fabrics, RDMA, NVLink, or hardware discovery that was not actually measured.

## Building

Requires CMake 3.22+, a C++20 compiler, and Ninja or Microsoft Visual Studio.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Options: `DF_BUILD_TESTS` (ON), `DF_BUILD_BENCHMARKS` (ON), `DF_BUILD_EXAMPLES` (ON), `DF_BUILD_TOOLS` (ON), `DF_BUILD_CUDA` (OFF), `DF_BUILD_SHARED` (OFF), `DF_ENABLE_SANITIZERS` (OFF, non-MSVC).

The library installs a CMake package with an exported target `DependencyFabric::DependencyFabric`. Downstream consumers use `find_package(DependencyFabric CONFIG REQUIRED)` and link `DependencyFabric::DependencyFabric`.

## Repository layout

- `include/dependency_fabric/` - public API (identities, enums, graph, coordinator, protocol, persistence, adapters).
- `src/graph/` - graph core, readiness evaluation, invalidation, recovery traversal, enum parsers.
- `src/persistence/` - versioned, integrity-checked binary format.
- `src/protocol/` - framed, checksummed TCP protocol.
- `src/coordinator/` - coordinator application and server loop.
- `src/adapters/` - narrow integration interfaces and reference adapters.
- `examples/`, `tests/`, `benchmarks/`, `tools/`, `cuda/`.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
