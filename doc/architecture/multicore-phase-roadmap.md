# OpenStarbound Multicore Phase Roadmap

This roadmap turns `multicore-engineering-plan.md` into an execution checklist. It keeps the same compatibility-first strategy: improve multicore use around the serial world simulation lane before attempting world-internal parallel simulation.

Phase 0A is implemented. Phase 0 has started with bounded universe-loop phase timing in `/serverstatus`, while world-thread and benchmark timing remain pending. Phase 1 worker-owned network connection lists and event wakeups are implemented in `source/game/StarUniverseConnection.*` and covered by focused `UniverseConnectionServer` tests. Phase 2 is implemented behind `usePendingConnectionStateMachine`, preserving the old thread-per-handshake path as a fallback, and now has focused state-machine success/protocol-rejection coverage. Phase 3 is partially implemented with immutable versioned persistence snapshots, synchronous shared write helpers, bounded opt-in async JSON persistence behind `useAsyncPersistence`, explicit system-world and ship-chunk snapshot boundaries, and focused async completion / queue-pressure fallback tests. Phase 4 has a world-thread command mailbox covering common synchronous wrappers, admin/RPC world actions, weather, dungeon placement, and flying-sky transitions. Phase 5 has started with owner-indexed entity update application plus per-tick monitoring-region reuse and cached generation-priority distance calculations.

The observability and crash-reporting work that supports these phases is tracked in `diagnostics-debugging-roadmap.md`. In short: build on the current `/debug` overlay, `LogMap`, `SpatialLogger`, `Logger`, stack traces, Lua profiles, and `/servernetstats` to provide F3-style status, server diagnostic commands, crash bundles, and structured logs.

## Current Status After Review

- Phase 0A render-rate decoupling and the user-facing VSync / max-FPS controls are implemented and wired through the client configuration, graphics menu, and debug HUD.
- Phase 0 remains incomplete, but bounded universe-loop phase timing now reports average, p50, p95, p99, and max microseconds through `/serverstatus`; world-thread timing, world subphase timing, and the `world_benchmark` measurement path described below are still roadmap work.
- Phase 1 is implemented and validated by focused worker-ownership, many-idle-connection, wakeup, remove-during-callback, and cross-worker packet-ordering tests, but broader TCP stress and readiness-abstraction follow-up items are still open.
- Phase 2 is implemented and live behind its fallback flag. Focused tests cover local state-machine success and protocol mismatch rejection; timeout, password, duplicate UUID, asset mismatch, max-player, and login-burst matrix cases remain open.
- Phase 3 has its current compatibility-first slice in place: immutable universe/client/system snapshots, bounded async JSON persistence, synchronous fallback on queue pressure, retry accounting, shutdown draining, and server diagnostics. Focused tests now cover async triggered-storage completion and queue-full synchronous fallback; save/load depth, failure injection, and broader latency validation remain open before default enablement.
- Phase 4 now routes most external `WorldServerThread` entry points through named commands while the world thread is active: client add/remove, spawn checks, revive position, new planet type, weather list/set, dungeon placement, flying-sky start/stop, container item RPC, universe flag RPC, admin/scripted `executeForClient`, unload, and chunk reads. `/serverstatus` exposes aggregate world-command pending, processed, direct, failed, and wait-time counters. The ship-upgrade mutation block and system-world direct mutations remain to be retired in later batches.
- Phase 5 has non-parallel cleanup in place: client-owned/slave entity ids are indexed by owner connection, `WorldServer::update()` computes per-client monitoring regions once per tick and reuses them for liquid no-processing regions, sector signaling, and packet preparation, and world generation sorting memoizes nearest-player sector distances. Blank entity-update deltas are still delivered to every indexed entity for that owner, preserving interpolation/extrapolation behavior.

## Research Notes

Relevant guidance from C++ and Windows threading references maps cleanly onto the current server problems:

- Prefer waitable work queues or condition variables over short sleep polling. The current network workers used a 1 ms sleep loop, so wakeable worker state is the right first step.
- Avoid high thread churn for short or bursty work. Windows thread-pool guidance specifically calls out reducing thread creation/destruction overhead; this supports replacing one thread per pending handshake with a state machine or bounded executor.
- Keep long blocking operations off latency-sensitive orchestration threads. Disk flush, compression, serialization, and slow handshakes should not sit directly in the universe update lane.
- Do not solve scheduler contention with high thread priority. Windows scheduling guidance recommends brief high-priority work only; server throughput should come from ownership, queues, batching, and reduced contention.
- Preserve per-connection and owner-thread ordering. For this codebase, that means connection packets stay ordered per client and `WorldServer` mutation stays on the owning world thread unless a snapshot boundary exists.
- For game-loop ownership, use single-reader command queues rather than global event buses. Queued commands should carry the data they need because delayed processing must not assume the current mutable world still matches the sender's original context.
- For snapshot/post-tick work, double-buffer style boundaries are the right mental model: readers should see a complete current snapshot while the owner thread prepares the next one, and swaps/merges must stay explicit and cheap.

Sources consulted:

- Microsoft Learn, Windows thread pools: `https://learn.microsoft.com/en-us/windows/win32/procthread/thread-pools`
- Microsoft Learn, Windows scheduling priorities: `https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities`
- C++ condition-variable model: `std::condition_variable` behavior and predicate-based waiting patterns
- Game Programming Patterns, Event Queue: `https://gameprogrammingpatterns.com/event-queue.html`
- Game Programming Patterns, Double Buffer: `https://gameprogrammingpatterns.com/double-buffer.html`

## Windows Tooling Baseline

Current host status checked on Windows:

| Tool | Status | Notes |
| --- | --- | --- |
| CMake | Installed | `C:\Program Files\CMake\bin\cmake.exe` |
| Ninja | Installed | Found under the Python scripts directory |
| Git | Installed | `C:\Program Files\Git\cmd\git.exe` |
| Visual Studio Build Tools | Installed | VS 2022 Build Tools detected |
| vcpkg | Installed during this pass | `C:\src\vcpkg`; `VCPKG_ROOT` persisted for the user environment |
| MSVC `cl` | Not in current shell PATH | Usually available after loading the VS developer environment |

