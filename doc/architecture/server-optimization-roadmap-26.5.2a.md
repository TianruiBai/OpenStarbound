# OpenStarbound 26.5.2a Server Optimization Roadmap

This roadmap is the first concrete post-Phase-6 server-performance slice after `26.5.1a` in the `Ethereal Drake` release line. It targets `26.5.2a` as an alpha preview in that same release line, focused on reducing the remaining crowded-world hot-thread problem without changing legacy gameplay, Lua, save, or packet behavior by default.

Vulkan and broader graphics backend modernization are intentionally out of scope for this release slice. The current bottleneck is server simulation and replication, not graphics API overhead.

## 1. Release Thesis

`26.5.2a` should reduce p95/p99 tick cost for one crowded authoritative world by removing avoidable serial bookkeeping and expanding snapshot-safe worker work. It should not try to make entity or Lua mutation parallel by default.

The expected outcome is a visibly cooler hot world thread under packet-heavy, liquid-heavy, wiring-heavy, generation-heavy, and save-heavy workloads. The release should be judged by measured workload deltas, not by thread count.

Post-pass review: [server-performance-followup-review-26.5.2a.md](server-performance-followup-review-26.5.2a.md) summarizes the current code state after Phase 0-6 and the extra `26.5.2a` server optimization passes, with ranked follow-up paths for fixed workload captures, queue-only send defaulting, storage spike reduction, and subsystem bookkeeping.

Self-workload capture: [server-self-workload-capture-26.5.2a.md](server-self-workload-capture-26.5.2a.md) records the first repeatable in-process game-mechanism capture and the compatibility-sensitive gates for packet prep, storage, wiring, liquid/falling blocks, entity, and Lua paths.

## Implementation Progress

This roadmap is now being tracked as an implementation checklist. Status values are:

- Done: implemented with focused regression coverage and local validation.
- Active: the current implementation slice.
- Next: planned for this release after the active slice.
- Pending: scoped but not started.

Current checklist:

1. Done: sector-to-client fan-out index for tile, tile-damage, and liquid update queueing, with `sectorFanout` diagnostics and focused coverage.
2. Done: first monitoring-region snapshot reuse slice, with precomputed player-active signal regions and `regions=builds/rects/splits/reuses` diagnostics for liquid and packet-prep consumers.
3. Done: liquid no-limit membership cache using bucketed region candidates and unchanged-region rebuild skips, with `liquidCache=builds/skips/regions/buckets/lookups/candidates/hits` diagnostics and focused engine coverage.
4. Done: per-entity serialization counters for packet-prep cost attribution, with `entitySerialize=type=store:calls/bytes first:calls/bytes delta:calls/bytes` diagnostics and focused ItemDrop coverage.
5. Done: immutable entity net-state input research and deeper `writeNetState(0)` equivalence tests, including byte-equivalent but version-advancing first writes.
6. Done: storage timing counters for world sync/readChunks phases, with byte-identical B-tree insert filtering to reduce repeated save spikes.
7. Done: queue-only network send experiment measured with in-process dummy clients, default-enabled as `queueOnlyConnectionSend=true`, with `queueOnlyConnectionSend=false` preserving the legacy eager-write fallback.
8. Done: guarded empty `EntityUpdateSetPacket` suppression behind `skipEmptyEntityUpdateSets`, with default-off legacy behavior, packet-prep diagnostics, and focused coverage.
9. Done: guarded empty `SystemWorldUpdatePacket` suppression behind `skipEmptyUpdatePackets`, with default-off legacy behavior, `/worldstats` packet diagnostics, and focused coverage.
10. Done: compatibility-sensitive entity/Lua attribution under `subsystemBaselineMetrics`, with entity copy/sort/update/metadata timings, bounded per-entity-type callback attribution, Lua script-context readiness/timing diagnostics, and no off-thread entity/Lua mutation.
11. Done: per-subsystem mutation gate reporting for requested liquid/falling/wiring/entity/Lua experiments, including fixed-seed, dependency-analysis, mod-visibility, and implementation blockers.
12. Done: narrow core `SocketPoller` API seed for readable/writable interests, unregister, wake, timed waits, and ready socket handles, with focused loopback TCP coverage and no runtime network-worker integration yet.
13. Done: dirty wiring-network signature prototype under the existing serial full-scan fallback, reporting clean versus dirty topology/output signatures and clean/dirty entity counts for future skip eligibility.
14. Done: first opt-in fixed-seed mutation-signature preflight for liquid, falling blocks, and wiring under `mutationParallelismFixedSeedSignatures`, including deterministic random seeds for that preflight mode, serial signature diagnostics, and focused stability coverage. This is still a gate artifact, not mutation parallelism.
15. Done: first opt-in mutation shadow-worker slice for liquid, falling blocks, and wiring. When the subsystem flag, fixed-seed signature gate, dependency-analysis gate, and mod-visibility gate are all explicitly enabled, the Phase 6 worker pool now runs immutable signature jobs and compares them against the serial owner-thread signature with visible worker/check/divergence/fallback counters. Entity and Lua requests remain implementation-blocked.
16. Active: fixed workload captures, release validation, and metric comparison against the `26.5.1a` baseline. The repeatable self-workload, queue-only, and save/disconnect storage captures are available as disabled `ServerMeasurement` tests, and the current local validation build is green.

