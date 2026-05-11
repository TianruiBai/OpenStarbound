# OpenStarbound Performance and Multithreading Review

This document records a source-backed review of the current performance and concurrency design in OpenStarbound.

It is intended for engineers planning performance work, lock-scope reductions, network scaling changes, or a broader threading model modernization.

This is a static code review, not a profiler report. The recommendations below are based on verified control flow and synchronization patterns in the current implementation.

For a phase-by-phase implementation plan and second-pass performance estimates, see `doc/architecture/multicore-engineering-plan.md`.

## 1. Scope

The review focused on the code paths that dominate thread ownership, packet flow, world updates, and storage coordination:

- `source/game/StarUniverseServer.cpp`
- `source/game/StarUniverseConnection.cpp`
- `source/game/StarWorldServerThread.cpp`
- `source/game/StarNetPacketSocket.cpp`
- `source/core/StarWorkerPool.cpp`
- `source/core/StarAtomicSharedPtr.hpp`
- `source/core/StarThread.hpp`
- `source/base/StarAssets.cpp`
- `source/core/StarBTreeDatabase.cpp`

## 2. Executive Summary

The current server runtime uses multiple threads, but the design still relies heavily on shared mutable state protected by coarse locks, periodic polling loops, and synchronous handoffs between subsystems.

The most important conclusion is that OpenStarbound is not primarily limited by a lack of threads. It is limited by where those threads are allowed to do real work independently.

The strongest performance and multithreading opportunities are:

1. move world logic from shared-lock access to thread ownership and mailbox-style command passing
2. replace sleep-based networking loops with readiness-driven I/O and true worker sharding
3. remove persistence and maintenance work from the universe tick path
4. simplify low-level synchronization primitives only after the larger scheduling bottlenecks are addressed

## 3. Confirmed Findings

### 3.1 World threads still expose shared-state access instead of owning world state

The current world-thread abstraction is named like a thread-owned simulation boundary, but several public methods still allow foreign threads to enter world state directly under `WorldServerThread::m_mutex`.

Verified entry points:

- `WorldServerThread::addClient()`
- `WorldServerThread::removeClient()`
- `WorldServerThread::executeAction()`
- `WorldServerThread::sync()`
- `WorldServerThread::readChunks()`

The core update loop in `WorldServerThread::update()` then holds that same mutex across:

- incoming packet handling
- world simulation update
- world message delivery
- outgoing packet extraction
- update callback execution

Why it matters:

- world update work, administration, and external world actions serialize on the same recursive lock
- adding more world-related threads will not improve throughput if they keep funneling through the same shared lock
- recursive locking also makes lock ownership harder to reason about and reduces confidence in future parallel refactors

Modernization direction:

- make the world thread the exclusive owner of mutable world state
- turn external calls into queued commands or messages
- publish outgoing packet batches and read-only snapshots from the owner thread
- reserve direct locking for small metadata that is genuinely cross-thread

This is the highest-value concurrency refactor in the codebase.

### 3.2 Networking still depends on polling sleeps and repeated full-connection scans

The networking layer uses explicit polling loops at both the individual connection level and the connection-server worker level.

Verified patterns:

- `UniverseConnection::sendAll()` loops until output drains and sleeps for `PacketSocketPollSleep`
- `UniverseConnection::receiveAny()` loops until any packet arrives and sleeps for `PacketSocketPollSleep`
- `UniverseConnectionServer` workers copy `m_connections.pairs()` each pass, then filter by `workerIndex`
- when no data was transmitted, the worker sleeps for `PacketSocketPollSleep`

Why it matters:

- idle or lightly loaded servers still wake up frequently just to discover no work is available
- each worker performs bookkeeping over the whole connection set, not just its own shard
- latency is tied to poll intervals rather than socket readiness
- CPU time is spent on scheduler wakeups and queue churn instead of useful packet processing

Modernization direction:

- partition connections by worker instead of rescanning the full map on every loop
- move to readiness-driven socket processing rather than sleep polling
- preserve per-connection ordering, but let worker wakeups follow actual network activity
- keep transport buffering, packet framing, and compression separate from worker scheduling decisions