Build preparation tasks:

1. Install or locate vcpkg, preferably at a short path such as `C:\src\vcpkg`.
2. Set `VCPKG_ROOT` for the user or current shell.
3. Load the VS 2022 developer environment before command-line builds if `cl` is not found.
4. Configure from `source/` with `cmake --preset=windows-release`.
5. Build with `cmake --build --preset=windows-release`.
6. Run no-asset tests with `ctest --preset=windows-release` once configure succeeds.

Recommended Windows profiling tools:

- Visual Studio Profiler for CPU sampling and thread timelines.
- Windows Performance Recorder and Windows Performance Analyzer for scheduler, context-switch, CPU, disk, and networking traces.
- Windows Performance Counters for process/thread CPU and disk queue baselines.
- OpenStarbound internal timing counters for phase comparisons before and after changes.

## Diagnostics And Debugging Track

This track runs beside every multicore phase. It should provide the information needed to debug crashes, verify behavior, and prove performance changes.

Existing building blocks:

- `Logger` and `FileLogSink` for text logs.
- `LogMap` for high-frequency debug key/value values shown in the client overlay.
- `SpatialLogger` for debug geometry and text overlays.
- `/debug` and `/debug hud` for client overlay control.
- `StarException`, `fatalError`, and `fatalException` for stack traces and fatal reporting.
- Lua profile dumps.
- `/servernetstats` for Phase 1 network worker profiling.
- Separate client render FPS and update-rate reporting for verifying render/update decoupling.

Planned capabilities:

1. F3-style client status pages for basic player/world/server state, renderer state, network state, Lua/mod state, and performance counters.
2. Server-side `/serverstatus`, `/worldstats`, `/serverprofile`, and `/dumpdiag` commands.
3. Crash report bundles containing build/platform data, exception and stack trace, recent logs, recent diagnostic events, asset/mod context, and redacted configuration context.
4. Structured logging categories, bounded recent-log retention, log rotation, optional JSON-lines output, and repeated-warning rate limiting.
5. Redaction of secrets, auth tokens, passwords, private platform ids, and full IP addresses in any copy, dump, or crash path.

Acceptance criteria:

- Diagnostics are opt-in or bounded by config and do not become the bottleneck.
- Crash reports are written to a predictable directory and referenced in logs/dialogs.
- The overlay and server commands expose the metrics required by each multicore phase.
- Diagnostic snapshots are machine-readable JSON as well as human-readable command output.

## Phase 0A: Client Render FPS Unlock

Goal: allow high-refresh or uncapped client rendering without changing simulation, Lua, physics, network, or server tick timing.

Code areas:

- `source/application/StarApplicationController.hpp`
- `source/application/StarMainApplication_sdl.cpp`
- `source/client/StarClientApplication.cpp`

Implementation tasks:

1. Add a client `renderFrameRate` setting separate from `updateRate`.
2. Keep `updateRate` mapped to `GlobalTimestep` for compatibility-sensitive simulation behavior.
3. Let `renderFrameRate: 0` disable the software render limiter, while VSync still caps presentation if enabled.
4. Keep default rendering at 60 FPS unless the player opts into a higher value or uncapped rendering.
5. Show both render FPS and update Hz in debug diagnostics.

Acceptance criteria:

- Default config remains near 60 FPS and 60 Hz update.
- Raising or disabling `renderFrameRate` can exceed 60 FPS without increasing update Hz.
- Movement, Lua timers, packet cadence, and local server tick behavior do not speed up from render-only changes.

Primary risks:

- Some rendering paths may assume one render follows every update.
- Uncapped rendering can increase GPU/CPU use when VSync is disabled.

## Phase 0: Measurement Foundation

Goal: prove the hot paths before deeper scheduling changes.

Code areas:

- `source/game/StarUniverseServer.cpp`
- `source/game/StarWorldServerThread.cpp`
- `source/game/StarWorldServer.cpp`
- `source/game/StarUniverseConnection.cpp`
- `source/utility/world_benchmark.cpp`

Implementation tasks:

1. Add a lightweight timing accumulator type that can be compiled into release-with-debug-info builds without excessive allocation.
2. Add universe-loop phase timers around Lua update, chat, teams, ship update, warp/fly/arrive, celestial response, broken-world cleanup, world messages, inactive-world shutdown, and triggered storage.
3. Add world-thread timers around incoming packets, world update, message handling, outgoing packet collection, update callbacks, and sync.
4. Add world-update subphase timers for entity update, world scripts, damage, wiring, weather, liquid, falling blocks, storage tick, storage generation, and packet preparation.
5. Add network counters for worker wakeups, idle wakeups, owned connections, scanned connections, packets processed, and callback duration.
6. Extend `world_benchmark` to print subphase timing summaries and percentiles.
7. Feed selected counters into the diagnostics registry and F3-style client/server status surfaces.
8. Add crash-report context hooks for current phase timers, network worker stats, active worlds, and storage state.

Acceptance criteria:

- A dedicated server run can emit per-phase timing summaries without changing packet or save formats.
- Measurements include p50, p95, and p99 tick durations where practical.
- Measurement overhead is visible and bounded under a config flag.
- Client overlay, server commands, logs, and crash reports can all consume the same diagnostic snapshots.

Primary risks:

- Logging too frequently can become the bottleneck.
- Timers placed inside entity/Lua inner loops can distort the profile.

Go/no-go:

- Continue when metrics identify whether the server is network-bound, storage-bound, universe-loop-bound, or one-world-update-bound.

## Phase 1: Network Worker Sharding And Wakeups

Goal: reduce idle CPU and high-connection bookkeeping without altering packet semantics.

Code areas:

- `source/game/StarUniverseConnection.hpp`
- `source/game/StarUniverseConnection.cpp`
- later: `source/core/` socket poller abstraction

Implementation tasks:

1. Give each network worker explicit ownership of assigned connection IDs.
2. Stop each worker from copying and filtering the full global connection map every loop.
3. Wake the owning worker when a connection is added, removed, or has packets queued for send.
4. Keep the current timed polling fallback until a portable socket readiness abstraction exists.
5. Add worker counters for wakeups, idle timed wakeups, owned connection count, processed packets, and callback time.
6. Design a narrow `SocketPoller` abstraction for Windows, Linux, and macOS.
7. Add Windows implementation using a conservative readiness API first, then evaluate IOCP separately as a larger networking project.
8. Add Linux `epoll` and macOS `kqueue` implementations only after the fallback path is stable.

Started work:

- `UniverseConnectionServer` now has worker-owned connection lists.
- Worker loops now copy only their assigned connection IDs.
- Add, remove, and send paths wake the owning worker through `ConditionVariable`.
- Worker scan, stale-id, callback, wakeup, timed-wait, and idle-wait counters are available through `UniverseConnectionServer::workerStats()`.
- `/servernetstats` exposes the worker counters for live admin profiling.
- `/serverstatus` exposes a compact server diagnostics snapshot with uptime, player counts, active worlds, pending queue sizes, TCP state, and aggregate network counters.
- Per-connection packet callback ordering remains owned by one worker.

Remaining Phase 1 work:

1. Add a many-idle-connections stress case that records `ownedConnections`, `connectionScans`, `wakeups`, `timedWaits`, and `idleTimedWaits` before and after a small active packet burst.
2. Add packet-ordering tests with at least two clients on different workers and repeated small packet bursts beyond the existing local/TCP echo coverage.
3. Expose byte counters if the packet socket layer can provide them without allocation-heavy sampling.
4. Measure whether `sendPackets` should remain an eager nonblocking write path or become queue-only plus wakeup under high fan-out broadcast load.
5. Keep the timed fallback at 1 ms until socket readiness has cross-platform coverage and failure-mode tests.
6. Document the expected worker-stat interpretation so operators can tell a healthy idle server from lost-wakeup latency.

Recommended Phase 1 implementation sequence:

1. Add test scaffolding around `LocalPacketSocket::openPair()` and `UniverseConnectionServer` packet callbacks.
2. Cover add, send, receive, remove, and shutdown with one worker, then with multiple workers.
3. Add a many-idle-connections stress case that records `ownedConnections`, `connectionScans`, `wakeups`, `timedWaits`, and `idleTimedWaits` before and after a small active packet burst.
4. Add a disconnect-during-callback case where the callback removes the active connection and then another packet is queued or attempted.
5. Only after those pass, prototype `SocketPoller` behind a config flag or compile-time path while keeping the condition-variable fallback as the default.

Phase 1 diagnostics to keep or add:

- `/servernetstats` should show per-worker owned connections, last handled connections, scans, stale scans, packets, callbacks, average callback time, wakeups, timed waits, and idle waits.
- `/serverstatus` should keep aggregate network worker counts and pending accept counts visible while Phase 2 is being developed.
- A healthy worker-owned loop should have `connectionScans` scale with owned connections per worker, not worker count times total connections.
- After a quiet period, a queued send should increase the owning worker's wakeup count and should not require waiting for a long polling timeout.

Phase 1 go/no-go for Phase 2:

- No known packet ordering regression with multiple clients.
- No stale-worker-list growth after repeated add/remove/disconnect stress.
- No deadlock in shutdown or `removeAllConnections` while workers are active.
- Idle CPU and idle wait behavior are measured on at least one Windows run and one Linux or Wine/proton-equivalent server run if available.
- Existing local single-player hosting and remote TCP multiplayer still connect, exchange packets, and disconnect cleanly.

Acceptance criteria:

- Packet ordering is unchanged per client.
- Idle network CPU drops under many idle connections.
- High-connection worker bookkeeping no longer scales as worker-count times total-connections.
- Local and TCP connections still connect, exchange packets, and disconnect cleanly.

Primary risks:

- Lost wakeups can add latency if the fallback wait grows later.
- Remove/reconnect races can leave stale IDs in worker lists if ownership cleanup is incomplete.
- Calling packet callbacks concurrently for one client would violate the existing contract; worker ownership must prevent that.

Tests:

- Multi-client packet ordering.
- Add/remove during active worker loops.
- Send queue wakeup for idle worker.
- Disconnect while callback is active.
- Many idle TCP clients with a small number of active clients.

## Phase 2: Handshake State Machine

Goal: remove one thread per pending handshake and reduce login-burst scheduler pressure.

Code areas:

- `source/game/StarUniverseServer.cpp`
- `source/game/StarUniverseServer.hpp`
- `source/game/StarNetPackets.hpp`
- possibly `source/game/StarUniverseConnection.*`

Implementation tasks:

1. Introduce `PendingConnection` with explicit states: await protocol request, send protocol response, await client connect, await password challenge response, finalize, reject, dead.
2. Store deadlines per pending connection instead of blocking a thread in `receiveAny(clientWaitLimit)`.
3. Drive pending handshakes from the universe loop or a bounded handshake executor.
4. Preserve the existing packet sequence exactly.
5. Keep final client-context registration under the universe ownership path.
6. Move slow failure flushes to a bounded path that cannot create unbounded detached work.

Current blocking flow to preserve:

1. `addClient` or the TCP accept callback creates one `Thread::invoke("UniverseServer::acceptConnection", ...)` task.
2. The server blocks for `ProtocolRequestPacket`, replies with `ProtocolResponsePacket`, and enables the compression stream only after that response is sent.
3. The server blocks for `ClientConnectPacket`, validates protocol, asset digest, ship species, account settings, password requirements, anonymous login settings, and bans.
4. If the account is named, the server sends `HandshakeChallengePacket`, waits for `HandshakeResponsePacket`, and compares the salted hash.
5. The server computes `NetCompatibilityRules`, handles duplicate UUID priority, enforces max players, loads client context, adds the connection to `UniverseConnectionServer`, sends `ConnectSuccessPacket`, and schedules the initial warp/fly state.
6. Failure paths send `ConnectFailurePacket` where possible and retain the connection in `m_deadConnections` long enough to flush pending data.