## 2. Current Bottleneck Statement

The remaining jam is a hot authoritative world thread:

- one `WorldServerThread` still owns one `WorldServer::update()` sequence
- entity updates, Lua/script contexts, damage, wiring, liquid, falling blocks, storage tick/generation, entity removal, and packet ordering decisions remain serial
- Phase 0-6 has relieved surrounding pressure, especially handshakes, async persistence, connection-worker ownership, packet-sector prefill, storage-generation planning, and diagnostics
- Phase 6 mutation-parallelism flags now either remain blocked by explicit gates or, for liquid/falling/wiring only, run shadow-worker signature checks after fixed-seed, dependency-analysis, and mod-visibility gates are all enabled

The practical goal for `26.5.2a` is therefore not full single-world multicore scaling. The goal is to reduce the serial work that does not need to be serial, then prepare the data contracts needed for later subsystem experiments.

## 3. Non-Goals

Do not include these in `26.5.2a` unless they become explicitly separate release tracks:

- Vulkan renderer bring-up or graphics backend packaging
- default parallel entity updates
- default parallel Lua execution
- hidden changes to immediate world mutation visibility
- packet format changes
- save format changes
- protocol version changes
- removal of serial fallbacks or differential checks
- removal of the legacy handshake fallback before broader soak coverage

## 4. Workstream A: Fixed Workload Gates

Goal: make every optimization claim reproducible.

Tasks:

1. Define the `26.5.2a` fixed workload set: crowded hub, high-fan-out replication, liquid-heavy region, wiring-heavy base, generation-heavy exploration, save/disconnect spike, login burst, and modpack smoke.
2. Record baseline p50/p95/p99 for universe loop, world thread, world-update subphases, packet preparation, persistence queue, network worker wakeups, and Phase 6 worker helpers.
3. Add or update command output notes for collecting `/serverstatus`, `/worldstats`, and `/servernetstats` during the workloads.
4. Done: add `ServerMeasurement.DISABLED_GameMechanismSelfWorkloadCapture`, a direct `WorldServer` mechanism capture covering client windows, liquid edits/collection, tile damage, falling blocks, wire objects, item-drop replication, packet prep, storage sync, dirty-sector diagnostics, entity/Lua attribution, and full snapshot export. Latest local result on 2026-05-14 with opt-in empty update-set suppression enabled in the harness: `packets=22085`, `tile=3116`, `liquid=15944`, `tileDamage=1008`, `falling=30/1216/1539/324`, `wiring=48/144/144/144/144`, `wiringDirty=144/141/3/3/3/141/3`, `entityCreate=92`, `entityUpdate=784`, `entityUpdateDeltas=4800`, `updateSets=784/4800/0/176`, `sectorFanout=5044/20176/0`, `netStateCache=7968/7464`, `entity=240/3891/720/10/3891/3891/230/162/14890/622`, `entityTypes=object=720/720/0/1047/75,itemDrop=3171/0/10/13441/113`, `lua=240/240/240/240/569/12`, `luaContexts=OpenStarbound=240/240/240/569/12`, `storage=1/64/64/64/66/65/1/65/6286`, `dirty=32/32/3/0/32/0`, `dirtySync=32/0/0`, `dirtySnapshot=0/32`, `snapshotSync=32/18360`, `elapsedUs=166491`.
5. Done: add `ServerMeasurement.DISABLED_SaveDisconnectStorageCapture`, a direct `WorldServer` storage capture covering repeated ordinary `sync()` passes and repeated full `readChunks()` exports under unchanged and dirty tile sectors. Latest local result on 2026-05-14: `dirtyTileEdits=64`, `syncPasses=5`, `snapshotExports=10`, `sync=5/100/300`, `btree=46/570/4295/41601/614`, `storeTypes=2/14/0:24/276/0:20/280/0:0/0/0:0/0/0`, `dirty=24/24/0/0/20/0`, `dirtySync=24/76/0`, `dirtySnapshot=0/200`, `snapshot=10/410/32196/220`, `snapshotSync=200/85782`, `elapsedUs=162640`.
6. Re-enable or document the `world_benchmark` utility target only if it can run without disturbing normal builds; otherwise keep it as a manual profiling harness.
7. Store benchmark world setup notes with enough detail that future releases can rerun the same workload.