This is the second-highest scaling issue after world ownership.

### 3.3 UniverseServer is a serial scheduler and still performs durable work on the tick path

`UniverseServer::run()` performs a long sequence of universe maintenance passes and then unconditionally sleeps for `mainWakeupInterval`.

That same runtime path also performs persistence-related work in `doTriggeredStorage()`, including:

- ship chunk reads from world threads
- per-client context serialization
- `VersionedJson::writeFile(...)`
- `m_celestialDatabase->cleanupAndCommit()`

Why it matters:

- tick latency depends on the slowest maintenance pass in the current cycle
- storage activity competes directly with game coordination work
- a fixed sleep loop is simple, but it makes the universe scheduler less responsive to bursty work and harder to optimize incrementally

Modernization direction:

- separate periodic maintenance from latency-sensitive coordination
- snapshot data on the universe thread and hand off actual persistence to background workers
- move from fixed-sleep scheduling to wake-on-work or wake-on-deadline scheduling where practical
- keep the universe thread focused on orchestration, not long-running disk or cleanup tasks

This is the strongest non-network scheduling issue in the server.

### 3.4 Connection acceptance uses one thread per pending handshake and blocks during protocol setup

Incoming TCP connections are accepted in `UniverseServer::run()`, which spawns a dedicated `Thread::invoke("UniverseServer::acceptConnection", ...)` task for each pending handshake.

Inside `acceptConnection()` the handshake path performs timeout-driven blocking steps such as:

- `connection.receiveAny(clientWaitLimit)`
- `connection.sendAll(clientWaitLimit)`
- version and asset compatibility checks
- optional password challenge/response handling

Why it matters:

- the design is bounded by `maxPendingConnections`, so it is not unbounded thread explosion
- but slow or hostile handshakes still consume a full thread slot for their lifetime
- the cost of pending logins grows with thread scheduling overhead instead of actual I/O readiness

Modernization direction:

- fold handshake progression into the network worker model, or
- use a bounded handshake queue serviced by a small dedicated pool
- treat login as an explicit connection state machine instead of a blocking thread function

This is worth addressing, but it is lower priority than the world and network worker models.

### 3.5 SpinLock remains a pure busy-spin primitive and is used in shared helpers

`SpinLock::lock()` is implemented as a raw `test_and_set` loop with no backoff, yield, or pause instruction.

Verified uses reviewed in this pass:

- `AtomicSharedPtr` takes the spinlock for every load, store, and pointer-style access
- `BTreeDatabase` uses a spinlock around its index-cache access path

Why it matters:

- if these sites become contended, they burn CPU instead of yielding quickly
- spinlocks make sense only when contention windows are tiny and demonstrably rare
- they are not the primary source of lost performance here, but they are poor primitives to carry into a broader threading modernization

Modernization direction:

- replace with `std::atomic<std::shared_ptr<T>>` style operations where supported and appropriate
- otherwise use a normal mutex for non-hot paths
- if a real spin primitive is still needed, add an adaptive backoff strategy

This should be treated as a secondary cleanup after the larger architectural bottlenecks.

### 3.6 Inbound entity delta application still scans the whole world

The server-side `EntityUpdateSetPacket` path in `WorldServer::handleIncomingPackets()` does not iterate only the entity ids present in the packet.

Verified pattern:

- on `EntityUpdateSetPacket`, `WorldServer` calls `m_entityMap->forAllEntities(...)`
- inside that loop, it filters by `connectionForEntity(entityId) == clientId`
- it then reads the delta from `entityUpdateSet->deltas.value(entityId)`

Why it matters:

- applying one client delta batch scales with total entity count in the world, not with the number of changed entities in the packet
- busy worlds pay this scan cost even when only a small subset of client-owned entities actually changed
- the pattern compounds the single-hot-world problem because it sits directly on the authoritative world thread

Modernization direction:

- iterate the keys present in the packet delta set
- look up each entity directly in `EntityMap`
- verify connection ownership before applying the delta
- keep packet format and replication ordering unchanged

This is a high-confidence, compatibility-preserving optimization.

### 3.7 World replication rebuilds monitoring and visibility state multiple times per tick

