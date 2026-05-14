# OpenStarbound 26.5.2a Post-Pass Server Performance Review

Review date: 2026-05-13

This report is a code-review follow-up after the Phase 0-6 multicore work and the extra `26.5.2a` server optimization passes through queue-only network sends. It is meant to turn the current code state into a focused list of performance paths, not to replace the broader roadmap documents.

The important shift since the original performance review is that several earlier findings are no longer open problems. Network workers now own connection lists, pending handshakes have a state-machine path, persistence has bounded async snapshot writes, world entry points are largely mailbox-driven, packet preparation has reusable snapshots and caches, storage has timing counters, and queue-only network sends are implemented with a config fallback.

The main remaining problem is therefore narrower and clearer: one crowded authoritative world can still be dominated by serial world update phases, while save/export and readiness-driven networking still need deeper follow-up before defaults change.

## Scope

Reviewed areas:

- [source/game/StarWorldServer.cpp](../../source/game/StarWorldServer.cpp) and [source/game/StarWorldServer.hpp](../../source/game/StarWorldServer.hpp)
- [source/game/StarWorldStorage.cpp](../../source/game/StarWorldStorage.cpp) and [source/game/StarWorldStorage.hpp](../../source/game/StarWorldStorage.hpp)
- [source/game/StarUniverseServer.cpp](../../source/game/StarUniverseServer.cpp) and [source/game/StarUniverseServer.hpp](../../source/game/StarUniverseServer.hpp)
- [source/game/StarUniverseConnection.cpp](../../source/game/StarUniverseConnection.cpp) and [source/game/StarUniverseConnection.hpp](../../source/game/StarUniverseConnection.hpp)
- [source/game/StarWireProcessor.cpp](../../source/game/StarWireProcessor.cpp)
- [source/base/StarCellularLiquid.hpp](../../source/base/StarCellularLiquid.hpp)
- [source/game/StarFallingBlocksAgent.cpp](../../source/game/StarFallingBlocksAgent.cpp)
- [source/game/StarEntityMap.cpp](../../source/game/StarEntityMap.cpp)
- [source/game/StarSystemWorldServer.cpp](../../source/game/StarSystemWorldServer.cpp)
- [source/core/StarBTreeDatabase.cpp](../../source/core/StarBTreeDatabase.cpp)
- Related status and diagnostic output in [source/game/StarCommandProcessor.cpp](../../source/game/StarCommandProcessor.cpp)

References:

- [doc/architecture/multicore-engineering-plan.md](multicore-engineering-plan.md)
- [doc/architecture/multicore-phase-roadmap.md](multicore-phase-roadmap.md)
- [doc/architecture/performance-threading-review.md](performance-threading-review.md)
- [doc/architecture/server-optimization-roadmap-26.5.2a.md](server-optimization-roadmap-26.5.2a.md)
- [doc/architecture/networking.md](networking.md)

## Current State

The current implementation has useful observability for most of the remaining performance questions:

- `/serverstatus` reports universe-loop timings, world-thread timings, world-update phase timings, network queued/eager/worker send counters, world packet-prep counters, storage timing counters, Phase 6 parallel helper divergence counters, and Phase 6 subsystem baselines.
- `/worldstats` reports per-world command stats, packet-prep stats, storage timings, Phase 6 helper counters, and mutation gate states.
- `/servernetstats` reports per-worker connection ownership, scans, callbacks, wakeups, waits, and queued/eager/worker send counts.
- `WorldStorageTimingStats` separates sync, sync-pass sector visits, `readChunks()` pre-sync sector/time cost, incremental chunk-update pre-sync/export cost, sector copy, entity/tile store, compression, B-tree insert/skip, per-store-surface write/skip/remove attribution, commit, and full snapshot export work.

This means the next pass should not begin with broad instrumentation. It should begin with fixed workload captures using the existing counters, then implement only the tickets whose target workload is visible.

## Fixes That Are No Longer Open Findings

These were real concerns in earlier reviews but should now be treated as implemented baselines:

1. Full-map network worker scans are relieved. `UniverseConnectionServer` keeps per-worker connection ID lists and reports owned, scanned, stale, wakeup, wait, callback, queued-send, eager-send, and worker-send counters.
2. Thread-per-handshake is no longer the default. Pending connections advance through a state machine, while the old blocking accept path remains as a fallback.
3. Persistence is no longer a simple synchronous write path. Versioned JSON snapshots can be written through bounded async persistence, with queue-pressure fallback, shutdown drain, retry/failure counters, and visible timing.
4. Foreign world entry is much more mailbox-shaped than in the original review. The world-command counters now make pending, processed, direct, failed, and wait time visible.
5. Monitoring regions are built into a per-tick `WorldTickSnapshot` and reused by liquid, packet preparation, weather visibility, and active signal regions.
6. Tile, tile-damage, and liquid update fan-out no longer do a full client scan for every tile by default; sector-to-client fan-out counters are exposed.
7. Sector tile-array packet preparation and storage generation planning have default-enabled guarded worker helpers with serial fallback and differential counters.
8. Storage sync/readChunks has enough timing attribution to separate copy, serialization, compression, B-tree insert, skipped insert, commit, and full export costs.
9. Queue-only network send behavior is implemented behind `queueOnlyConnectionSend`; after the 2026-05-13 dummy-client measurement pass it is default-enabled, with legacy eager sends preserved through `queueOnlyConnectionSend=false`.

## Main Conclusions

1. The next required work is fixed workload capture. The code now has enough counters to compare default/eager networking, queue-only networking, save spikes, packet-prep cost, and subsystem baselines. Without those captures, it will be too easy to optimize a visible code pattern that is not the current p99 cost.
2. Storage spikes are now measurable, and the first full-export reduction is implemented for ship/client-context paths. Byte-identical B-tree insert skipping avoids redundant leaf rewrites, dirty-sector filtering remains default-off, and `readChunkUpdate()` avoids repeated full B-tree snapshots when the caller already has a client-context baseline.
3. Network readiness is the next structural networking step. Queue-only sends now remove caller-thread writes by default, but workers still rely on a 1 ms timed fallback and synchronous nonblocking `writeData()` / `readData()` polling under the connection mutex.
4. First-observation entity net-state work should not be naively cached. `writeNetState(0)` has byte-equivalence tests, but it also advances entity net versions. The safe path is to separate byte generation from owner-thread version advancement, then test a cache or worker path behind differential checks.
5. The remaining single-world hot path is compatibility-sensitive: entity update, Lua update, wiring, liquid, falling blocks, damage, storage tick/generation, removal, and packet preparation still run in one ordered owner-thread sequence.

## Ranked Next Paths

| Rank | Path | Primary workload | Expected payoff | Risk | Recommendation |
| --- | --- | --- | --- | --- | --- |
| 1 | Fixed workload capture and comparison pack | all | enables correct decisions | low | do first |
| 2 | Queue-only send A/B soak and fallback validation | high fan-out, many clients | medium for caller-thread latency and lock scope | low to medium | keep measuring with `queueOnlyConnectionSend=false/true` |
| 3 | Dirty-sector sync filtering | save-heavy loaded worlds | medium to high p95/p99 sync reduction | medium | implement after dirty-mark map design |
| 4 | Reduce full `readChunks()` exports for ship/disconnect saves | save/disconnect spike | high p99 reduction | high | first incremental chunk-update path implemented; expand fixtures and fixed workloads before broadening |
| 5 | Socket readiness abstraction | idle/many connections, latency | high idle CPU and latency improvement | medium to high | design API before implementation |
| 6 | Liquid no-limit region cache rebuild skip | liquid-heavy static visibility | low to medium | low | safe bookkeeping ticket |
| 7 | Dirty wiring-network tracker | wiring-heavy bases | high for stable wiring | medium to high | guarded prototype with full-scan fallback |
| 8 | Entity/Lua per-type and per-context profiling | modded crowded worlds | diagnostic, guides later work | low | first bounded counters added; use captures before mutation experiments |
| 9 | System-world delta/cache review | many system-world clients | low to medium | low to medium | only after workload shows cost |
| 10 | Separate first net-state byte generation from version advancement | crowded hubs, first observation | medium | medium | prerequisite for workerized entity packet prep |

## Detailed Findings

### 1. Fixed Workload Capture Is The Current Gate

The roadmap correctly marks fixed workload capture as next. The code now has the necessary command output to gather p50/p95/p99 and work counters without adding another broad diagnostics pass.

Use these command surfaces during each workload:

- `/serverstatus` for universe timings, world timings, packet-prep counters, storage timing, Phase 6 helper counters, network queued/eager/worker sends, and persistence state.
- `/worldstats` for per-world packet-prep, storage, command, and Phase 6 baselines.
- `/servernetstats` for per-worker ownership, queued/eager/worker sends, callbacks, scans, wakeups, waits, and idle waits.

Recommended fixed workload set:

1. crowded hub with many clients entering the same dense settlement
2. high fan-out tile/liquid mutation area
3. liquid-heavy region with mostly static monitoring regions
4. wiring-heavy base with many stable disconnected networks
5. generation-heavy exploration with many queued sectors
6. save/disconnect spike with multiple ship worlds
7. login burst with slow and normal clients
8. modpack smoke with default config and with new optimization flags disabled

Go/no-go rule: no optimization should become default unless p95/p99 improves or remains neutral in the relevant fixed workload and no compatibility counters regress.

### 2. Queue-Only Networking Is Default-On After Initial A/B Data

`UniverseConnectionServer::sendPackets()` now records queued send batches and packets. In legacy mode it still sends and writes immediately from the caller thread, then wakes the owning worker. In queue-only mode it only appends to the send queue and wakes the owning worker. The worker loop then owns packet framing and socket writes for queued packets.

The first automated A/B run now exists as `ServerMeasurement.DISABLED_QueueOnlySendFanoutComparison`. On 2026-05-13 it used 8 in-process dummy clients and 128 direct server fan-out payloads:

- eager fallback: `sent=128 received=193 queuedPackets=140 eagerPackets=140 workerPackets=0 wakeups=138 idleTimedWaits=2 elapsedUs=7576`
- queue-only default: `sent=128 received=152 queuedPackets=128 eagerPackets=0 workerPackets=129 wakeups=128 idleTimedWaits=0 elapsedUs=411`

This is a synthetic local workload, not the full fixed-workload gate, but it confirms that queue-only mode moves server fan-out writes off the caller thread and preserves packet delivery under dummy clients. The default is now `queueOnlyConnectionSend=true`; keep the eager path available for rollback and comparison:

- `queueOnlyConnectionSend=false`: legacy eager-write fallback behavior
- `queueOnlyConnectionSend=true`: worker-owned socket write experiment

Measure:

- packet order tests remain green
- `/servernetstats` eager send counters fall to zero in queue-only mode
- worker send counters rise as expected
- p50/p95/p99 packet latency and world/universe timings stay neutral or improve
- idle timed waits and wakeups do not regress enough to matter

If fixed high-fan-out or normal-play workloads show higher p99 latency, set `queueOnlyConnectionSend=false` as the rollback and keep readiness-driven wakeups ahead of any further networking default changes.

### 3. Readiness-Driven Networking Remains The Structural Network Step

The current worker loop still scans the worker-owned connections, calls nonblocking `writeData()` / `readData()` under each connection mutex, and falls back to `ConditionVariable::wait(..., PacketSocketPollSleep)` when no data moved. This is much better than scanning all connections from every worker, but it is still sleep-poll networking.

The next structural ticket is a narrow `SocketPoller` API:

- register readable and writable interest
- unregister connection/socket handles
- wake from another thread
- timed wait with fallback timeout
- return ready handles without exposing platform details

Implementation order should be conservative:

1. define the platform-neutral API and a fallback implementation
2. add test harness coverage for wake, unregister, close, timeout, and writable-interest behavior
3. integrate established `UniverseConnectionServer` workers
4. only then consider moving pending handshakes onto the same readiness path

Avoid jumping straight to IOCP/epoll/kqueue behavior in the game layer. The game code should depend on readiness semantics, not platform event details.

### 4. Pending Handshakes Are Bounded, But Still Advanced By The Universe Loop

The state-machine path is a major improvement over one thread per pending handshake. However, `processPendingConnections()` advances all pending connections under `m_mainLock`, and `advancePendingConnection()` performs nonblocking receive/send, validation, challenge, reject/flush, and finalize steps from the universe loop.

This is acceptable for `26.5.2a` because it bounds thread count and exposes state counters. It is not the final architecture. After `SocketPoller` exists, pending handshakes should be driven by readiness and should wake the universe loop only at decision points such as accepted, rejected, finalized, or timed out.

Next measurement:

- login burst workload with default state machine
- track `PendingHandshakes` timing in `/serverstatus`
- compare slow clients versus normal clients
- check whether universe p99 moves during the burst

### 5. Storage Sync Still Visits Every Loaded Sector

`WorldStorage::sync()` still loops every `m_sectorMetadata` entry and calls `syncSector()` before commit. Pass 6's byte-identical insert filtering prevents redundant B-tree leaf rewrites, but it does not skip sector copy, entity/tile serialization, or compression work.

The next storage implementation ticket is dirty-sector filtering, but it needs a careful dirty model. At minimum, dirty marks should distinguish:

- tile data changed
- entity store changed
- entity moved between sectors
- unique entity index changed
- sector-unique store changed
- sector unloaded or expired
- generation wrote or altered a sector

Acceptance tests:

- sync/reload after tile-only changes
- sync/reload after entity-only changes
- entity movement across sectors
- unique entity lookup after clean tile sector but dirty unique index
- unload path still persists required state
- byte-identical unchanged sync still reports skipped inserts when dirty filtering is disabled

Diagnostics now available for the dirty-sector ticket:

- dirty reason counters: `dirty=marked/tile/entity/unique/generation/unload`
- ordinary sync visit attribution: `dirtySync=marked/unmarked/skipped`
- full snapshot pre-sync visit attribution: `dirtySnapshot=marked/unmarked`

Diagnostics still needed before considering the filter default-safe:

- dirty sync fallbacks
- reload verification count in tests

### 6. `readChunks()` Full Exports Are The Save/Disconnect Spike To Watch

`WorldStorage::readChunks()` syncs active sectors and then iterates the full database with `m_db.forAll()` to build a complete `WorldChunks` snapshot. That remains the fallback/reference path. The ship/client-context hot paths now use `WorldStorage::readChunkUpdate(oldChunks)` when the server already has the client's chunk baseline: it performs the same snapshot-style pre-sync, emits only changed/new chunks and removals, and lets `ServerClientContext::applyShipChunksUpdate()` update both server-held baseline state and the pending client update stream.

The disabled save/disconnect-style storage capture now exists as `ServerMeasurement.DISABLED_SaveDisconnectStorageCapture`. On 2026-05-14 it produced `sync=5/100/300`, `btree=46/570/4300/41629/516`, `storeTypes=2/14/0:24/276/0:20/280/0:0/0/0:0/0/0`, `dirty=24/24/0/0/20/0`, `dirtySync=24/76/0`, `dirtySnapshot=0/200`, `snapshot=1/41/3176/22`, `snapshotSync=20/8774`, `chunkUpdate=9/4/0/648/271`, and `chunkUpdateSync=180/76696`. That confirms the release path has one baseline full export and nine incremental exports carrying only four changed chunks in this fixture, while the owner-thread pre-sync remains explicit and measurable. The dirty-sector filter now exists behind `storageDirtySectorFiltering=false` by default; the third `dirtySync` value reports clean sectors skipped by that opt-in path.

This remains a storage p99 risk to validate in fixed workloads:

- it is proportional to connected clients with ship worlds
- it still pre-syncs active ship sectors before each export
- it still allocates update collections and compares against the caller baseline
- it needs real multi-client ship/disconnect fixed workload data beyond the direct `WorldServer` capture

Implemented and remaining paths:

1. Done: add incremental chunk deltas to the client-context ship snapshot path.
2. Expand real ship-world tile/entity/unique/unload/reload fixtures and fixed workload captures.
3. Queue immutable ship-world snapshot exports from the world owner thread and write them through the persistence worker path.
4. Keep full `readChunks()` as the serial fallback and byte-compare incremental versus full exports during tests.

This path has higher compatibility risk than dirty-sector sync because the exported chunk set becomes part of durable client context state. The focused gates now cover current changed/added/removed/no-op client-context update semantics and a real dirty world update versus full snapshot comparison. Do not broaden beyond the existing ship/client-context callsites until fixtures cover entity edits, unique entities, unload, and old-save reload.

### 7. B-tree Tuning Should Be Measurement-Led

`BTreeDatabase` remains the correct durability shape for world storage; it is not a simple in-memory hash map problem. The review did not find evidence that replacing the backend is the next best move.

More plausible next work:

- report B-tree cache hit/miss or lookup/insert/commit cadence counters
- measure leaf count and commit time under save-heavy workloads
- compare commit cadence under autosave and shutdown drain
- only then tune cache size or commit timing

Do not consider backend replacement until dirty-sector filtering, full-export reduction, and commit/cache profiling have been exhausted.

### 8. First-Observation Entity Net-State Needs An API Split, Not A Simple Cache

Packet preparation already caches sector tile-array packets, entity create-store data, and later delta net-state results. The first-observation path still calls `writeNetState(0, netRules)` for each client that newly observes an entity.

This looks like an obvious cache target, but it is not safe as a direct cache because prior Phase 5/6 tests showed an important detail: first writes can be byte-equivalent while still advancing top-level entity net versions. Caching only the returned bytes would skip owner-thread version advancement unless the code explicitly separates those two responsibilities.