Must-ship acceptance:

- every optimization merged for this release identifies the workload and metric it is meant to improve
- p95/p99 world-thread and `WorldServer::update()` numbers are available before and after each substantial change
- modpack smoke remains part of the release gate

## 5. Workstream B: Serial Hot-Path Bookkeeping Wins

Goal: reduce the hot world thread while preserving legacy exact behavior.

Priority tickets:

1. Done: sector-to-client fan-out index for tile, tile-damage, and liquid update queueing. Replaced repeated per-tile full-client scans with sector-local subscriber lookup while preserving per-client packet order. Diagnostics: `sectorFanout=lookups/recipients/misses` in `LogMap`, `/serverstatus`, and `/worldstats`.
2. Done: first monitoring-region generation/reuse slice. `WorldTickSnapshot` now carries precomputed player-active signal regions, counts downstream monitoring-region reuse, and exposes `regions=builds/rects/splits/reuses` through `LogMap`, `/serverstatus`, and `/worldstats`. Further liquid-cell membership pruning belongs to the liquid no-limit cache ticket.
3. Done: liquid no-limit membership cache. Replaced the per-active-cell full `m_noProcessingLimitRegions` scan under a processing limit with bucketed region candidates that are still confirmed with exact `RectI::contains` checks, and skip rebuilding the bucket map when monitoring regions are unchanged. Diagnostics: `liquidCache=builds/skips/regions/buckets/lookups/candidates/hits` in Phase 6 subsystem output, `/serverstatus`, and `/worldstats`.
4. Done: dirty wiring-network signature prototype. The serial full scan remains the only execution path, but `wiringDirty=checks/clean/dirty/topology/output/cleanEntities/dirtyEntities` now reports which loaded networks are unchanged versus topology/output dirty so future skip eligibility can be measured before any behavior change.
5. Done: per-entity serialization counters. Packet preparation now attributes create-store serialization, first-observation `writeNetState(0)`, and later delta `writeNetState(version)` calls/bytes by `EntityType`. Diagnostics: `entitySerialize=type=store:calls/bytes first:calls/bytes delta:calls/bytes` in `/serverstatus` and `/worldstats`.
6. Done: opt-in empty entity update-set suppression. `skipEmptyEntityUpdateSets=false` preserves legacy default packet behavior; enabling it suppresses only empty `EntityUpdateSetPacket`s and reports `entityUpdateSets=emitted/deltas/empty/skipped` diagnostics.
7. Done: opt-in empty system-world update suppression. `skipEmptyUpdatePackets=false` preserves legacy default packet behavior; enabling it suppresses only `SystemWorldUpdatePacket`s with no object or ship deltas and reports `packets=updates:emitted/objectDeltas/shipDeltas/empty/skipped` in `/worldstats`.