`WorldServer::update()` and `WorldServer::queueUpdatePackets()` repeatedly recompute per-client monitoring regions and monitored entity sets from the same client state.

Verified patterns:

- monitoring regions are built once to populate `clientMonitoringRegions` for weather and liquid processing
- the same monitoring regions are built again for `signalRegion(...)`
- they are built again inside `queueUpdatePackets()` to gather `monitoredEntities`
- `WorldClientState::monitoringRegions(...)` resolves presence-entity bounds through callbacks each time it runs

Why it matters:

- the world thread repeats the same set-building work several times in one tick
- every recomputation may re-query entity bounds and spatial lookups
- the overhead grows with client count, presence entities, and world density even when nothing structural changed in the client view setup

Modernization direction:

- compute monitoring regions once per client per tick
- reuse those cached regions for weather visibility, liquid no-limit regions, sector activation, and monitored-entity collection
- optionally cache the per-client monitored entity set for the duration of the tick

This is another low-risk optimization because it changes how often the server derives the same sets, not what packets or world behavior it produces.

### 3.8 Tile replication fan-out still scans all clients for each changed tile

Tile, tile-damage, and liquid update queueing still use per-tile fan-out across the full client set.

Verified patterns:

- `queueTileUpdates()` loops over every client and checks whether the tile's sector is active for that client
- `queueTileDamageUpdates()` does the same
- liquid collection and several tile-mutation paths also append pending liquid updates through the full client set

Why it matters:

- high-frequency tile mutation paths pay an `O(changed tiles x clients)` membership-check cost on the world thread
- crowded building, liquid, wiring, or destruction scenarios amplify that cost quickly
- the work is mechanically simple, but repeated often enough to matter on hot worlds

Modernization direction:

- maintain a sector-to-subscribed-client index or equivalent reverse mapping
- queue sector-local batches and fan them out once per sector instead of once per tile mutation
- preserve current packet contents and ordering per client

This is a good compatibility-preserving optimization because it targets fan-out bookkeeping rather than simulation behavior.

### 3.9 System world replication does per-client full-state walks without shared delta caching

`SystemWorldServer::queueUpdatePackets()` walks every ship and every object for every connected client and calls `writeNetState(...)` separately for each client version map.

Why it matters:

- the cost scales with `clients x ships x objects`
- the world server already uses a net-state cache for entity updates keyed by net rules and prior version, but system-world replication does not
- this is less important than the main world tick, but it is still a clear source of redundant serialization work

Modernization direction:

- add dirty tracking or shared delta caching for system ships and objects
- reuse identical update payloads across clients where version state allows
- avoid emitting empty update packets unless they serve an explicit heartbeat purpose

This should be treated as a medium-priority optimization after the main-world hot path changes.

### 3.10 World generation queue sorting recomputes player distance inside the comparator

`WorldServer::update()` passes a sector ordering callback into `WorldStorage::generateQueue()` when world storage generation runs.

Verified pattern:

- `WorldStorage::generateQueue()` sorts `m_generationQueue` with the supplied sector comparator
- the comparator computes `distanceToClosestPlayer(a)` and `distanceToClosestPlayer(b)`
- each distance calculation resolves the sector region, then scans every client and looks up each player entity

Why it matters:

- sort comparators run many times, so repeated player scans and entity lookups multiply quickly during generation bursts
- this cost occurs on the world thread before sector generation itself
- it is especially visible when many sectors are queued near active players

Modernization direction:

- snapshot relevant player positions before sorting
- precompute one priority value per queued sector
- sort by cached priority rather than recomputing nearest-player distance for every comparator call

This is low risk if tie behavior is preserved or made explicitly stable.

### 3.11 Liquid active-cell setup scans no-limit regions per active cell

`LiquidCellEngine::setup()` applies `m_processingLimit` by checking whether each active cell is inside any no-processing-limit region.

Verified pattern:

- for every active liquid cell under a processing limit, it linearly scans `m_noProcessingLimitRegions`
- those regions are rebuilt from client monitoring regions in `WorldServer::update()`
- active cells are then sorted and processed through several serial passes

Why it matters:

- liquid-heavy worlds can have many active cells
- client monitoring regions are often few and stable within a tick, making repeated linear region checks avoidable
- this is still compatibility-sensitive because the actual liquid update order and random lateral movement order should not change casually

Modernization direction:

- cache unlimited-region membership at sector or cell granularity for one tick
- merge or normalize overlapping monitoring rectangles before passing them to liquid
- keep the existing active-cell order and liquid movement algorithm unchanged

This is a compatibility-preserving bookkeeping optimization, not a liquid-physics rewrite.

### 3.12 Wiring processing starts from a full entity scan each wiring update

`WireProcessor::process()` begins by scanning all live entities and adding every `WireEntity` to the working set. It then loads connected wire networks and evaluates every working wire entity.

Why it matters:

- wiring-heavy worlds pay a full entity scan even when only a small number of wire outputs or connections changed
- connected-network loading can also pull additional sectors into memory during the update
- the current approach is conservative and behaviorally simple, but it leaves clear room for dirty-network tracking

Modernization direction:

- maintain a tile-position index of live wire entities or expose one from the entity map/world layer
- mark wire networks dirty on connection changes, object placement/removal, and output-state changes
- evaluate unchanged disconnected networks only when their inputs or topology changed
- keep the full-scan path as a fallback during rollout

This is medium risk because missed dirty marks can create stale wiring behavior, but it can preserve compatibility if guarded and validated carefully.

### 3.13 Entity create serialization is not cached like entity delta serialization

`WorldServer::queueUpdatePackets()` already caches repeated `writeNetState(version, netRules)` results for entity deltas within one tick through `m_netStateCache`.

The first-observation path does not use an equivalent cache:

- `writeNetState(0, netRules)` is called when a client first observes an entity
- `EntityFactory::netStoreEntity(monitoredEntity, netRules)` is also called for the create packet
- this can repeat for multiple clients entering the same dense region in the same tick

Why it matters:

- dense settlements, ship hubs, and spawn areas can create many first-observation packets
- `EntityFactory::netStoreEntity()` is protected by a recursive mutex and dispatches to type-specific serializers
- repeated create-store serialization is packet-compatible but redundant when net rules match

Modernization direction:

- cache first-update net state by entity id and net-compatibility rules for the current tick
- cache net store payloads by entity id and net-compatibility rules for the current tick
- clear both caches at the same barrier as `m_netStateCache`

This is low risk because it reuses identical serialized bytes within a tick and does not change packet structure.

## 4. Profile-First Watchlist

The items below are real lock-scope or maintenance patterns, but they are lower confidence as top-level bottlenecks without profiling data.

### 4.1 Asset cache maintenance scans under the main asset mutex

`Assets::clearCache()` and `Assets::cleanup()` iterate the full asset cache while holding `m_assetsMutex`.

This may become visible if the cache grows large or if cleanup runs too often, but it is not as strong a concern as the server-side scheduling and ownership issues.

### 4.2 Celestial database cleanup and commit under database lock

`CelestialMasterDatabase::cleanupAndCommit()` performs cache cleanup and conditional commit while holding its recursive mutex.

This is worth measuring during long-running servers, especially if persistence intervals are shortened or modded universes grow large.

### 4.3 World and player storage should not be treated as an in-memory HashMap replacement problem

It is reasonable to ask whether world and player persistence would benefit from replacing the current B-tree store with a hash table or another supposedly faster structure.

The code reviewed here suggests that a wholesale `BTree -> HashMap` replacement is unlikely to be the right first optimization.

Verified facts:

- `WorldStorage` uses `BTreeDatabase` as an on-disk key-value store with fixed-size keys, explicit commit/rollback, and compressed record payloads
- world sector storage uses compact 5-byte keys for metadata, tile sectors, entity sectors, unique-index buckets, and sector-unique data
- unique-entity lookup already hashes string ids into bucket keys with `xxHash32(uniqueId)`
- `PlayerStorage` is file-oriented: `.player` files are `VersionedJson`, `.shipworld` files are world-chunk databases, and metadata is a plain JSON file
- `WorldStorage::readChunks()` and `WorldStorage::getWorldChunksFromFile()` intentionally walk the whole database because they are exporting a full persistent snapshot, not performing a point lookup