Proposed `PendingConnection` data:

- stable pending id for diagnostics, separate from final `ConnectionId`
- `UniverseConnection` and optional remote address
- current state and state deadline in monotonic milliseconds
- pending `ClientConnectPacket`, account string, administrator flag, password salt, legacy-client flag, compression-stream flag, and accumulated failure reason
- cached remote address string for logging without recomputing it in every state

State machine sketch:

| State | Nonblocking work | Next state |
| --- | --- | --- |
| `AwaitProtocolRequest` | call `receive()`, read `ProtocolRequestPacket`, reject missing or bad packets at deadline | `SendProtocolResponse` or `RejectAndFlush` |
| `SendProtocolResponse` | queue/write `ProtocolResponsePacket`; unsupported protocol sets `allowed=false` | `EnableCompression` or `RejectAndFlush` |
| `EnableCompression` | enable zstd stream on `CompressedPacketSocket` after the protocol response has been flushed or at the same point as current behavior | `AwaitClientConnect` |
| `AwaitClientConnect` | call `receive()`, read `ClientConnectPacket`, reject timeout or unexpected packet | `ValidateClientConnect` |
| `ValidateClientConnect` | perform asset, species, account, anonymous, ban, max-version, and compatibility checks | `AwaitHandshakeResponse`, `FinalizeClient`, or `RejectAndFlush` |
| `AwaitHandshakeResponse` | send challenge once, then call `receive()` until `HandshakeResponsePacket` or deadline | `FinalizeClient` or `RejectAndFlush` |
| `FinalizeClient` | under universe/client ownership, allocate `ConnectionId`, load context, register RPC handlers, add to `UniverseConnectionServer`, send success packets, schedule initial world/system placement | `Dead` |
| `RejectAndFlush` | queue `ConnectFailurePacket` or final protocol response, call nonblocking `send()`, keep the connection until sent or deadline | `Dead` |

Recommended Phase 2 implementation sequence:

1. Add `PendingConnection` and `PendingConnectionState` to `UniverseServer` while keeping the old `acceptConnection` path compiled and callable.
2. Add `m_pendingConnections`, pending counters, and `processPendingConnections()` called from the universe loop near `reapConnections()`.
3. Change TCP accept and `addClient` to enqueue pending connections behind a temporary config flag, leaving the thread-per-handshake path available as a fallback.
4. Port protocol response handling first, including unsupported protocol failure and compression-mode negotiation.
5. Port `ClientConnectPacket` validation without changing messages or packet order.
6. Port password challenge and failure handling, preserving the same missing-account versus bad-password message.
7. Port finalization as a narrow helper that runs on the universe thread and reuses existing duplicate UUID, max player, client context loading, system-world placement, revive warp, and script callback logic.
8. Remove or disable the old accept-thread list only after local, TCP, legacy, OpenStarbound, password, timeout, and duplicate UUID tests pass.

Phase 2 diagnostics to add:

- pending handshake count by state in `/serverstatus`
- accepted, finalized, rejected, and timed-out counters in `/serverstatus`
- dropped-pending counters
- oldest pending handshake age and maximum pending deadline overrun
- failure reason buckets for protocol mismatch, timeout, asset mismatch, anonymous disabled, auth failed, ban, duplicate UUID, max players, and unexpected packet

Phase 2 compatibility checkpoints:

- Packet order stays byte-for-byte equivalent at the packet type level for success, password challenge, and common failure paths.
- `CompressedPacketSocket::setCompressionStreamEnabled` happens at the same protocol boundary as today.
- `m_connectionServer->addConnection` is still called only after successful authentication and final client context creation.
- `m_clients`, `m_chatProcessor`, team RPC registration, system-world placement, intro/revive warp scheduling, `clientFlyShip`, `ServerInfoPacket`, and Lua `acceptConnection` callback remain on the universe ownership path.
- Failure connections remain alive long enough for `ConnectFailurePacket` or disallowed `ProtocolResponsePacket` to flush without a detached thread.

Acceptance criteria:

- Login success, failure, timeout, password challenge, asset mismatch, and protocol mismatch behavior match current behavior.
- Slow clients no longer reserve a full thread until timeout.
- `maxPendingConnections` remains an upper bound for memory and work.
- `/serverstatus` reports pending handshakes by state so connection bursts are visible.

Primary risks:

- Timing changes can expose assumptions in client connection code.
- Compression stream enablement must occur at the same protocol point.
- Duplicate UUID resolution still needs safe interaction with the clients map.
- Universe-loop driven polling can add one main-loop interval of handshake latency unless the accept path wakes the server loop.
- Moving finalization into smaller helpers can accidentally change login failure messages or script callback timing.

Tests:

- Covered by focused tests: local state-machine success and protocol mismatch rejection.
- Legacy and OpenStarbound handshakes.
- Timeout in every pending state.
- Password success and failure.
- Duplicate UUID as admin and non-admin.
- Login burst with deliberately slow clients.
- Protocol mismatch flushes a disallowed `ProtocolResponsePacket` before closing.
- Asset mismatch tests cover both server refusal and client-side `allowAssetsMismatch` refusal.
- Max-player refusal still allows administrator priority.
- Local single-player connection still succeeds without requiring TCP-only code paths.

## Phase 3: Async Persistence And Snapshot Writes

Goal: move serialization, compression, and disk write latency off universe and world hot paths.

Code areas:

- `source/game/StarUniverseServer.cpp`
- `source/game/StarServerClientContext.*`
- `source/game/StarWorldServerThread.*`
- `source/game/StarSystemWorldServerThread.*`
- `source/game/StarWorldStorage.*`
- `source/game/StarPlayerStorage.*`
- `source/game/StarCelestialDatabase.*`
- `source/core/StarBTreeDatabase.*`
- `source/core/StarWorkerPool.*`

Implementation tasks:

1. Define immutable snapshot types for client context, ship chunks, universe settings, temp world indexes, and celestial commit requests.
2. Add a bounded persistence executor with queue-depth metrics and shutdown flushing.
3. Produce world snapshots on the world owner thread at safe barriers.
4. Produce client and universe snapshots on the universe thread.
5. Write snapshots on persistence workers.
6. Report completion and failures back to the universe thread.
7. Preserve required shutdown and disconnect flush semantics.
8. Add retry or fatal-failure policy before moving critical writes fully async.

Current synchronous flow to preserve:

1. `UniverseServer::doTriggeredStorage()` saves `universe.dat`, saves `tempworlds`, reads each active ship world through `WorldServerThread::readChunks()`, serializes `ServerClientContext::storeServerData()`, writes each `.clientcontext`, and then calls `CelestialMasterDatabase::cleanupAndCommit()`.
2. `WorldServerThread::run()` periodically calls `sync()`, which locks the world and calls `WorldServer::sync()` / `WorldStorage::sync()`.
3. `SystemWorldServerThread::run()` periodically calls `store()`, builds `SystemWorldServer::diskStore()`, and writes a versioned `System` JSON file.
4. Ship-world chunk storage currently uses full chunk snapshots and `WorldStorage::applyWorldChunksUpdateToFile()` for incremental file updates.
5. Shutdown and disconnect paths assume required client, ship, and world state is durable before ownership disappears.

Proposed persistence job types:

- `WriteUniverseSettingsJob`: versioned `UniverseSettings` JSON and target path
- `WriteTempWorldIndexJob`: serialized temporary-world index and target path
- `WriteClientContextJob`: player UUID, serialized `ClientContext`, target path, and optional ship-chunk update metadata
- `WriteShipChunksJob`: shipworld path plus `WorldChunks` update or full snapshot
- `WriteSystemWorldJob`: system location, versioned `System` JSON, and target path
- `CommitCelestialDatabaseJob`: bounded request or completion marker for celestial commit work if it can be separated safely
- `WorldStorageSyncJob`: pre-serialized sector/database updates only; never a live `WorldStorage*`

Started work:

- `VersionedJsonStorageSnapshot` now covers `UniverseSettings`, `TempWorldIndex`, and `ClientContext` writes through one snapshot/write path.
- `UniverseServer::doTriggeredStorage()` builds immutable snapshots first, then persists them through a shared helper before celestial cleanup/commit.
- `useAsyncPersistence` enables a dedicated `UniverseServerPersistencePool`; it is disabled by default while the path is validated.
- `maxQueuedPersistenceSnapshots` bounds queued snapshot count. When the queue is full, the server falls back to synchronous writes instead of dropping required state.
- `maxPersistenceWriteRetries` adds opt-in bounded write retries; it defaults to `0` to preserve current write behavior while reporting retry counts when enabled.
- `ServerClientContext::ShipChunksSnapshot` now captures immutable full-chunk and delta data at the `readChunks()` boundary before the context is mutated, while keeping the existing ship-update path unchanged.
- `SystemWorldServerThread::store()` now has a system-world `System` JSON snapshot boundary: it serializes immutable versioned data first, then writes that snapshot synchronously.
- `UniverseServer::doTriggeredStorage()` now runs celestial cleanup/commit outside the universe locks, and `/serverstatus` reports accumulated celestial commit time and count so the remaining synchronous hot path stays visible.
- Shutdown drains pending async persistence writes before final synchronous universe and temp-world index saves, preventing older queued autosaves from overwriting shutdown state.
- `/serverstatus` reports pending persistence batches, pending snapshots, oldest pending age, completed batches, written snapshots, build/write time, failures, retry attempts, synchronous fallbacks, and queue-full fallbacks.

Recommended Phase 3 implementation sequence:

1. Extend the current versioned snapshot path to include any remaining universe-owned metadata writes that can be serialized before dispatch.
2. Validate `useAsyncPersistence` under save/load and shutdown tests before enabling it by default.
3. Add failure-injection or filesystem-denial tests that prove retries, failures, and queue-full fallbacks are reported.
4. Decide whether ship chunk snapshots should remain a transport-only boundary or gain a server-side write path for local-host durability work.
5. Move client context and universe settings writes to the executor first, because their snapshots are already plain versioned JSON.
6. Move ship chunk update writes after proving that `readChunks()` or a future `readChunkUpdate()` happens only on the world owner boundary.
7. Move system-world storage after adding result reporting and making `SystemWorldServerThread::store()` produce immutable JSON before enqueueing work.
8. Keep world database `sync()` on the world thread until sector-level serialized updates can be produced without exposing live `WorldStorage` or `BTreeDatabase` to background workers.
9. Add shutdown draining that waits for required jobs, reports failed optional jobs, and preserves the current durable-exit behavior.

Phase 3 diagnostics to add:

- persistence queue depth, oldest queued age, completed jobs, failed jobs, retry count, and bytes written by job type
- time spent snapshotting on owner threads versus time spent writing on persistence workers
- per-world sync duration and per-system store duration
- crash-report context for pending required persistence jobs

Phase 3 compatibility checkpoints:

- Background workers receive only immutable data, never `ServerClientContextPtr`, `WorldServerThreadPtr`, `WorldStorage*`, or live entity/world pointers.
- File formats and versioning labels stay unchanged: `UniverseSettings`, `ClientContext`, `System`, and world chunk database records must load through the existing versioning paths.
- Write ordering that matters for shutdown and disconnect is either preserved or explicitly flushed before returning.
- Failure handling is visible in logs and diagnostics; required state is not silently dropped when a worker throws.
- Bounded queues apply backpressure instead of unbounded memory growth during save storms.

Acceptance criteria:

- Save files remain byte-compatible or semantically compatible with current versioned data.
- Shutdown waits for required writes.
- Simulated write failures are visible and do not silently drop required state.
- Autosave and disconnect p95/p99 spikes improve or stay stable.
- `/serverstatus` or a nearby diagnostics command reports persistence queue health.