Safe next design:

1. introduce an API that can generate first-observation bytes from immutable state without advancing versions
2. keep owner-thread version advancement explicit and per client
3. compare generated bytes to existing `writeNetState(0)` output in focused tests
4. only then allow first-observation byte generation to be cached or workerized

Acceptance tests:

- byte-equivalence for controlled entity types
- version advancement remains identical to the current path
- separate net compatibility rules do not share bytes incorrectly
- packet order and create/update packet contents remain unchanged

### 9. System World Replication Has A Smaller But Similar Cache Opportunity

`SystemWorldServer::queueUpdatePackets()` still loops each client, then each ship and object, and calls `writeNetState()` with that client's version map. This is not as likely to dominate as a crowded planet world, but it can matter for system-world-heavy sessions.

Treat this as a medium-priority cache/equivalence ticket:

- measure system-world update time first
- cache only by safe keys: object/ship UUID, prior version, and compatibility rules if they become relevant
- preserve per-client version advancement
- add byte-equivalence tests before enabling by default

### 10. Wiring Is The Best Subsystem Bookkeeping Candidate

`WireProcessor::process()` still starts by scanning live entities, collecting every `WireEntity`, recursively loading networks, and evaluating every working wire entity. For stable wiring builds this can be mostly repeated work.

Dirty wiring-network tracking is probably the highest-value subsystem ticket before any mutation parallelism:

- track topology changes from object placement/removal and wire connection changes
- track output-state changes that can affect downstream evaluation
- evaluate dirty networks and their dependent networks
- keep full-scan evaluation as fallback
- add a differential mode that compares dirty-network results against the full scan for controlled test worlds

Risks:

- missed dirty marks cause stale wiring output
- evaluation order can matter for feedback and cross-network dependencies
- network loading currently has recursive behavior, so tracker design must avoid hiding recursion bugs

Tests:

- static wiring network skips after initial evaluation
- output toggle marks the right network dirty
- object add/remove marks topology dirty
- connected multi-sector network reloads correctly
- dirty path and full path produce matching output states in a controlled wiring fixture

### 11. Liquid Has A Low-Risk Cache-Rebuild Ticket And A High-Risk Mutation Ticket

The liquid no-limit membership cache is already a good bookkeeping win: bucketed candidates replace full region scans. The remaining low-risk issue is that `setNoProcessingLimitRegions()` rebuilds the bucket cache every time it is called, even if the region list is unchanged from the previous liquid update.

Low-risk ticket:

- skip no-limit region cache rebuild when the `List<RectI>` is unchanged
- add `liquidCacheRebuildSkips` or equivalent counter
- test repeated identical monitoring regions produce same liquid state and fewer rebuilds

Higher-risk future ticket:

- dirty active-cell processing or region-based liquid mutation
- requires deterministic fixed-seed signatures, boundary signatures, and full serial-versus-experimental state comparison

Do the cache-rebuild ticket before any liquid mutation experiment.

### 12. Falling Blocks Need Better Spike Diagnostics Before Behavior Changes

`FallingBlocksAgent::update()` takes the pending set, shuffles and Y-sorts it, processes positions, and can add upward neighbors back into the same update when blocks move. This can spike on unstable columns, but changing the algorithm risks physics and world-state differences.

Next safe step:

- add iteration count, max processing set size, processed positions, and moved block counters
- expose p99 falling-block phase time through the existing world-update timings and subsystem counters
- build a pathological fixture with a tall unstable column

Only after measurements should deferred-to-next-tick or capped propagation be considered, and any behavior change should remain opt-in or experimental.

### 13. Entity And Lua Remain The Single-World Wall

`WorldServer::update()` still executes entity updates and Lua script contexts serially on the world owner thread. `EntityMap::updateAllEntities()` copies entity entries, sorts them when a sort callback is supplied, calls the update callback, and refreshes spatial/unique metadata for each entity.

This is the unavoidable compatibility wall for one crowded world. The next useful work is not parallel entity or Lua mutation. It is attribution:

- split entity phase timing into pointer-copy, sort, entity callback, and metadata refresh when diagnostics are enabled
- add per-entity-type update cost sampling or bounded aggregation
- add per-script-context update timing
- correlate those costs with modpack smoke and crowded-world p99

The latest safe progression adds immutable entity/Lua signature shadow-worker jobs behind the same explicit gates as other mutation experiments: subsystem request, fixed-seed signatures, dependency analysis, mod-visibility contract, and worker threads. These jobs hash scalar post-serial work signatures and compare them with the owner-thread serial signature; they do not run live entity or Lua mutation off-thread. Live entity grouping and Lua execution parallelism still require opt-in mechanism-compatible APIs and staged visibility rules.