Why it matters:

- a plain in-memory `HashMap` is not a substitute for a transactional, durable, block-structured on-disk store
- replacing the backend would be a large compatibility and data-integrity project, not a local performance tweak
- current storage costs appear more likely to come from serialization, compression, full-snapshot reads, commit frequency, and work being done on hot threads than from B-tree lookup complexity alone

More plausible optimization targets are:

- avoid full `readChunks()` snapshot exports on hot paths when incremental chunk updates are sufficient
- move storage serialization and commit scheduling off the universe coordination path, as already outlined in the engineering plan
- profile and tune `BTreeDatabase` parameters such as cache size and commit cadence before considering a backend replacement
- reduce how often world sectors are copied, compressed, or rewritten when only a small portion changed
- keep player metadata indexes in memory for name and UUID lookup, but leave durable player and shipworld persistence on disk-oriented formats unless measurements prove otherwise

Practical conclusion:

- for compatibility-preserving work, optimize storage access patterns first
- treat a storage-engine replacement as a later, much riskier project that would need migration, corruption-recovery, durability, and tooling review

## 5. Findings Explicitly Removed From The Earlier Draft

Two earlier concerns did not survive direct verification and should not drive planning decisions.

### 5.1 Asset loading is not using a pure busy-wait path

The asset loader waits with condition variables such as `m_assetsDone.wait(m_assetsMutex)` and `m_assetsQueued.wait(m_assetsMutex)`.

That means the strongest earlier claim about asset-loading busy waiting was incorrect.

### 5.2 `executeForClient()` is not dispatching a detached cross-thread callback

`UniverseServer::executeForClient()` unlocks the universe lock, calls `currentWorld->executeAction(...)`, and that action executes synchronously under the world mutex.

The real concern there is architectural coupling between universe and world locking, not a detached callback lifetime bug.

## 6. Recommended Modernization Sequence

### Phase 1: Measure and reduce scheduling waste

- add per-loop timing around the major `UniverseServer::run()` passes
- record packet-worker idle wakeups, connection scan counts, and per-worker handled connections
- measure time spent in `WorldServerThread::update()` versus time spent waiting on world mutex entry points

### Phase 2: Rework world ownership

- convert `WorldServerThread` external entry points into mailbox commands
- keep outgoing packets and admin responses as explicit products of the world thread
- remove direct foreign-thread mutation of `WorldServer`

### Phase 3: Rework networking worker structure

- shard connections by worker at ownership time
- eliminate repeated full-map scans in network workers
- replace poll sleeps with readiness-based wakeups

### Phase 4: Move persistence off the orchestration path

- snapshot per-client and per-world data on owner threads
- write snapshots from background workers
- keep commit cadence visible and measurable

### Phase 5: Clean up low-level primitives

- replace or downgrade spinlock use where contention is possible
- reduce recursive mutex usage once ownership boundaries are flatter
- revisit worker-pool and cache helpers only after the higher-level changes land

## 7. Suggested Validation Metrics

If the team starts modernizing these areas, the most useful success metrics will be:

- universe tick duration percentiles
- world update duration percentiles
- time spent waiting to enter world-owned operations
- network worker idle wakeups per second
- packet latency under mostly-idle and bursty workloads
- persistence time spent on the universe thread versus background workers
- CPU utilization on low-player and high-player servers

The goal should be to show that concurrency changes reduce contention and wakeups, not just increase thread count.

## 8. Why A Live Linux Server Can Still Look Single-Core

The codebase does create multiple threads today:

- `UniverseServer` runs its own orchestration loop
- `UniverseConnectionServer` runs network worker threads
- each loaded `WorldServerThread` owns a world thread
- each loaded `SystemWorldServerThread` owns a system-world thread
- world creation and selected database requests use a worker pool

Even with that structure, a production server can still present like it is mostly using one core.

### 8.1 The busiest gameplay path is still one serial world tick

When players are concentrated in one active world, the dominant gameplay work lands inside one `WorldServer::update()` call on one `WorldServerThread`.

That update currently executes a long, ordered sequence in one thread, including:

- entity updates via `m_entityMap->updateAllEntities(...)`
- world script context updates via `m_scriptContexts`
- damage and wiring processing
- sky and weather updates
- liquid and falling-block processing
- world storage ticking and generation queue advancement
- outgoing packet preparation

That is enough by itself to saturate one core when a single world becomes busy.

### 8.2 Extra threads do not help much if the load is concentrated in one world

The current thread model scales better across many simultaneously active worlds than within one very busy world.

That means:

- players split across different worlds can use more cores naturally
- players packed into one main world tend to bottleneck on one world thread
- helper threads may exist but remain much cooler than the dominant world thread

This is the most likely explanation for a Linux server showing one consistently hot core in practice.

### 8.3 Universe coordination is still centralized around one server thread

`UniverseServer::run()` still owns the main coordination loop for:

- chat, team, warp, flight, and connection maintenance
- world lifecycle management
- periodic storage work
- universe Lua updates

This usually will not be the hottest thread in a crowded single-world server, but it does create another serial choke point that prevents the rest of the runtime from behaving like a fully parallel actor system.

### 8.4 Network workers are present, but they are not the dominant simulation engine

Packet receive and send processing already happens on separate workers, but those workers mostly feed packets into the universe or world pipelines.

They do not remove the fact that authoritative world mutation still happens in a mostly serial world update lane.

### 8.5 Practical conclusion

If the goal is to use more cores without changing gameplay behavior, the best near-term gains come from:

- improving cross-world parallelism
- keeping expensive non-simulation work off the universe thread
- moving world access to queue-based ownership
- parallelizing around the world tick before attempting to parallelize through the world tick

If the goal is to make one extremely busy world use many cores, that is a much harder problem and has to be approached carefully to avoid breaking mod-visible update order.

## 9. Compatibility Constraints For A Multicore Refactor

The safest threading strategy is the one that preserves the current mod-visible and gameplay-visible ordering semantics.

### 9.1 Preserve one authoritative owner per world

World state should still have one authoritative owner at any instant.

That means the target model should be:

- one world thread owns mutation for one world
- other threads send commands or consume snapshots
- no foreign thread directly mutates world state under shared locks

This improves concurrency without changing the basic server-authoritative model.

### 9.2 Preserve entity and Lua update order inside a world tick

The riskiest compatibility surface is the mod-visible order inside `WorldServer::update()`.

Today, entity updates and script updates execute in a consistent serial sequence inside the world tick. Many gameplay systems and mods may implicitly rely on that ordering even if the project never documented it as a strict contract.

For best compatibility, the default path should keep these serial for now:

- entity update order within a world tick
- world script context update order
- message handling order that is visible to scripts and entities
- packet application ordering at the world boundary

### 9.3 Preserve per-client packet ordering

Network worker improvements must not change the existing expectation that packets for a given connection are processed and emitted in a stable order.

This is especially important for:

- world start and stop transitions
- warp and flight flows
- entity create, delta, and destroy ordering
- script-driven responses that are observed by clients or other entities

### 9.4 Preserve current data formats and API surfaces first

The best first concurrency changes should avoid changing:

- packet formats
- save formats
- Lua callback signatures
- world and universe script callback timing semantics

Concurrency work should be as invisible to mods as possible until profiling proves that deeper semantic changes are necessary.

## 10. Draft: Compatibility-Preserving Multicore Architecture

This section outlines the highest-value path to better core utilization while keeping most gameplay and mod behavior in place.

### 10.1 Stage 1: Fix the ownership model before chasing more threads

The first structural step should be to turn `WorldServerThread` and `SystemWorldServerThread` into stricter actor-style owners.

Concretely:

- replace direct cross-thread mutation methods with queued commands
- replace synchronous `executeAction()` style access with mailbox requests and explicit replies
- let world and system-world threads publish outgoing packets and snapshots after each tick
- keep `UniverseServer` as coordinator, but stop letting it enter world state directly

This does not increase per-world parallelism yet, but it removes the biggest blocker to safe multicore expansion.

### 10.2 Stage 2: Push non-simulation work off the hot threads

The next gains should come from moving work that does not need to sit inside the authoritative simulation lane.

Good candidates:

- connection handshake state handling
- client-context serialization and file writes
- ship chunk serialization and snapshot persistence
- celestial request work
- packet compression or encoding work where ordering can be preserved
- maintenance scans that do not require live world mutation

This should reduce time spent on the universe thread and free world threads to spend more time on actual simulation.

### 10.3 Stage 3: Keep a serial mod-visible lane, parallelize around it

For a compatibility-first design, each world tick should still have one serial lane that remains the authoritative order for mod-visible simulation:

1. apply queued inbound packets and commands
2. run entity updates
3. run world script context updates
4. run immediate gameplay-visible mutations
5. commit authoritative state for the tick

Then, after that serial lane reaches a stable barrier, the server can parallelize tasks that are not supposed to change script-visible ordering.

The most promising candidates are:

- background snapshot building for persistence
- packet delta encoding from already-frozen state
- region and interest-set calculations derived from frozen state
- selected storage generation or unload analysis work

The key rule is that background tasks must read from a stable snapshot and merge results only at explicit barriers.

### 10.4 Stage 4: Improve multi-world scaling first

The easiest place to use more cores safely is across worlds, not inside one world.

That means prioritizing:

- cleaner world ownership
- lower-cost world-to-universe communication
- less lock contention when many worlds are active
- faster world create and unload paths

If the server hosts many active worlds, this can produce large multicore gains without changing world-internal semantics.

### 10.5 Stage 5: Add optional world-internal job systems only for low-risk subsystems

After the ownership and snapshot boundaries are clean, the project can consider optional internal job systems for world subsystems that are less likely to break mods.

The best candidates from the current code structure are:

- world storage generation queue work
- world storage unload analysis
- selected packet-diff or replication preparation work

Possible but higher-risk later candidates are:

- liquid processing
- falling-block processing
- wiring evaluation

These later candidates are riskier because they mutate simulation state directly and can interact with entity behavior, sector loading, or script-observed world state.

### 10.6 Stage 6: Treat single-world scaling as a separate project

Using many cores for one crowded world is possible, but it should be treated as a distinct, more invasive effort.

Why it is harder:

- entity updates are currently one ordered pass
- entity scripts and world scripts run in the world tick
- cross-entity interactions are common
- several world subsystems mutate shared state directly

A compatibility-first server should not try to parallelize those paths by default.

Instead, the project should:

- keep the default world simulation lane serial
- add measurement first
- consider experimental feature flags for deeper subsystem parallelism later

## 11. Recommended Draft Roadmap

### 11.1 Near-term, low-risk changes

- add profiling and timing counters for universe loop phases and world loop phases
- shard network workers without full connection-map rescans
- replace sleep polling with readiness-driven wakeups
- move handshake progression out of thread-per-connection flow
- move persistence writes and client-context serialization off the universe tick path

Expected result:

- better overall responsiveness
- lower scheduler waste
- more core usage outside the simulation lane
- minimal gameplay or mod compatibility risk

### 11.2 Medium-term, high-value structural changes

- convert world and system-world access to mailbox-style ownership
- remove foreign-thread direct entry into world state
- define stable snapshots for persistence and packet preparation

Expected result:

- better scaling across many active worlds
- cleaner reasoning about thread safety
- a safer base for future multicore work

### 11.3 Longer-term, optional experimental changes

- per-world subsystem job graphs for low-risk phases
- snapshot-based parallel replication encoding
- experimental parallel world-internal subsystems behind config flags

Expected result:

- better chance of using multiple cores even for a very busy world
- higher engineering risk
- higher mod compatibility risk unless rollout is tightly controlled

## 12. Direct Recommendation

If the target is maximum practical multicore usage with the best compatibility, the project should not begin by parallelizing entity updates or Lua execution.

The best first draft is:

1. keep one serial authoritative world tick per world
2. make world ownership strict and queue-based
3. move networking, handshakes, persistence, and snapshot work off the hot orchestration path
4. optimize scaling across many worlds first
5. introduce deeper world-internal parallelism only behind explicit experimental boundaries

That path will not instantly make one crowded world use every core, but it is the strongest route to higher multicore utilization without destabilizing the gameplay and mod ecosystem.