Focused tests now cover opt-in async triggered-storage completion, creation of `universe.dat` and `tempworlds.index`, and bounded-queue synchronous fallback when the async queue is full.

Primary risks:

- Writing live mutable objects from background threads would introduce races.
- Reordering critical writes can change crash-recovery behavior.
- Unbounded persistence queues can trade tick spikes for memory growth.
- Moving `BTreeDatabase::commit()` off-thread before serialized sector updates exist can corrupt ownership assumptions.

Tests:

- Normal save/load.
- Disconnect while save is pending.
- Ship upgrade while save is pending.
- Shutdown while writes are queued.
- Simulated write failure.
- Old save load after async-written state.
- System-world flight and arrival state survives restart after async store.
- Persistence queue saturation uses backpressure and does not drop required client context writes.

## Phase 4: Strict World Mailbox Ownership

Goal: make world threads true mutation owners and reduce direct cross-thread world locking.

Code areas:

- `source/game/StarWorldServerThread.*`
- `source/game/StarSystemWorldServerThread.*`
- `source/game/StarUniverseServer.cpp`
- `source/game/StarServerClientContext.*`

Implementation tasks:

1. Add typed world commands for add/remove client, pause, read chunks snapshot, spawn-target check, messages, ship properties, flying sky, sync, and unload.
2. Add reply promises for commands that need a result.
3. Drain commands at explicit tick boundaries before packet handling or before world update, depending on current behavior requirements.
4. Convert `addClient` and `removeClient` first because they are high-value and easy to test.
5. Replace generic `executeAction()` call sites with typed commands in small batches.
6. Convert system-world mutation paths to equivalent commands.
7. Add queue-depth and command-latency metrics.

Started work:

- `WorldServerThread` now has a command mailbox drained at the beginning of the owner-thread update tick.
- Synchronous wrappers for `spawnTargetValid`, `addClient`, `removeClient`, `playerRevivePosition`, `pullNewPlanetType`, `unloadAll`, and `readChunks` enqueue commands when the world thread is running and fall back to direct execution before start or after stop.
- Named commands also cover pause propagation, weather list/set, dungeon placement, flying-sky start/stop, container item RPC insertion, universe flag RPC mutation, and the admin/scripted `executeForClient` helper.
- Command waiters are released with a failure if the world thread exits before their command runs.
- Per-thread command counters exist in `WorldServerThread::CommandStats` and aggregate pending, processed, direct, failed, and wait-time values are visible in `/serverstatus`.

Remaining Phase 4 work:

1. Replace the remaining ship-upgrade `executeAction()` block with a purpose-built command/result path that preserves species template lookup, ship chunk snapshot updates, and lock ordering without copying large template maps every tick.
2. Convert system-world add/remove client ship paths to commands and publish read snapshots for ship location, sky, warp action, and active instance worlds.
3. Add oldest command age and per-world command details to `/worldstats` once that command exists.
4. Add focused tests for command failure propagation, disconnect during queued world work, ship upgrade commands, and admin/RPC behaviors now routed through named commands.

Current direct world-entry points to retire or wrap:

- `WorldServerThread::spawnTargetValid()`
- `WorldServerThread::addClient()`
- `WorldServerThread::removeClient()`
- `WorldServerThread::playerRevivePosition()`
- `WorldServerThread::pullNewPlanetType()`
- `WorldServerThread::executeAction()`
- `WorldServerThread::unloadAll()`
- `WorldServerThread::readChunks()`
- `WorldServerThread::sync()`
- the ship-upgrade block in `UniverseServer::updateShips()` that still calls `executeAction()` while coordinating client context snapshots

Current system-world entry points to normalize:

- `SystemWorldServerThread::addClient()` and `removeClient()` mutate live system-world state under a write lock.
- `setClientDestination()`, `executeClientShipAction()`, and `pushIncomingPacket()` already queue work and should become the model for the remaining methods.
- `clientShipLocation()`, `clientWarpAction()`, `clientSkyParameters()`, and `activeInstanceWorlds()` should read published snapshots instead of locking live state where possible.

Recommended Phase 4 implementation sequence:

1. Add a command queue and result queue to `WorldServerThread`, but initially keep the old public methods as synchronous wrappers around commands.
2. Add published world snapshots for client ids, expiration state, new planet type, player revive position, and ship chunks where synchronous callers need read results.
3. Convert `spawnTargetValid`, `playerRevivePosition`, and `readChunks` first because they are easy to compare against existing behavior.
4. Convert `addClient` and `removeClient`, preserving the current outgoing packet return path and chat channel join/leave ordering in `UniverseServer::warpPlayers()` and `doDisconnection()`.
5. Replace `executeAction()` call sites with named commands for weather, flying sky start/stop, container item RPC, universe flag RPC, ship properties, admin actions, and scripted world actions.
6. Move `sync()` and `unloadAll()` to owner-thread commands with completion results so shutdown and storage phases can wait deliberately.
7. Bring `SystemWorldServerThread` into the same model by moving add/remove client ship into queued commands and publishing read snapshots after each update.
8. Remove the old direct-entry wrappers only after command latency, queue depth, and error propagation are visible in diagnostics.

Phase 4 diagnostics to add:

- per-world command queue depth, oldest command age, processed command count, and failed command count
- time waiting for command replies from the universe thread
- number of remaining direct `executeAction()` calls by category during rollout
- world-thread update time split into command drain, packet handling, simulation, messages, outgoing packet collection, and update callbacks

Phase 4 compatibility checkpoints:

- Commands that return packets must preserve packet order relative to client context updates and warp result packets.
- Reply-waiting from `UniverseServer` must not happen while holding locks that the world command needs to complete.
- Client world and system-world references in `ServerClientContext` remain consistent while add/remove commands are in flight.
- Lua/admin behavior currently routed through `executeAction()` must receive purpose-built commands before the generic path is removed.

Acceptance criteria:

- World state mutation from outside the owner thread is removed or explicitly isolated.
- Common warps, beam up/down, ship travel, disconnect, and idle unload behave the same.
- Command queueing does not introduce unbounded one-tick delays in user-visible flows.
- Remaining direct world-lock entry points are documented and temporary.

