# OpenStarbound Multicore Engineering Plan

This document turns the performance and threading review into an implementation-oriented plan.

The goal is to increase practical multicore utilization on dedicated servers while preserving current gameplay behavior, save compatibility, packet compatibility, Lua callback surfaces, and mod expectations as much as possible.

This is not a promise that one crowded world can immediately use every core. The current architecture makes that difficult because the authoritative world tick is mostly serial. The plan therefore improves multicore use in layers: first by reducing scheduler waste and offloading non-simulation work, then by tightening world ownership, then by adding carefully bounded parallel work around the serial simulation lane.

## 1. Design Goals

Primary goals:

- reduce the single hot-core symptom seen on Linux dedicated servers
- increase throughput across many active worlds and clients
- reduce tick spikes caused by network polling, handshake blocking, and persistence work
- build a safer foundation for future world-internal parallelism
- preserve mod-visible behavior by default

Non-goals for the first implementation wave:

- changing packet formats
- changing save formats
- changing Lua callback signatures
- changing entity or world script update order
- making one single active world fully parallel in one pass

## 2. Current Runtime Diagnosis

The server already creates several thread groups:

- `UniverseServer` runs its own orchestration thread
- `UniverseConnectionServer` creates network workers
- every loaded `WorldServerThread` owns one world update thread
- every loaded `SystemWorldServerThread` owns one system-world update thread
- `UniverseServerWorkerPool` handles world creation and selected request work

The production symptom of one core doing most of the work is still plausible because the hottest gameplay lane is usually one `WorldServer::update()` for one crowded active world.

That update executes entity logic, world Lua, damage, wiring, weather, liquid, falling blocks, world storage work, and packet preparation in one ordered sequence. More helper threads do not help much when most players and mod work are concentrated inside that one world tick.

## 3. Compatibility Rules

These rules should be treated as hard constraints until profiling proves a deeper break is worth it.

### 3.1 Keep one authoritative mutation owner per world

`WorldServer` state should be mutated by its owning world thread. Other threads should enqueue commands, receive promises, or consume snapshots.

### 3.2 Keep mod-visible world order serial by default

Do not parallelize these by default in the first waves:

- entity update order
- world script context update order
- entity message handling order
- immediate tile, liquid, and object mutation that scripts can observe in the same tick

### 3.3 Preserve per-client packet ordering

Network changes may shard by connection, but must keep packet order stable for each connection.

### 3.4 Prefer snapshot reads over live cross-thread reads

Persistence, packet encoding, metrics, and administrative views should consume stable snapshots instead of entering live world state from foreign threads.

## 4. Phase 0: Baseline Measurement

Purpose: prove where the live server spends time before changing behavior.

### 4.1 Add timing scopes

Add lightweight timers around `UniverseServer::run()` phases:

- `updateLua`
- `processUniverseFlags`
- `removeTimedBan`
- `sendPendingChat`
- `updateTeams`
- `updateShips`
- `sendClockUpdates`
- `kickErroredPlayers`
- `reapConnections`
- `processPlanetTypeChanges`
- `warpPlayers`
- `flyShips`
- `arriveShips`
- `processChat`
- `sendClientContextUpdates`
- `respondToCelestialRequests`
- `clearBrokenWorlds`
- `handleWorldMessages`
- `shutdownInactiveWorlds`
- `doTriggeredStorage`

Add timing scopes around `WorldServerThread::update()`:

- incoming packet handling
- `WorldServer::update()` total time
- message handling
- outgoing packet collection
- update action callback

Add timing scopes inside `WorldServer::update()`:

- entity update pass
- world script context update
- damage update
- wiring update
- sky and weather update
- liquid update
- falling blocks update
- world storage tick
- world storage generation
- packet update preparation

### 4.2 Add network worker counters

Track per network worker:

- wakeups per second
- idle wakeups per second
- number of assigned connections
- number of scanned connections
- packets processed
- bytes read and written where available
- callback execution time

The current worker loop copies the full connection map and filters by `workerIndex`, so `scanned connections` versus `assigned connections` is an important metric.

