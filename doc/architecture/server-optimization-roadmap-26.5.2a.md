# OpenStarbound 26.5.2a Server Optimization Roadmap

This roadmap is the first concrete post-Phase-6 server-performance slice after `26.5.1a` in the `Ethereal Drake` release line. It targets `26.5.2a` as an alpha preview in that same release line, focused on reducing the remaining crowded-world hot-thread problem without changing legacy gameplay, Lua, save, or packet behavior by default.

Vulkan and broader graphics backend modernization are intentionally out of scope for this release slice. The current bottleneck is server simulation and replication, not graphics API overhead.

## 1. Release Thesis

`26.5.2a` should reduce p95/p99 tick cost for one crowded authoritative world by removing avoidable serial bookkeeping and expanding snapshot-safe worker work. It should not try to make entity or Lua mutation parallel by default.

The expected outcome is a visibly cooler hot world thread under packet-heavy, liquid-heavy, wiring-heavy, generation-heavy, and save-heavy workloads. The release should be judged by measured workload deltas, not by thread count.

## Implementation Progress

This roadmap is now being tracked as an implementation checklist. Status values are:

- Done: implemented with focused regression coverage and local validation.
- Active: the current implementation slice.
- Next: planned for this release after the active slice.
- Pending: scoped but not started.

Current checklist:

1. Done: sector-to-client fan-out index for tile, tile-damage, and liquid update queueing, with `sectorFanout` diagnostics and focused coverage.
2. Done: first monitoring-region snapshot reuse slice, with precomputed player-active signal regions and `regions=builds/rects/splits/reuses` diagnostics for liquid and packet-prep consumers.
3. Done: liquid no-limit membership cache using bucketed region candidates, with `liquidCache=builds/regions/buckets/lookups/candidates/hits` diagnostics and focused engine coverage.
4. Done: per-entity serialization counters for packet-prep cost attribution, with `entitySerialize=type=store:calls/bytes first:calls/bytes delta:calls/bytes` diagnostics and focused ItemDrop coverage.
5. Done: immutable entity net-state input research and deeper `writeNetState(0)` equivalence tests, including byte-equivalent but version-advancing first writes.
6. Done: storage timing counters for world sync/readChunks phases, with byte-identical B-tree insert filtering to reduce repeated save spikes.
7. Done: queue-only network send experiment behind a default-off config flag, with queued/eager/worker send counters and ordering coverage.
8. Next: fixed workload captures, release validation, and metric comparison against the `26.5.1a` baseline.

## 2. Current Bottleneck Statement

The remaining jam is a hot authoritative world thread:

- one `WorldServerThread` still owns one `WorldServer::update()` sequence
- entity updates, Lua/script contexts, damage, wiring, liquid, falling blocks, storage tick/generation, entity removal, and packet ordering decisions remain serial
- Phase 0-6 has relieved surrounding pressure, especially handshakes, async persistence, connection-worker ownership, packet-sector prefill, storage-generation planning, and diagnostics
- Phase 6 mutation-parallelism flags are correctly blocked until fixed-seed signatures, dependency analysis, mod-visibility contracts, and differential checks exist

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
4. Re-enable or document the `world_benchmark` utility target only if it can run without disturbing normal builds; otherwise keep it as a manual profiling harness.
5. Store benchmark world setup notes with enough detail that future releases can rerun the same workload.

Must-ship acceptance:

- every optimization merged for this release identifies the workload and metric it is meant to improve
- p95/p99 world-thread and `WorldServer::update()` numbers are available before and after each substantial change
- modpack smoke remains part of the release gate

## 5. Workstream B: Serial Hot-Path Bookkeeping Wins

Goal: reduce the hot world thread while preserving legacy exact behavior.

Priority tickets:

1. Done: sector-to-client fan-out index for tile, tile-damage, and liquid update queueing. Replaced repeated per-tile full-client scans with sector-local subscriber lookup while preserving per-client packet order. Diagnostics: `sectorFanout=lookups/recipients/misses` in `LogMap`, `/serverstatus`, and `/worldstats`.
2. Done: first monitoring-region generation/reuse slice. `WorldTickSnapshot` now carries precomputed player-active signal regions, counts downstream monitoring-region reuse, and exposes `regions=builds/rects/splits/reuses` through `LogMap`, `/serverstatus`, and `/worldstats`. Further liquid-cell membership pruning belongs to the liquid no-limit cache ticket.
3. Done: liquid no-limit membership cache. Replaced the per-active-cell full `m_noProcessingLimitRegions` scan under a processing limit with bucketed region candidates that are still confirmed with exact `RectI::contains` checks. Diagnostics: `liquidCache=builds/regions/buckets/lookups/candidates/hits` in Phase 6 subsystem output, `/serverstatus`, and `/worldstats`.
4. Pending: dirty wiring-network prototype. Keep the full scan as fallback, but start tracking dirty topology/output state so unchanged disconnected networks can be skipped or measured.
5. Done: per-entity serialization counters. Packet preparation now attributes create-store serialization, first-observation `writeNetState(0)`, and later delta `writeNetState(version)` calls/bytes by `EntityType`. Diagnostics: `entitySerialize=type=store:calls/bytes first:calls/bytes delta:calls/bytes` in `/serverstatus` and `/worldstats`.

Must-ship acceptance:

- default behavior remains packet/save/Lua compatible
- each cache or index has invalidation coverage and a serial fallback
- diagnostics expose enough hit/miss/work counts to decide whether the optimization is worth keeping

## 6. Workstream C: Packet Preparation Expansion

Goal: move more replication prep off the owner thread only after immutable inputs are proven.

Priority tickets:

1. Done: finish immutable net-state input research for first-observation and later-delta paths. `writeNetState(0)` full writes are byte-equivalent for unchanged state but still advance the top-level net version, so create-store bytes can be shared from `EntityCreateSnapshot`, while first-observation net-state writes must remain per-client owner-thread work until byte generation is separated from version advancement.
2. Done: add deeper `writeNetState(0)` equivalence tests, including cases where first writes advance entity net versions.
3. Next: separate byte generation from owner-thread version advancement where possible, or explicitly document why a path must remain serial.
4. Extend workerized packet preparation from sector tile-array prefill toward entity create/update payload preparation only when the worker consumes immutable inputs and the owner thread performs the visible merge.
5. Keep default-enabled serial-versus-worker differential checks and divergence counters for every expanded packet-prep worker path.

Must-ship acceptance:

- no packet ordering changes
- no entity net-version advancement happens on worker threads unless the input contract is immutable and tested
- divergence uses the serial result and increments visible counters
- controlled entity create/update packet payload tests cover the expanded path

## 7. Workstream D: Storage And Persistence Depth

Goal: reduce save, disconnect, and sync spikes without weakening durability.

Priority tickets:

1. Pending: dirty-sector write filtering for world storage sync, with careful dirty marks for generation, tile edits, entity movement, entity persistence changes, unload, and unique-index updates.
2. Reduce avoidable `readChunks()` full exports in ship/disconnect paths where incremental chunk updates are available.
3. Done: add storage timing counters for sector copy, entity store, tile store, compression, B-tree insert, commit, and full snapshot export. Diagnostics: `storageTiming=sync:... entity:... tile:... copy:... compress:... btree:... commit:... snapshot:...` in `/serverstatus`, `/worldstats`, and `LogMap`.
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

1. Done: move `UniverseConnectionServer::sendPackets()` toward queue-only behavior behind `queueOnlyConnectionSend`, so socket writes can be fully owned by the assigned network worker while the legacy eager-send path remains the default fallback.
2. Done: measure eager send/write versus queue-only send with `queued`, `eager`, and `workerSend` counters in `/serverstatus` and `/servernetstats` before changing the default.
3. Draft the narrow `SocketPoller` API for readable/writable interest, unregister, wake, timed wait, and ready connection handles.
4. Keep the current condition-variable and timed fallback until Windows, Linux, and macOS paths have coverage.

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

1. Liquid fixed-seed state signatures for active cells, boundary regions, interactions, and final flow cells.
2. Falling-block dependency-region signatures for pending positions, processed positions, moved blocks, and newly added positions.
3. Wiring component signatures for loaded networks, evaluated entities, output states, and topology changes.
4. Region/component boundary stress tests for each subsystem.
5. Per-subsystem gate reporting that explains why a requested mutation worker is blocked.

Must-ship acceptance:

- mutation parallelism remains default-disabled
- serial baseline metrics remain default-enabled
- every experiment can fall back to serial on divergence or unsupported mod/API use
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
- queue-only network send experiment behind a config or diagnostic flag: done, default-off as `queueOnlyConnectionSend`

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
5. Run storage write-failure, queue-pressure, shutdown-drain, and reload tests.
6. Run fixed workload captures and compare p50/p95/p99 against the `26.5.1a` baseline.
7. Run modpack smoke with default config and with every new optimization explicitly disabled.

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