Primary risks:

- Moving work to tick boundaries can shift behavior by a tick.
- Generic `executeAction()` may hide script/admin behaviors that need purpose-built commands.
- Blocking promises can recreate lock coupling if used from the wrong thread.
- Synchronous compatibility wrappers can hide deadlocks unless lock ordering is tested aggressively.

Tests:

- Planet, ship, party ship, and instance warps.
- Disconnect during world transition.
- Idle world shutdown.
- Admin commands that touch world/player state.
- Entity messages with replies.
- Container item RPC and universe flag RPC still affect the intended world state.
- Command queue saturation and world-thread exception propagation.

## Phase 5: Snapshot-Based Post-Tick Parallel Work

Goal: use extra cores around the serial simulation lane without changing Lua-visible update order.

Code areas:

- `source/game/StarWorldServer.cpp`
- `source/game/StarWorldServerThread.cpp`
- packet replication code paths
- world storage snapshot paths

Implementation tasks:

1. Define stable post-tick snapshots for data needed by packet encoding, region interest sets, metrics, and persistence.
2. Add a world-local post-tick job stage after authoritative update.
3. Parallelize low-risk read-only work first: packet delta preparation, region calculations, metrics, and persistence serialization.
4. Merge job results only at explicit barriers.
5. Compare packet streams before and after for deterministic scenarios.
6. Keep entity, Lua, liquid, wiring, and direct tile mutation serial by default.

Current serial work to split carefully:

- `WorldServer::handleIncomingPackets()` now applies `EntityUpdateSetPacket` by iterating an owner-indexed set of client-master entity ids instead of scanning all entities. This keeps blank-delta delivery for interpolation while bounding work to the packet owner.
- `WorldClient::handleIncomingPackets()` mirrors that owner-indexed receive path for slaved entities.
- `WorldServer::update()` rebuilds per-client monitoring regions for weather/liquid, sector signaling, and packet generation.
- `WorldServer::queueUpdatePackets()` serializes tile updates, liquid updates, entity creates, entity deltas, and destroy packets for each client.
- `m_netStateCache` already caches repeated entity delta serialization within one tick, but first-observation entity create payloads are not cached the same way.
- `WorldStorage::generateQueue()` can sort queued sectors using a comparator that recomputes nearest-player distance repeatedly.

Started work:

- `WorldServer::ClientInfo` tracks `clientMasterEntities`, populated from legal `EntityCreatePacket` ids and cleaned during entity removal and client removal.
- `WorldClient` tracks slave entity ids by `ConnectionId` for incoming `EntityUpdateSetPacket` application.
- Packet update receive paths still pass empty deltas for owner entities without payload entries, preserving `NetElementTop::blankNetDelta()` behavior when interpolation is enabled.
- `WorldServer::update()` now caches each client's monitoring regions once per tick and reuses them for liquid no-processing regions, sector signaling, and packet preparation.
- `WorldStorage::generateQueue()` ordering now memoizes each queued sector's nearest-player distance during sorting instead of recomputing it for every comparator call.

Remaining Phase 5 work:

1. Extend monitoring-region reuse into a formal `WorldTickSnapshot` that also captures monitored entity ids, client net rules, pending tile/liquid updates, and immutable entity serialization inputs.
2. Add serial packet-equivalence tests before moving packet preparation onto worker jobs.
3. Surface packet-prep split timings, monitoring-region build counts, generation-priority cache counts, and owner-index hit/miss counters in diagnostics.

Recommended Phase 5 implementation sequence:

1. First do non-parallel algorithmic cleanups that preserve behavior: apply inbound entity updates through owner-indexed entity sets, compute monitoring regions once per client per tick, and precompute generation queue priorities before sorting.
2. Add a `WorldTickSnapshot` containing current step/time, per-client monitoring regions, monitored entity ids, client net rules, pending tile/liquid/damage update lists, and immutable entity serialization inputs.
3. Add create-packet and first-update serialization caches keyed by entity id and `NetCompatibilityRules`, cleared with `m_netStateCache`.
4. Move packet preparation into a post-tick job for read-only snapshot data, then merge produced packet lists back on the world thread in the same per-client order.
5. Move metrics and persistence snapshot serialization next, because they can consume immutable summaries and do not affect gameplay state.
6. Consider a world-local job scheduler only after the shared `WorkerPool` queue behavior is measured under server load; overloaded post-tick jobs must not stall the next authoritative update indefinitely.
7. Keep liquid, falling blocks, wiring, entity update, and Lua update serial until Phase 6 experiments prove deterministic boundaries.

Phase 5 diagnostics to add:

- per-world post-tick job queue depth, job duration, merge duration, and missed-deadline count
- packet preparation time split by tile updates, entity creates, entity deltas, monitored entity collection, and compression/serialization where measurable
- cache hit/miss counts for entity delta, entity create, and net-store payload caches
- monitoring-region build count per tick to prove repeated recomputation has been removed

Phase 5 compatibility checkpoints:

- Per-client packet ordering and packet type sequence remain equivalent for deterministic scenarios.
- Jobs read only frozen snapshot data; they do not touch `EntityMap`, `WorldStorage`, Lua contexts, live entities, or mutable client info.
- Generated packet bytes are compared against the serial path for fixed seeds and controlled worlds before enabling by default.
- If a job misses its merge deadline, the fallback is a serial packet preparation path or a bounded one-tick defer that is explicitly measured.

Acceptance criteria:

- Packet output remains ordered and equivalent.
- Snapshot jobs never read live mutable world state.
- World tick p95/p99 improves in packet-heavy or save-heavy scenarios.
- The serial packet preparation path remains available behind a config flag during rollout.

Primary risks:

- Snapshot creation can cost more than the parallel work saves.
- Job completion waits can stall the next tick if queues are overloaded.
- Packet stream equivalence can be difficult when timing-dependent state exists.
- Capturing too much world state can trade CPU wins for memory pressure.