### 4.3 Add world ownership contention counters

For `WorldServerThread` and `SystemWorldServerThread`, record:

- time spent waiting to enter world mutexes
- direct external calls per second
- queued packet count per world
- outgoing packet batch size per world

### 4.4 Use existing benchmark support

The existing `source/utility/world_benchmark.cpp` already runs a `WorldServer` for a fixed number of steps and reports simulation throughput. Extend it or run it alongside new timing scopes for repeatable local comparisons.

### 4.5 Linux production tools

Use process and thread-level Linux tools during a representative live load:

- `pidstat -t -p <pid> 1`
- `top -H -p <pid>`
- `perf top -g -p <pid>`
- `perf record -g -p <pid> -- sleep 60`

Expected output from this phase:

- a per-thread CPU profile
- a per-world tick profile
- a per-universe-loop profile
- baseline TPS or tick-duration percentiles
- baseline network idle wakeups and packet latency

Performance gain estimate: none directly. This phase prevents false optimization and gives the team confidence in later numbers.

## 5. Phase 1: Network Worker Sharding And Wakeups

Purpose: reduce scheduler waste and improve packet latency without touching gameplay semantics.

### 5.1 Current issue

`UniverseConnectionServer` starts worker threads. When `networkWorkerThreads` is `0`, it chooses `max(2, hardware_concurrency / 4)` workers.

Each worker currently:

- copies `m_connections.pairs()`
- iterates over the copied full connection set
- filters by `workerIndex`
- sleeps for `PacketSocketPollSleep` when no data was transmitted

This causes unnecessary work on mostly idle servers and scales poorly as connection count rises.

### 5.2 Engineering changes

Introduce per-worker connection ownership:

- add `NetworkWorkerState`
- store each worker's assigned connection IDs or connection pointers directly
- on `addConnection`, insert into exactly one worker's owned collection
- on `removeConnection`, remove from that worker's owned collection
- avoid copying the full global connection map in every worker loop

Introduce wakeup-driven worker loops:

- wake a worker when packets are queued to one of its connections
- wake a worker when a connection is added or removed
- wake a worker on socket readiness once a poller abstraction exists
- keep a fallback timed wakeup for portability and shutdown handling

### 5.3 Socket readiness plan

A cross-platform implementation should add a small abstraction in `core` rather than hardcoding Linux `epoll` directly into game code.

Suggested shape:

- `SocketPoller` interface in `source/core/`
- Linux implementation backed by `epoll`
- Windows implementation backed by `WSAPoll`, IOCP later, or a conservative select-like fallback
- macOS implementation backed by `kqueue` or poll/select fallback

The first version can keep the API narrow:

- register socket readable/writable interest
- unregister socket
- wake from another thread
- wait with timeout
- return connection handles ready for service

### 5.4 Compatibility impact

Low, if per-connection packet ordering is preserved.

### 5.5 Test plan

- extend `universe_connection_test.cpp` with multi-connection ordering checks
- add tests for connection add/remove while workers are active
- add tests for queued sends waking the correct worker
- stress with many idle connections and a small number of active connections

### 5.6 Performance estimate

Expected improvement:

- idle network CPU: 50-90% lower
- low-load packet latency: 1-5 ms better if polling was the limiting factor
- high-connection bookkeeping overhead: 20-60% lower in the network worker layer
- total server TPS for one crowded world: usually 0-5% better

This phase mainly reduces waste and latency. It will not fix one crowded world by itself.

## 6. Phase 2: Handshake State Machine

Purpose: remove one-thread-per-pending-handshake behavior and reduce scheduler pressure during login bursts or slow clients.

### 6.1 Current issue

`UniverseServer` creates a `Thread::invoke("UniverseServer::acceptConnection", ...)` task for each pending TCP handshake. The handshake performs blocking `receiveAny(clientWaitLimit)` and `sendAll(clientWaitLimit)` calls.

The count is bounded by `maxPendingConnections`, but every slow handshake still consumes a thread slot.