Must-ship acceptance:

- default behavior remains packet/save/Lua compatible
- each cache or index has invalidation coverage and a serial fallback
- diagnostics expose enough hit/miss/work counts to decide whether the optimization is worth keeping

## 6. Workstream C: Packet Preparation Expansion

Goal: move more replication prep off the owner thread only after immutable inputs are proven.

Priority tickets:

1. Done: finish immutable net-state input research for first-observation and later-delta paths. `writeNetState(0)` full writes are byte-equivalent for unchanged state but still advance the top-level net version, so create-store bytes can be shared from `EntityCreateSnapshot`, while first-observation net-state writes must remain per-client owner-thread work until byte generation is separated from version advancement.
2. Done: add deeper `writeNetState(0)` equivalence tests, including cases where first writes advance entity net versions.
3. Done: measure and gate empty update-set packet suppression. The self-workload capture found `180` empty update-set packets out of `960`; the opt-in path suppresses them while preserving the default legacy stream.
4. Done: measure and gate empty system-world update packet suppression. A direct `SystemWorldServer` fixture proves the default still emits empty no-op updates, while the opt-in path skips only packets whose object and ship update maps are both empty after an initial ship delta.
5. Next: separate byte generation from owner-thread version advancement where possible, or explicitly document why a path must remain serial.
6. Extend workerized packet preparation from sector tile-array prefill toward entity create/update payload preparation only when the worker consumes immutable inputs and the owner thread performs the visible merge.
7. Keep default-enabled serial-versus-worker differential checks and divergence counters for every expanded packet-prep worker path.

Must-ship acceptance:

- no packet ordering changes
- no entity net-version advancement happens on worker threads unless the input contract is immutable and tested
- divergence uses the serial result and increments visible counters
- controlled entity create/update packet payload tests cover the expanded path

## 7. Workstream D: Storage And Persistence Depth

Goal: reduce save, disconnect, and sync spikes without weakening durability.

Priority tickets:

1. Done: default-off dirty-sector write filtering for ordinary world storage `sync()`, using dirty marks for generation, tile edits, entity changes, unload, and unique-index updates. The `storageDirtySectorFiltering` gate requires one committed full sync before it can skip clean sectors, reports skipped clean sectors in `dirtySync=marked/unmarked/skipped`, and leaves `readChunks()` full snapshot pre-sync serial.
2. Started: reduce avoidable `readChunks()` full exports in ship/disconnect paths where incremental chunk updates are available. The first gate is now covered by `MulticorePhaseTest.ServerOptimizationShipChunkSnapshotsAreUpdateEquivalent`, which verifies current `ServerClientContext` changed/added/removed/no-op chunk update semantics before any export shortcut is implemented.
3. Done: add storage timing counters for sector copy, entity store, tile store, compression, B-tree insert, commit, full snapshot export, `readChunks()` pre-sync work, per-store-surface write/skip/remove attribution, and dirty-sector reason/visit attribution. Diagnostics: `storageTiming=sync:... entity:... tile:... copy:... compress:... btree:... storeTypes:... dirty:... dirtySync:marked/unmarked/skipped dirtySnapshot:... commit:... snapshot:... snapshotSync:...` in `/serverstatus`, `/worldstats`, and `LogMap`.
4. Done: skip byte-identical B-tree inserts during world storage writes. This keeps the serialized compatibility surface unchanged while avoiding repeated leaf rewrites when periodic sync serializes unchanged sectors or metadata.
5. Evaluate async compression only from immutable sector snapshots; do not expose live `WorldStorage` or `BTreeDatabase` to workers.
6. Profile `BTreeDatabase` cache size and commit cadence before considering any backend change.

Must-ship acceptance:

- old saves and newly written saves load through existing versioning paths
- shutdown drains required writes
- write failures remain visible and covered by fallback/retry accounting
- no live mutable storage object crosses into a worker job

## 8. Workstream E: Network Readiness Hardening

Goal: prepare the eventual readiness-driven network backend while keeping the current worker-owned connection model stable.

Priority tickets:

1. Done: move `UniverseConnectionServer::sendPackets()` toward queue-only behavior behind `queueOnlyConnectionSend`, so socket writes can be fully owned by the assigned network worker while the legacy eager-send path remains available as `queueOnlyConnectionSend=false`.
2. Done: measure eager send/write versus queue-only send with `queued`, `eager`, and `workerSend` counters in `/serverstatus` and `/servernetstats` before changing the default.
3. Done: add `ServerMeasurement.DISABLED_QueueOnlySendFanoutComparison`, which runs 8 direct `UniverseConnectionServer` local-socket clients and compares eager versus queue-only transport fan-out with inter-round drains so the measurement does not turn `UniverseServer` inactivity reaping or local socket backlog into a flake. Latest local result on 2026-05-14: eager `sent=128 received=128 queued=128 eager=128 worker=0 wakeups=128 idleTimedWaits=32 elapsedUs=247373`; queue-only `sent=128 received=128 queued=128 eager=0 worker=128 wakeups=128 idleTimedWaits=9 elapsedUs=252290`.
4. Done: draft the narrow core `SocketPoller` API for readable/writable interest, unregister, wake, timed wait, and ready socket handles. The seed wraps the existing portable `Socket::poll` path and is covered by `SocketPollerTest`; it is not wired into `UniverseConnectionServer` yet.
5. Keep the current condition-variable and timed fallback until Windows, Linux, and macOS paths have coverage.

Must-ship acceptance:

- per-connection ordering remains stable
- idle CPU does not regress
- queued-send wakeups still reach the owning worker
- no readiness path becomes required for basic local hosting

## 9. Workstream F: Mechanism-Compatible API Seed

Goal: create the first opt-in compatibility surface for future modern internals without affecting existing mods.

Priority tickets:

1. Define a capability-query shape for mechanism-compatible worlds or scripts.
2. Draft batched read APIs for common query patterns that currently require many immediate calls.
3. Draft deferred write or command-buffer APIs for phase-boundary mutation.
4. Document that legacy exact mode remains the default and that mechanism-compatible APIs promise phase-boundary results, not every undocumented intermediate state.
5. Add diagnostics that show whether a world is running only legacy APIs or has opted into mechanism-compatible behavior.

Must-ship acceptance:

- no existing Lua callback signature changes
- no existing mod silently opts into new semantics
- capability checks are explicit, versioned, and documented
- unsafe worlds can fall back to legacy exact mode

## 10. Workstream G: Mutation Experiment Preflight

Goal: prepare liquid, falling-block, and wiring experiments without enabling mutation workers by default.

Priority tickets:

1. Started: liquid fixed-seed signature preflight now records active-cell count/hash and no-processing-limit region hash under the existing serial update path when `mutationParallelismFixedSeedSignatures=true`. Boundary/interactions/final-flow signatures remain future expansion before any liquid worker can graduate.
2. Started: falling-block fixed-seed signature preflight now records pending-position, processed-position, moved-block, and next-pending signatures under the existing serial update path when `mutationParallelismFixedSeedSignatures=true`. Dependency-region classification remains future expansion before any falling-block worker can graduate.
3. Started: wiring signature preflight now exposes aggregate loaded-network topology and output hashes in addition to the existing clean/dirty network counters. Dirty-component skip or worker evaluation still requires differential output checks and fallback behavior.
4. Started: opt-in mutation shadow workers now run immutable signature jobs for liquid, falling blocks, and wiring when all explicit gates are enabled. This proves worker-pool wiring, diagnostics, and serial comparison behavior; it does not move live subsystem mutation off the world owner thread yet.
5. Region/component boundary stress tests for each subsystem.
6. Done: compatibility-sensitive entity/Lua attribution. `subsystemBaselineMetrics` now reports entity iteration copy/sort/update/metadata-refresh timing, bounded per-entity-type callback counts/timing, and bounded Lua script-context update/readiness timing while preserving the serial execution path.
7. Done: per-subsystem gate reporting. `/serverstatus` and `/worldstats` now render requested mutation experiments, active shadow-worker subsystems, fixed-seed/dependency/mod-visibility blockers, implementation blockers, and worker/check/divergence/fallback counters.