Tests:

- Dense entity replication.
- Multiple clients entering and leaving monitored regions.
- Save-heavy world benchmark.
- Deterministic packet comparison for fixed scenarios.
- Entity create burst where many clients observe the same entities in the same tick.
- World generation queue with many sectors and multiple players.

## Phase 6: Experimental World-Internal Parallelism

Goal: explore deeper single-world scaling behind configuration flags after ownership and snapshot boundaries are stable.

Code areas:

- world generation and storage generation planning
- liquid processing
- falling blocks
- wiring networks
- eventually entity update grouping only with strong dependency analysis

Implementation tasks:

1. Start with storage generation planning and packet preparation, where observability risk is lowest.
2. Add region-based liquid experiments behind a config flag.
3. Add disconnected wiring-network experiments behind a config flag.
4. Add deterministic fallback paths for every experiment.
5. Add per-world metrics comparing serial and parallel duration.
6. Never parallelize entity or Lua updates by default until mod-visible ordering rules are formally defined.

Experiment order and guardrails:

1. Storage generation planning: precompute sector priorities and generation candidates outside the mutation step, then apply generation serially.
2. Packet preparation: keep using the Phase 5 snapshot path and only broaden worker count or batching strategy.
3. Liquid processing: partition active cells by non-overlapping regions, keep the current active-cell ordering inside each region, and compare serial/parallel output before enabling.
4. Falling blocks: process independent regions only when no pending positions can influence neighboring regions during the same update.
5. Wiring: build disconnected network components, mark dirty components on topology/output changes, and evaluate unchanged components through the existing serial fallback until dirty tracking is proven.
6. Entity/Lua grouping: research only; do not enable by default unless a formal dependency and mod-visibility model exists.

Phase 6 diagnostics to add:

- per-experiment enabled flag, serial duration, parallel duration, merge duration, and fallback count
- divergence detector counters for serial-versus-parallel comparison runs
- per-subsystem work item counts: liquid active cells, falling-block pending positions, wiring networks, generation sectors, packet entities
- config surface that can disable each experiment independently without changing save or packet formats

Phase 6 compatibility checkpoints:

- Experimental flags are off by default in release configs.
- Serial fallback can be selected at runtime or startup for every experiment.
- Parallel liquid, falling, and wiring experiments must prove no boundary artifacts in fixed-seed differential tests before broader testing.
- Mods must continue to observe the same Lua/entity order in default mode.

Acceptance criteria:

- Experimental flags are off by default.
- Serial fallback remains available and tested.
- Lua-visible order does not change in default mode.
- Any save, packet, or simulation divergence is intentional and documented.
- Each experiment has enough diagnostics to decide whether it should graduate, stay hidden, or be removed.

Primary risks:

- Liquid, wiring, entity, and Lua systems may have hidden order dependencies.
- Region boundaries can create edge artifacts.
- Mods can depend on behavior that looks accidental from engine code.
- Comparing serial and parallel worlds can be noisy unless seeds, client inputs, and timing are controlled.

Tests:

- Serial-versus-parallel differential runs.
- Region boundary stress tests.
- Wiring-heavy worlds.
- Liquid-heavy worlds.
- Modded smoke tests.

## Remaining Codebase Work Order

This section is the practical backlog for the rest of the codebase after Phase 1/2 planning. It keeps low-risk, compatibility-preserving changes ahead of deeper concurrency work.

1. Finish Phase 0 diagnostics so every later phase has universe, world, network, persistence, and packet-preparation timing.
2. Complete Phase 1 worker tests and run idle/high-connection profiling before changing socket readiness behavior.
3. Implement Phase 2 behind a fallback flag and keep packet-order compatibility tests close to the handshake code.
4. Land low-risk world hot-path cleanups before broad parallelism: keep `EntityUpdateSetPacket` application bounded by owner-indexed entity sets, cache per-client monitoring regions once per tick, precompute world-generation sector priorities, add entity-create serialization caches parallel to `m_netStateCache`, batch tile/liquid fan-out by subscribed sector where practical, and add shared delta caching for `SystemWorldServer` ship/object replication.
5. Build Phase 3 persistence snapshots and executor with strict immutable-data rules.
6. Convert Phase 4 world and system-world entry points to typed commands while keeping synchronous compatibility wrappers during rollout.
7. Use Phase 5 snapshots for packet preparation, metrics, and persistence serialization before trying subsystem parallelism.
8. Keep Phase 6 experiments isolated, off by default, and backed by differential tests.
9. Revisit lower-level primitive cleanup after ownership boundaries are flatter: recursive mutex reduction, spinlock replacement, and `WorkerPool` backpressure improvements.
10. Update `diagnostics-debugging-roadmap.md` whenever a phase adds counters that should appear in overlays, `/serverstatus`, crash bundles, or structured logs.

Codebase-wide acceptance gates:

- Existing save files, world files, player files, packet formats, and mod-visible Lua/entity ordering remain compatible by default.
- Every phase has a fallback path until tests and profiling prove the new path is stable.
- Performance claims are tied to measurements: p50/p95/p99 tick times, idle CPU, context switches, queue depths, and packet latency.
- Crash and diagnostics output includes enough context to debug the new queues, workers, and ownership boundaries.

## First Optimization Verification Plan

For the Phase 1 change already started, verify in this order:

1. Configure Windows build once vcpkg is available: `cmake --preset=windows-release` from `source/`.
2. Build at least `game_tests` or the full preset: `cmake --build --preset=windows-release`.
3. Run no-asset tests: `ctest --preset=windows-release`.
4. Add a temporary or permanent network worker counter dump after Phase 0 counters are available.
5. Compare idle CPU and context switches with many connected idle clients before and after the change.
6. Compare packet latency for a low-rate ping/action workload.
7. Stress add/remove/send while workers are active.

Expected result:

- Lower CPU time in network workers under many idle connections.
- Less global connection-map copying.
- Same per-client packet ordering and disconnect behavior.
- Minimal TPS change for one crowded world, because the main simulation lane remains serial.