### 6.2 Engineering changes

Create a `PendingConnection` state machine:

- `AwaitProtocolRequest`
- `SendProtocolResponse`
- `AwaitClientConnect`
- `AwaitPasswordChallengeResponse`
- `FinalizeClient`
- `RejectAndFlush`
- `Dead`

Drive pending handshakes from the network worker or a small dedicated handshake executor:

- no dedicated thread per connection
- deadlines stored per state
- existing compatibility checks preserved
- existing packet types preserved

Keep final client registration on the universe coordination path to preserve server ownership rules.

### 6.3 Compatibility impact

Low to medium. Packet sequence must stay identical, but implementation timing changes.

### 6.4 Test plan

- successful legacy and OpenStarbound protocol handshakes
- protocol version mismatch
- asset digest mismatch
- anonymous login disabled
- password challenge success and failure
- timeout in every handshake state
- duplicate UUID handling

### 6.5 Performance estimate

Expected improvement:

- login burst CPU and memory overhead: 20-70% lower
- slow-client resilience: significantly better because slow handshakes no longer pin full threads
- steady-state TPS: usually 0-3% better
- server responsiveness during connection bursts: 10-30% better

## 7. Phase 3: Async Persistence And Snapshot Writes

Purpose: remove disk and serialization work from hot orchestration paths.

### 7.1 Current issue

`UniverseServer::doTriggeredStorage()` performs work such as:

- reading ship chunks from world threads
- serializing client contexts
- writing versioned JSON files
- calling celestial cleanup and commit

Some disconnection paths also write client context data synchronously.

### 7.2 Engineering changes

Introduce snapshot types:

- `ClientContextSnapshot`
- `ShipWorldSnapshot`
- `UniverseSettingsSnapshot`
- `TempWorldIndexSnapshot`
- `CelestialCommitRequest`

Introduce a persistence executor:

- bounded worker count
- queue depth limits
- backpressure metrics
- completion and failure reporting to `UniverseServer`

Change owner-thread responsibilities:

- world thread produces ship/world snapshots at safe barriers
- universe thread produces client and universe snapshots
- persistence executor writes snapshots to disk
- universe thread records failures and schedules retries or disconnect safety work

### 7.3 Important safety rules

- never write live mutable objects from a background thread
- never let a background write observe partially mutated world state
- preserve shutdown behavior by flushing outstanding required writes before process exit
- preserve backup and versioning behavior exactly

### 7.4 Compatibility impact

Low if snapshot content and write order remain equivalent. Medium if shutdown and failure handling are not carefully designed.

### 7.5 Test plan

- save during normal play
- save during ship upgrade
- disconnect while save is pending
- shutdown while save is pending
- simulated write failure
- old save load after async-written snapshot

### 7.6 Performance estimate

Expected improvement:

- storage-trigger tick spikes: 30-80% lower
- universe loop p95/p99 duration during saves: 20-60% lower
- one crowded world TPS: 0-10% better unless storage was frequently stalling it
- perceived lag during autosave or disconnect: 10-40% better

## 8. Phase 4: Strict World Mailbox Ownership

Purpose: make world threads true owners of world state and remove foreign-thread direct mutation.

### 8.1 Current issue

`WorldServerThread` exposes methods that enter world state directly under a recursive mutex. Examples include:

- `addClient`
- `removeClient`
- `executeAction`
- `readChunks`
- `sync`

`SystemWorldServerThread` has similar patterns with write locks around live system-world state.

These methods are safe enough for the current model, but they prevent clean actor-style scaling and make future parallelism risky.

### 8.2 Engineering changes

Introduce typed world commands:

- `AddClientCommand`
- `RemoveClientCommand`
- `SpawnTargetValidCommand`
- `ReadChunksSnapshotCommand`
- `SetPauseCommand`
- `SetShipPropertyCommand`
- `StartFlyingSkyCommand`
- `StopFlyingSkyCommand`
- `PassMessagesCommand`
- `UnloadAllCommand`
- `SyncCommand`