Must-ship acceptance:

- mutation parallelism remains default-disabled
- serial baseline metrics remain default-enabled
- every experiment can fall back to serial on divergence or unsupported mod/API use
- worker jobs consume immutable signatures or snapshots only; live liquid, falling-block, wiring, entity, Lua, or storage state stays on the owner thread
- entity and Lua mutation stay research-only

## 11. Release Tiers

Must ship for `26.5.2a`:

- fixed workload gate and baseline collection notes
- at least two serial hot-path bookkeeping wins with diagnostics
- immutable net-state and `writeNetState(0)` packet-prep gate work
- storage timing and one storage spike-reduction ticket
- modpack smoke and focused regression coverage

Should ship if metrics justify it:

- sector-to-client fan-out index: done for tile, tile-damage, and liquid queue fan-out
- monitoring-region generation counters and owner-thread reuse: initial snapshot reuse slice done
- liquid no-limit membership cache: done with bucketed candidate diagnostics
- packet-prep worker expansion beyond sector prefill
- queue-only network send experiment behind a config or diagnostic flag: done, default-on as `queueOnlyConnectionSend=true`, legacy eager fallback available with `queueOnlyConnectionSend=false`

Stretch only:

- dirty wiring-network tracking enabled by default
- async sector compression
- first mechanism-compatible API prototype exposed to scripts
- disabled-by-default liquid/falling/wiring mutation worker prototypes

Explicitly defer:

- Vulkan backend work
- entity/Lua mutation parallelism
- storage backend replacement
- removal of compatibility fallbacks

## 12. Validation Matrix

Minimum local validation before the release branch is considered healthy:

1. `git diff --check`
2. Build `starbound`, `starbound_server`, and `game_tests` from the VS 2022 developer environment.
3. Run the focused multicore/server/network suite: `game_tests.exe --gtest_filter=MulticorePhaseTest.*:ServerTest.*:UniverseConnections.*:UniverseConnectionServer.*`
4. Run the new or refreshed packet-prep equivalence tests.
5. Run the disabled measurement captures when evaluating performance changes: `ServerMeasurement.DISABLED_QueueOnlySendFanoutComparison`, `ServerMeasurement.DISABLED_GameMechanismSelfWorkloadCapture`, and `ServerMeasurement.DISABLED_SaveDisconnectStorageCapture` with `--gtest_also_run_disabled_tests`.
6. Run storage write-failure, queue-pressure, shutdown-drain, and reload tests.
7. Run fixed workload captures and compare p50/p95/p99 against the `26.5.1a` baseline.
8. Run modpack smoke with default config and with every new optimization explicitly disabled.

Latest local validation on 2026-05-14: `starbound`, `starbound_server`, and `game_tests` built from the VS 2022 developer environment; full `game_tests` passed 62/62; `core_tests.exe --gtest_filter=NetElements.*` passed 17/17; the disabled queue-only and game-mechanism captures passed with `--gtest_also_run_disabled_tests`.

## 13. Go/No-Go Rules

Proceed only if:

- p95/p99 world-thread time improves or remains neutral under fixed workloads
- packet byte-equivalence tests pass for changed replication paths
- save/load tests pass with old and newly written data
- modpack smoke passes
- diagnostics clearly show fallback, divergence, queue pressure, and worker starvation states

Do not ship an optimization enabled by default if:

- it changes Lua-visible ordering
- it changes packet order or packet bytes unexpectedly
- it changes save output without an intentional migration
- it improves average tick time but worsens p99 under representative load
- it requires Vulkan or any graphics backend change

## 14. Exit Criteria

`26.5.2a` is successful if it makes the crowded-world hot thread measurably cooler while preserving legacy behavior. It does not need to solve full single-world multicore scaling.

The release should end with:

- clear before/after workload numbers
- serial fallbacks still available
- no default mutation parallelism for entity, Lua, liquid, falling blocks, or wiring
- stronger immutable packet/storage snapshot boundaries
- enough signatures and diagnostics to decide whether `26.5.3a` can safely attempt the first disabled-by-default subsystem mutation prototype