### 14. Universe Loop Is Still Fixed-Sleep Scheduled

`UniverseServer::run()` executes the timed phase list, records loop timing, then sleeps for `mainWakeupInterval`. This remains simple and compatible, but it means pending work is not fully event-driven.

Do not tackle this before network readiness and fixed workload captures. A wake-on-work universe loop touches many ownership paths and is more likely to introduce ordering surprises than the currently ranked tickets.

## Recommended Implementation Order

### Pass 8: Measurement And Default Decisions

1. Write a fixed workload capture note for the eight workloads listed above.
2. Capture `/serverstatus`, `/worldstats`, and `/servernetstats` for default config.
3. Repeat network-heavy captures with `queueOnlyConnectionSend=false` as rollback comparison.
4. Keep queue-only default only if fixed workloads stay neutral or improve.
5. Run `ServerMeasurement.DISABLED_SaveDisconnectStorageCapture` to keep save/disconnect-style `sync`, `dirty`, `dirtySync=marked/unmarked/skipped`, `dirtySnapshot`, `snapshot`, and `snapshotSync` counters in the comparison pack.
6. Record the current top p99 world-update phases and storage snapshot/export counters.

### Pass 9: Low-Risk Bookkeeping Wins

1. Skip liquid no-limit cache rebuilds when monitoring regions are unchanged.
2. Expand dirty-sector sync filtering coverage with reload verification, entity/unique/unload fixtures, and fallback counters before considering a default-on decision.
3. Add B-tree cache/cadence counters if save-heavy workload points there.
4. Use the new per-entity-type and per-Lua-context attribution to decide whether deeper mod-heavy profiling is needed.

### Pass 10: Spike Reduction And Readiness Design

1. Expand incremental ship chunk-update equivalence tests from the current dirty tile/entity fixture into unique/unload/reload and multi-client ship cases.
2. Draft and test `SocketPoller` fallback API.
3. Prototype dirty wiring networks with full-scan differential validation.
4. Start first-net-state byte-generation API split for future packet-prep worker expansion.

### Later Research

1. Move pending handshakes onto readiness-driven workers.
2. Add platform socket poller implementations.
3. Prototype live subsystem mutation only after fixed-seed, dependency, mod-visibility, and staged API gates exist.
4. Add mechanism-compatible APIs for batched reads and phase-boundary writes.

## Validation Matrix For The Next Review

Before accepting another optimization as default-enabled:

1. `git diff --check`
2. Build `game_tests`, `starbound_server`, and `starbound`
3. Run `game_tests.exe -bootconfig ..\scripts\windows\sbinit.config --gtest_filter=MulticorePhaseTest.*:ServerTest.*:UniverseConnections.*:UniverseConnectionServer.*`
4. Run any focused new regression suite for the ticket
5. Run relevant disabled measurement captures, including `ServerMeasurement.DISABLED_SaveDisconnectStorageCapture` for storage/export changes
6. Capture fixed workload before/after p50/p95/p99
7. Capture `/serverstatus`, `/worldstats`, and `/servernetstats`
8. Run modpack smoke with the optimization enabled and disabled
9. Confirm no packet, save, protocol, or Lua-visible behavior changes unless explicitly versioned and documented

## Code Review Watchlist

Keep these points visible during future implementation reviews:

- Do not cache first-observation `writeNetState(0)` bytes unless version advancement is explicitly preserved.
- Do not skip storage writes based only on tile dirtiness; entity stores, unique indexes, and sector-unique stores have separate persistence surfaces.
- Do not change liquid or falling-block processing order in a default path.
- Do not remove the `queueOnlyConnectionSend=false` eager fallback until fixed-workload latency and p99 measurements are neutral or better.
- Do not replace `BTreeDatabase` as a shortcut; optimize storage access patterns and commit/cache behavior first.
- Do not remove serial fallbacks or differential checks from Phase 6 helpers while mutation experiments remain research-only.

## Bottom Line

The codebase is now in a better state for performance work because it can answer more questions with counters instead of hunches. The next useful move is not a broad parallelism push. It is to gather fixed workload numbers, make a data-backed call on queue-only sends, then reduce the measured save and sync spikes with dirty-sector filtering and safer ship snapshot exports. After that, wiring dirty tracking and the first-net-state API split are the most promising bridges toward deeper single-world gains.