Add command queues to `WorldServerThread`:

- inbound command queue
- reply promise support for commands that need results
- bounded queue metrics
- failure propagation when world thread errors

Process commands at explicit tick boundaries:

1. drain inbound commands
2. apply incoming packets
3. run world update
4. process messages
5. publish outgoing packets and snapshots

For `SystemWorldServerThread`, introduce equivalent commands:

- add/remove client ship
- set destination
- execute client ship action replacement with typed commands where possible
- read client ship location snapshot
- read active instance worlds snapshot

### 8.3 UniverseServer migration points

Refactor these areas to enqueue commands instead of entering worlds directly:

- `warpPlayers`
- `flyShips`
- `arriveShips`
- `updateShips`
- `shutdownInactiveWorlds`
- `doTriggeredStorage`
- `doDisconnection`
- admin and script paths that call `executeForClient`

### 8.4 Compatibility impact

Medium. The intent is behavior-preserving, but command processing boundaries may shift exact timing by one tick unless carefully staged.

### 8.5 Test plan

- warp to own ship, party ship, planet, and instance
- beam down and beam up
- ship flight and arrival
- disconnect while in world
- idle world shutdown
- errored world shutdown
- entity messages with replies
- admin commands that act on players or worlds

### 8.6 Performance estimate

Expected improvement:

- direct single-world throughput: 0-15% initially
- lock contention and tail latency: 10-40% better in multi-client/multi-world servers
- scalability across many active worlds: 20-100% better once universe/world lock contention is reduced
- future parallelism enablement: high

This phase is strategically important even if immediate TPS gains are modest.

## 9. Phase 5: Snapshot-Based Post-Tick Parallel Work

Purpose: use additional cores around the serial mod-visible world update lane.

### 9.1 Current issue

`WorldServer::update()` performs many tasks serially. Some of those tasks are gameplay-visible and should remain serial. Others can be split if they read stable state and publish results at barriers.

### 9.2 Candidate tasks

Lower risk:

- packet delta encoding from frozen state
- region and interest-set calculation
- persistence snapshot serialization
- metrics collection
- storage generation planning
- sector unload analysis

Higher risk:

- liquid processing
- falling-block propagation
- wire-network evaluation

Highest risk:

- entity update execution
- Lua script execution
- direct cross-entity message processing

### 9.3 Engineering shape

Add a world-local job scheduler or use a shared worker pool with strict barriers:

1. serial authoritative update runs
2. world thread freezes read-only snapshot data
3. post-tick jobs run in parallel
4. world thread merges results at the next safe boundary

### 9.4 Compatibility impact

Low for snapshot-only tasks, high for direct simulation tasks.

### 9.5 Test plan

- compare packet streams before and after for deterministic scenarios
- compare save output before and after where expected stable
- stress clients entering and leaving monitored regions
- verify no Lua-observable ordering changes in default mode

### 9.6 Performance estimate

Expected improvement:

- packet-heavy worlds: 5-25% better world tick time
- save-heavy or generation-heavy worlds: 10-35% better p95/p99 tick time
- entity/Lua-heavy modded worlds: 0-15% unless deeper experimental work follows
- multicore utilization: visibly better when packet encoding, snapshotting, or generation are significant fractions of tick time

## 10. Phase 6: Experimental World-Internal Parallelism

Purpose: explore stronger single-world scaling after safer phases are complete.

### 10.1 Candidate order

Try in this order:

1. world storage generation and unload analysis
2. packet replication preparation
3. liquid processing by independent regions
4. falling-block processing by non-overlapping regions
5. wiring by disconnected networks
6. entity update groups only if strong dependency analysis exists

### 10.2 Guardrails

- off by default at first
- controlled by server config
- deterministic fallback path always available
- runtime detection for unsafe mod or script assumptions where possible
- per-world metrics comparing parallel and serial work durations

### 10.3 Compatibility impact

Medium to very high depending on subsystem.

### 10.4 Performance estimate

Expected improvement for one crowded world:

- conservative low-risk subset: 10-30%
- successful region/subsystem parallelism: 20-80%
- entity/Lua parallelism: potentially larger, but not recommended as a compatibility-preserving default

## 11. Second-Pass Performance Review And Estimates

These estimates are planning ranges, not guaranteed benchmark results. They assume representative Linux dedicated-server load and no major algorithmic rewrites beyond the described phases.

### 11.1 Single crowded world

This is the workload most likely to show one hot core.

Expected gains:

| Work completed | Expected gain | Confidence | Notes |
| --- | ---: | --- | --- |
| Network sharding and wakeups | 0-5% TPS, lower idle CPU | Medium | Helps networking overhead, not core simulation |
| Async handshakes | 0-3% TPS, 10-30% better during login bursts | Medium | Mostly burst resilience |
| Async persistence | 0-10% TPS, 20-60% lower save spikes | Medium | More useful if saves are visible in p95/p99 |
| Mailbox world ownership | 0-15% TPS, 10-40% lower contention spikes | Medium | Mostly enables future work |
| Snapshot post-tick jobs | 5-25% tick-time improvement | Low to Medium | Depends on packet/snapshot share of tick cost |
| Experimental subsystem parallelism | 10-80% | Low | Depends heavily on liquid/wiring/storage share and compatibility constraints |

Realistic compatibility-first expectation for one crowded world after phases 1-5: 10-35% better p95/p99 tick behavior, with occasional larger gains if persistence, packet preparation, or world generation are major costs.

### 11.2 Many active worlds

This workload can benefit more because each world already has its own thread.

Expected gains:

| Work completed | Expected gain | Confidence | Notes |
| --- | ---: | --- | --- |
| Mailbox world ownership | 20-100% better scaling under contention | Medium | Removes central lock coupling and direct world entry |
| Async persistence | 10-40% better tail latency | Medium | Prevents storage from stalling coordination |
| Network sharding | 10-40% lower network overhead at high client count | Medium | Especially useful with many idle connections |
| Snapshot post-tick jobs | 5-25% per hot world | Low to Medium | Depends on per-world composition |

Realistic compatibility-first expectation for many active worlds: 1.3x to 2.5x better effective throughput on multicore hosts, assuming load is spread across worlds and storage/network overhead is non-trivial.

### 11.3 Idle or low-population server

Expected gains:

- CPU usage can drop substantially from network wakeup reduction
- latency can improve slightly because work wakes on events instead of polling intervals
- TPS gains are not meaningful because the server is not saturated

Expected idle CPU reduction after network wakeup changes: 30-80%, depending on connection count and platform poller implementation.

### 11.4 Amdahl limit for single-world scaling

If a single world tick remains mostly serial, total speedup is capped even if every safe background task becomes parallel.

Examples:

| Serial share of world tick | Best possible speedup if all other work is free |
| ---: | ---: |
| 85% | 1.18x |
| 70% | 1.43x |
| 60% | 1.67x |
| 50% | 2.00x |

This is why entity and Lua update order matters so much. If mods make entity and Lua work dominate the world tick, compatibility-preserving parallelism can reduce spikes and overhead but cannot fully use all cores for that one world.

## 12. Recommended Implementation Order

Recommended order for the team:

1. Add measurement and Linux profiling support.
2. Shard network workers and reduce polling wakeups.
3. Convert handshake handling to a state machine.
4. Move persistence writes and serialization behind snapshots.
5. Convert world and system-world access to mailbox ownership.
6. Add snapshot-based post-tick jobs.
7. Experiment with world-internal parallelism behind config flags.

This order gives useful gains early, lowers risk, and builds the ownership model needed for deeper multicore work.

## 13. Immediate Backlog

### 13.1 Measurement tickets

- Add `UniverseServer` phase timing.
- Add `WorldServerThread` phase timing.
- Add `WorldServer::update()` subphase timing.
- Add network worker scan, wakeup, and idle counters.
- Add world mutex wait counters.
- Extend `world_benchmark` to print subphase timings.

### 13.2 Low-risk performance tickets

- Stop network workers from scanning connections they do not own.
- Add worker wakeups for queued sends and connection changes.
- Move connection handshakes to an explicit state machine.
- Queue client-context writes onto a persistence executor.
- Queue ship chunk snapshots from the world owner thread.
- Apply inbound entity deltas by packet key instead of scanning all world entities.
- Cache per-client monitoring regions and reuse them across one world tick.
- Replace per-tile full-client fan-out with sector-to-client replication fan-out.
- Precompute world-generation sector priorities before sorting the generation queue.
- Replace liquid no-limit region list scans with a per-tick sector or cell membership cache.
- Avoid full wiring-network scans when no wire topology or output state changed.
- Cache entity create stores per entity and net-compatibility rules during one replication tick.

### 13.3 Structural tickets

- Add typed world command queue.
- Convert `addClient` and `removeClient` to commands.
- Replace generic `executeAction()` call sites with typed commands.
- Add stable post-tick world snapshots.
- Convert system-world mutation paths to commands.

### 13.4 Experimental tickets

- Parallel packet-delta encoding from frozen snapshots.
- Parallel world storage generation planning.
- Region-based liquid experiment behind config flag.
- Disconnected-network wiring experiment behind config flag.

## 14. Storage Backend Recommendation

The current world and shipworld persistence layer uses `BTreeDatabase`, and player persistence is file-oriented around `VersionedJson` plus shipworld chunk files.

That matters because a `HashMap` is not a drop-in faster version of the same thing.

### 14.1 Why a straight `BTree -> HashMap` replacement is unlikely to be the best next step

- `BTreeDatabase` is a durable on-disk structure with commit and rollback semantics.
- `WorldStorage` uses compact fixed-size keys and compressed record payloads for sectors and indexes.
- unique-entity storage already hashes ids into bucketed keys before storing the bucket contents.
- `PlayerStorage` bottlenecks are more likely to come from whole-file writes, shipworld snapshot export, and serialization frequency than from map lookup cost.

### 14.2 Better compatibility-preserving storage work

- reduce how often `readChunks()` full snapshots are required on hot paths
- prefer incremental chunk updates where the caller already has an old snapshot
- move storage serialization and commit work off orchestration or world hot paths
- profile `BTreeDatabase` cache size and commit behavior before considering a backend swap
- avoid rewriting unchanged player data or unchanged sector payloads more often than necessary

### 14.3 Concrete Storage Tickets

| Ticket | Scope | Expected impact | Risk | Notes |
| --- | --- | ---: | --- | --- |
| Add storage timing counters | `WorldStorage`, `PlayerStorage`, `BTreeDatabase` call sites | None directly | Low | Measure read, write, compression, commit, and full-snapshot export time before changing storage behavior. |
| Move shipworld snapshot persistence off the universe path | `UniverseServer::doTriggeredStorage`, `ServerClientContext`, `WorldServerThread` | 10-40% lower save/disconnect spikes where ship saves are visible | Medium | Keep snapshot creation on owner threads, but write and apply chunk files on a persistence executor. |
| Replace avoidable `readChunks()` full exports with incremental chunk updates | shipworld update paths | 5-25% lower storage CPU in ship-heavy flows | Medium | Best when the caller already has an old snapshot and only needs a delta. Must preserve disconnect and upgrade semantics. |
| Dirty-sector write filtering | `WorldStorage::syncSector`, sector mutation paths | 5-20% lower world sync cost on mostly idle worlds | Medium | Track dirty tile/entity sectors so sync does not rewrite sectors that were merely loaded. Needs careful marking on generation, tile edits, entity movement, and unload. |
| Tune `BTreeDatabase` cache and commit cadence | `WorldStorage::openDatabase`, storage configs | 0-15% lower persistence overhead | Low to Medium | Measure `indexCacheSize`, block cache behavior, and commit frequency under real saves. Avoid changing file format. |
| Async compression for sector payloads | `writeTileSector`, `writeEntitySector`, persistence executor | 10-35% lower p95/p99 sync spikes when compression dominates | Medium | Requires immutable sector snapshots. Do not compress live mutable sector arrays off-thread. |
| In-memory player name index | `PlayerStorage` name lookup | Small steady-state gain, better admin/search latency with many players | Low | Maintain normalized-name index alongside `m_savedPlayersCache`; persistence format unchanged. |
| Storage backend benchmark harness | utility/test code | None directly | Low | Compare current B-tree settings, compression cost, full scans, and candidate alternatives on real `.world` and `.shipworld` files. |

### 14.4 When a backend replacement would make sense

Only consider a new storage engine after measurements show that the dominant cost is truly inside B-tree block traversal or write amplification rather than serialization, compression, commit frequency, or cross-thread scheduling.

If that day comes, it should be treated as a separate migration project with:

- save-format compatibility review
- corruption-recovery testing
- tooling updates
- migration and rollback planning
- dedicated benchmark comparison on real world and shipworld workloads

## 15. Deeper Compatibility-Safe Subsystem Tickets

These tickets come from a second source pass through world generation, liquid processing, wiring, and entity serialization paths.

| Ticket | Source area | Expected impact | Risk | Compatibility notes |
| --- | --- | ---: | --- | --- |
| Precompute generation queue priorities | `WorldServer::update`, `WorldStorage::generateQueue` | 1-10% world tick improvement during active generation | Low | Preserve sector ordering by computing the same nearest-player distance once per sector before sorting. |
| Avoid repeated player lookup in generation sorting | `WorldServer::update` world-storage generation lambda | 1-5% during generation bursts | Low | Snapshot player positions before sorting; tie behavior should remain stable or explicitly match old order. |
| Cache liquid unlimited-region membership | `LiquidCellEngine::setup` | 5-20% liquid update improvement when many active cells and client regions exist | Low to Medium | Replace per-cell linear scan over `m_noProcessingLimitRegions` with per-tick sector/cell membership or merged rectangles. Keep liquid update order unchanged. |
| Merge or normalize liquid no-limit regions | `WorldServer::update`, `LiquidCellEngine` | 0-10% liquid update improvement | Low | Reuse cached monitoring regions from the replication work; reduce duplicate/overlapping rectangles before passing them to liquid. |
| Add wiring dirty-network tracking | `WireProcessor::process`, wire connection mutation paths | 5-30% wiring update improvement on wiring-heavy worlds | Medium | Default behavior can remain full scan until a dirty index is proven; dirty marks must fire on connection changes, object placement/removal, and node output changes. |
| Evaluate disconnected wire networks independently | `WireProcessor::process` | 5-25% on large wiring layouts | Medium | Can enable later parallelism by network component. Must preserve within-network evaluation semantics. |
| Cache entity create stores per tick | `WorldServer::queueUpdatePackets`, `EntityFactory::netStoreEntity` | 2-15% replication improvement when clients enter dense regions | Low | Cache by entity id and net-compatibility rules for the duration of one tick; packet bytes remain identical. |
| Extend net-state cache to first updates | `WorldServer::queueUpdatePackets` | 2-10% replication improvement | Low | Existing cache covers deltas by version; first-update `writeNetState(0)` and `netStoreEntity` can use the same per-tick cache discipline. |
| Add per-entity serialization counters | entity `writeNetState`, `EntityFactory::netStoreEntity` | None directly | Low | Identifies whether players, NPCs, monsters, objects, or projectiles dominate replication cost before deeper changes. |

## 16. Go/No-Go Criteria

Proceed from one phase to the next only when:

- p95 and p99 tick metrics improve or remain stable
- packet ordering tests pass
- save/load compatibility tests pass
- world transition and disconnect tests pass
- modded-world smoke tests pass
- CPU utilization moves from one hot thread toward expected worker/world distribution

Stop or gate behind config if:

- Lua-visible ordering changes
- packet streams reorder unexpectedly
- save output changes without an intentional migration
- world unload or disconnect races increase
- p99 tick time regresses under representative load
