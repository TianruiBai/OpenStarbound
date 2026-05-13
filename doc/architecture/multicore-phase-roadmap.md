# OpenStarbound Multicore Phase Roadmap

This roadmap turns `multicore-engineering-plan.md` into an execution checklist. It keeps the same compatibility-first strategy: improve multicore use around the serial world simulation lane before attempting world-internal parallel simulation.

Phase 0A is implemented. Phase 0 now has a shared timing accumulator, bounded universe-loop phase timing, world-thread timing, world-update subphase timing, network worker counters, `/serverstatus` timing summaries, and `world_benchmark.cpp` subphase summary support. Phase 1 worker-owned network connection lists and event wakeups are implemented in `source/game/StarUniverseConnection.*` and covered by focused `UniverseConnectionServer` tests. Phase 2 is implemented behind default-enabled `usePendingConnectionStateMachine`, preserving the old thread-per-handshake path as a fallback, and now has focused state-machine success, protocol-rejection, and timeout coverage plus live handshake diagnostics. Phase 3 has its compatibility-first persistence slice implemented with immutable versioned persistence snapshots, synchronous shared write helpers, default-enabled bounded async JSON persistence behind `useAsyncPersistence`, explicit system-world and ship-chunk snapshot boundaries, shutdown draining, retry/fallback accounting, and focused async completion / queue-pressure / shutdown-drain tests. Phase 4 has a world-thread command mailbox covering common synchronous wrappers, admin/RPC world actions, weather, dungeon placement, flying-sky transitions, and the ship-upgrade command/result path; `SystemWorldServerThread` routes add/remove client ship mutations through a small command mailbox and publishes read snapshots for ship location, warp action, sky, clients, and active instance worlds. Phase 5 has owner-indexed entity update application, a formal per-tick `WorldTickSnapshot` packet-prep context, cached generation-priority distance calculations, packet-prep caches for shared sector updates and entity store payloads, `/serverstatus` packet-prep diagnostics, and focused `MulticorePhaseTest` coverage for Phases 0 through 5. Phase 6 currently contains default-enabled storage-generation planning and packet-sector prefill experiments with serial fallback diagnostics, default-enabled serial-versus-worker differential checks, default-enabled serial mutation baseline metrics for liquid/falling-block/wiring/entity/Lua work, and focused guard/equivalence tests; liquid, falling-block, wiring, entity, and Lua parallel mutation remain deferred behind fixed-seed state, dependency, and mod-visibility gates. A future original gameplay mechanism compatible API track is now the planned bridge for more aggressive modern optimization: legacy APIs keep exact ordering, while opt-in APIs can preserve phase-boundary gameplay results without promising every undocumented intermediate step.

The observability and crash-reporting work that supports these phases is tracked in `diagnostics-debugging-roadmap.md`. In short: build on the current `/debug` overlay, `LogMap`, `SpatialLogger`, `Logger`, stack traces, Lua profiles, and `/servernetstats` to provide F3-style status, server diagnostic commands, crash bundles, and structured logs.

## Current Status After Review

- Phase 0A render-rate decoupling and the user-facing VSync / max-FPS controls are implemented and wired through the client configuration, graphics menu, and debug HUD.
- Phase 0 has the core measurement foundation in place: `StarServerTiming.hpp` provides shared bounded timing records, `/serverstatus` reports universe-loop, world-thread, and world-update timing summaries, `UniverseConnectionServer::workerStats()` reports scan/wakeup/callback timing counters, and `world_benchmark.cpp` includes world-update subphase timing percentiles. The `world_benchmark` CMake target is currently commented out in `source/utility/CMakeLists.txt`; re-enabling that utility target is build tooling work, not Phase 0 instrumentation work. Crash-bundle/F3-style consumers are tracked in the diagnostics roadmap, not as remaining Phase 0 implementation work.
- Phase 1 is implemented and validated by focused worker-ownership, many-idle-connection, wakeup, remove-during-callback, and cross-worker packet-ordering tests. Socket readiness abstraction, cross-platform poller implementations, and broad TCP/high-fan-out profiling are post-Phase-1 networking hardening work.
- Phase 2 is implemented and live behind its fallback flag. Focused tests cover local state-machine success, protocol mismatch rejection, protocol-request timeout, and client-connect timeout, while `/serverstatus` exposes pending handshakes by state plus accepted/finalized/rejected/timed-out counters. Password/duplicate UUID/asset mismatch/max-player/login-burst cases are the extended validation matrix for hardening the enabled path, not unfinished Phase 2 implementation tasks.
- Phase 3 has its compatibility-first persistence slice in place and default-enabled in the OpenStarbound game config: immutable universe/client/system snapshots, bounded async JSON persistence, synchronous fallback on queue pressure, retry accounting, shutdown draining, and server diagnostics. Focused tests now cover async triggered-storage completion, queue-full synchronous fallback, readable saved files, and shutdown draining of queued writes; save/load depth, failure injection, and broader latency validation remain required before removing fallback paths or widening the async write scope.
- Phase 4 now routes most external `WorldServerThread` entry points through named commands while the world thread is active: client add/remove, spawn checks, revive position, new planet type, weather list/set, dungeon placement, flying-sky start/stop, container item RPC, universe flag RPC, admin/scripted `executeForClient`, ship upgrades, unload, and chunk reads. `/serverstatus` exposes aggregate world-command pending, processed, direct, failed, and wait-time counters. `SystemWorldServerThread` now has a command mailbox for add/remove client ship mutations and published read snapshots for clients, ship locations, warp actions, sky parameters, and active instance worlds.
- Phase 5 has its compatibility-first snapshot/prep slice in place: client-owned/slave entity ids are indexed by owner connection, `WorldServer::update()` builds a per-tick `WorldTickSnapshot` with client windows and monitoring regions, liquid/weather/sector signaling/packet prep consume that snapshot, world generation sorting memoizes nearest-player sector distances, and packet prep reuses sector tile-array packets plus entity store payloads within a tick. Blank entity-update deltas are still delivered to every indexed entity for that owner, preserving interpolation/extrapolation behavior; first net-state writes remain per-client because they advance entity net versions. A focused Phase 6 packet-sector-prefill equivalence test now compares tile-array packet payloads against the serial path, the two implemented Phase 6 worker helpers run default-enabled serial-versus-worker differential checks with divergence counters, and serial liquid/falling-block/wiring/entity/Lua baseline counters are available before any parallel mutation path lands. Deeper immutable entity serialization snapshots and entity packet-equivalence tests are still Phase 6 gates before workerized packet prep.
- Phase 6's next aggressive route is an explicit original gameplay mechanism compatible API. Existing mods stay on legacy exact-order behavior; opt-in mods or server configs can accept defined phase-boundary equivalence, batched world APIs, deferred mutation, and snapshot reads so the engine can use more modern parallel internals while falling back whenever legacy-sensitive APIs are used.

May 2026 Phase 0-6 audit validation:

- Built `starbound`, `starbound_server`, and `game_tests` successfully from the VS 2022 developer environment with `cmake --build build/windows-release --target starbound starbound_server game_tests`.
- Passed the focused Phase 0-6/server/network suite after subsystem-baseline coverage was added: `game_tests.exe --gtest_filter=MulticorePhaseTest.*:ServerTest.*:UniverseConnections.*:UniverseConnectionServer.*` ran 26 tests with 26 passing.
- `ctest --output-on-failure` passed `core_tests`; the full `game_tests` CTest leg exceeded the audit timeout while entering the broad game fixture set.
- A broad game run excluding only `SpawnTest.*` exposed unrelated item/asset fixture failures in `ItemTest.ItemComparison` and `ItemTest.ConstructItems` (`/items/blueprints/blueprint_icon.png` missing and a `knightfall_buildunrandweapon` Lua buildscript issue).
- The remaining broad game suites passed with `game_tests.exe --gtest_filter=-SpawnTest.*:ItemTest.*`: 31 tests from 10 suites passed.

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

1. Done: add a lightweight timing accumulator type that can be compiled into release-with-debug-info builds without excessive allocation.
2. Done: add universe-loop phase timers around Lua update, chat, teams, ship update, warp/fly/arrive, celestial response, broken-world cleanup, world messages, inactive-world shutdown, and triggered storage.
3. Done: add world-thread timers around incoming packets, world update, message handling, outgoing packet collection, update callbacks, and sync.
4. Done: add world-update subphase timers for entity update, world scripts, damage, wiring, weather, liquid, falling blocks, storage tick, storage generation, and packet preparation.
5. Done: network counters cover worker wakeups, timed and idle timed waits, owned connections, handled/scanned/stale-scanned connections, packets processed, callback groups, and callback duration.
6. Done: extend `world_benchmark.cpp` to print subphase timing summaries and percentiles; the utility executable target remains commented out in `source/utility/CMakeLists.txt`.
7. Done: selected counters feed `/serverstatus`, `/servernetstats`, and `LogMap`; F3-style client overlay and crash-bundle consumers remain diagnostics-roadmap work.
8. Moved to diagnostics roadmap: crash-report context hooks for current phase timers, network worker stats, active worlds, and storage state are broader diagnostic consumers, not Phase 0 blockers.

Acceptance criteria:

- A dedicated server run can emit per-phase timing summaries without changing packet or save formats.
- Measurements include p50, p95, and p99 tick durations where practical.
- Measurement overhead is visible and bounded under a config flag.
- Server commands and logs consume the current diagnostic snapshots; client overlay and crash-report consumers are tracked in the diagnostics roadmap.

Primary risks:

- Logging too frequently can become the bottleneck.
- Timers placed inside entity/Lua inner loops can distort the profile.

Phase 0 closeout check:

- Done for Phase 0: the metrics can identify whether the server is network-bound, persistence-bound, universe-loop-bound, or one-world-update-bound.

## Phase 1: Network Worker Sharding And Wakeups

Goal: reduce idle CPU and high-connection bookkeeping without altering packet semantics.

Code areas:

- `source/game/StarUniverseConnection.hpp`
- `source/game/StarUniverseConnection.cpp`
- later: `source/core/` socket poller abstraction

Implementation tasks:

1. Done: give each network worker explicit ownership of assigned connection IDs.
2. Done: stop each worker from copying and filtering the full global connection map every loop.
3. Done: wake the owning worker when a connection is added, removed, or has packets queued for send.
4. Done: keep the current timed polling fallback until a portable socket readiness abstraction exists.
5. Done: add worker counters for wakeups, timed/idle timed wakeups, owned connection count, processed packets, scans, callback groups, and callback time.
6. Deferred: design a narrow `SocketPoller` abstraction for Windows, Linux, and macOS as future networking hardening.
7. Deferred: add a Windows readiness implementation using a conservative API first, then evaluate IOCP separately as a larger networking project.
8. Deferred: add Linux `epoll` and macOS `kqueue` implementations only after the fallback path is stable.

Started work:

- `UniverseConnectionServer` now has worker-owned connection lists.
- Worker loops now copy only their assigned connection IDs.
- Add, remove, and send paths wake the owning worker through `ConditionVariable`.
- Worker scan, stale-id, callback, wakeup, timed-wait, and idle-wait counters are available through `UniverseConnectionServer::workerStats()`.
- `/servernetstats` exposes the worker counters for live admin profiling.
- `/serverstatus` exposes a compact server diagnostics snapshot with uptime, player counts, active worlds, pending queue sizes, TCP state, and aggregate network counters.
- Per-connection packet callback ordering remains owned by one worker.

Phase 1 closeout result:

1. Done: `ManyIdleConnectionsRemainSharded` covers many idle local connections, owned connection distribution, scan counters, idle waits, active packet processing, and queued-send wakeups.
2. Done: `PacketOrderingAcrossWorkers` covers repeated small bursts across clients assigned to different workers.
3. Deferred: byte counters remain optional because the current worker-stat path does not need additional allocation-heavy packet sampling.
4. Deferred: high-fan-out profiling of eager `sendPackets` versus queue-only send remains a networking hardening measurement before any readiness-abstraction change.
5. Done: the 1 ms timed fallback remains in place until socket readiness has cross-platform coverage and failure-mode tests.
6. Done: `/servernetstats` and this section document the healthy interpretation of owned connections, scans, wakeups, timed waits, idle waits, packet counts, and callback duration.

Completed Phase 1 implementation sequence:

1. Done: test scaffolding uses `LocalPacketSocket::openPair()` and `UniverseConnectionServer` packet callbacks.
2. Done: add, send, receive, remove, and shutdown behavior is covered with one worker and multiple workers across the focused tests.
3. Done: the many-idle-connections stress case exercises sharded ownership, scans, idle waits, active packet processing, and send wakeups.
4. Done: `RemoveConnectionDuringCallback` covers disconnect from inside the packet callback.
5. Deferred: `SocketPoller` remains future work behind a flag or compile-time path; the condition-variable/timed fallback is still the default.

Phase 1 diagnostics status:

- Done: `/servernetstats` shows per-worker owned connections, last handled connections, scans, stale scans, packets, callbacks, average callback time, wakeups, timed waits, and idle waits.
- Done: `/serverstatus` keeps aggregate network worker counts and pending accept/handshake counts visible.
- Done: a healthy worker-owned loop has `connectionScans` scale with owned connections per worker, not worker count times total connections.
- Done: after a quiet period, a queued send increases the owning worker's wakeup count and does not rely on a long polling timeout.

Phase 1 closeout checks:

- No known packet ordering regression with multiple clients.
- No stale-worker-list growth after repeated add/remove/disconnect stress.
- No deadlock in shutdown or `removeAllConnections` while workers are active.
- Idle wait behavior is covered by focused tests; broader idle CPU/context-switch profiling across Windows and Linux/Wine remains hardening measurement.
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
- Many idle local clients with a small number of active clients; broader TCP idle profiling is a follow-up measurement.

## Phase 2: Handshake State Machine

Goal: remove one thread per pending handshake and reduce login-burst scheduler pressure.

Code areas:

- `source/game/StarUniverseServer.cpp`
- `source/game/StarUniverseServer.hpp`
- `source/game/StarNetPackets.hpp`
- possibly `source/game/StarUniverseConnection.*`

Implementation tasks:

1. Done: `PendingConnection` and `PendingConnectionState` cover await protocol request, send protocol response, await client connect, await password challenge response, finalize, reject/flush, and dead states.
2. Done: each pending connection stores a state deadline instead of blocking a dedicated thread in `receiveAny(clientWaitLimit)`.
3. Done: pending handshakes are driven from the universe loop through `processPendingConnections()`.
4. Done: protocol response, compression boundary, client connect validation, password challenge, and finalization preserve the packet sequence at the packet-type level.
5. Done: final client-context registration remains under the universe ownership path.
6. Done: failure paths use bounded reject/flush states instead of unbounded detached work.

Legacy blocking flow preserved by the fallback path:

1. `addClient` or the TCP accept callback creates one `Thread::invoke("UniverseServer::acceptConnection", ...)` task.
2. The server blocks for `ProtocolRequestPacket`, replies with `ProtocolResponsePacket`, and enables the compression stream only after that response is sent.
3. The server blocks for `ClientConnectPacket`, validates protocol, asset digest, ship species, account settings, password requirements, anonymous login settings, and bans.
4. If the account is named, the server sends `HandshakeChallengePacket`, waits for `HandshakeResponsePacket`, and compares the salted hash.
5. The server computes `NetCompatibilityRules`, handles duplicate UUID priority, enforces max players, loads client context, adds the connection to `UniverseConnectionServer`, sends `ConnectSuccessPacket`, and schedules the initial warp/fly state.
6. Failure paths send `ConnectFailurePacket` where possible and retain the connection in `m_deadConnections` long enough to flush pending data.

Implemented `PendingConnection` data:

- stable pending id for diagnostics, separate from final `ConnectionId`
- `UniverseConnection` and optional remote address
- current state and state deadline in monotonic milliseconds
- pending `ClientConnectPacket`, account string, administrator flag, password salt, legacy-client flag, compression-stream flag, and accumulated failure reason
- cached remote address string for logging without recomputing it in every state

Implemented state machine sketch:

| State | Nonblocking work | Next state |
| --- | --- | --- |
| `AwaitProtocolRequest` | call `receive()`, read `ProtocolRequestPacket`, reject missing or bad packets at deadline | `SendProtocolResponse` or `RejectAndFlush` |
| `SendProtocolResponse` | queue/write `ProtocolResponsePacket`; unsupported protocol sets `allowed=false`; enable zstd stream after the response has flushed at the current protocol boundary | `AwaitClientConnect` or `RejectAndFlush` |
| `AwaitClientConnect` | call `receive()`, read `ClientConnectPacket`, reject timeout or unexpected packet | `ValidateClientConnect` |
| `ValidateClientConnect` | perform asset, species, account, anonymous, ban, max-version, and compatibility checks | `AwaitHandshakeResponse`, `FinalizeClient`, or `RejectAndFlush` |
| `AwaitHandshakeResponse` | send challenge once, then call `receive()` until `HandshakeResponsePacket` or deadline | `FinalizeClient` or `RejectAndFlush` |
| `FinalizeClient` | under universe/client ownership, allocate `ConnectionId`, load context, register RPC handlers, add to `UniverseConnectionServer`, send success packets, schedule initial world/system placement | `Dead` |
| `RejectAndFlush` | queue `ConnectFailurePacket` or final protocol response, call nonblocking `send()`, keep the connection until sent or deadline | `Dead` |

Completed Phase 2 implementation sequence:

1. Done: `PendingConnection` and `PendingConnectionState` live in `UniverseServer` while the old `acceptConnection` path remains compiled and callable as a fallback.
2. Done: `m_pendingConnections`, pending counters, and `processPendingConnections()` are driven from the universe loop.
3. Done: TCP accept and `addClient` enqueue pending connections behind `usePendingConnectionStateMachine`, leaving the thread-per-handshake path available as a fallback.
4. Done: protocol response handling includes unsupported protocol failure and compression-mode negotiation.
5. Done: `ClientConnectPacket` validation was ported without changing the public packet sequence.
6. Done: password challenge and failure handling were ported while preserving the existing authentication messages.
7. Done: finalization is a helper on the universe thread and reuses duplicate UUID, max player, client-context loading, system-world placement, revive warp, and script callback logic.
8. Deferred: the old accept-thread fallback remains until the extended local/TCP/legacy/OpenStarbound/password/timeout/duplicate UUID matrix passes.

Phase 2 diagnostics status:

- Done: pending handshake count by state is visible in `/serverstatus`.
- Done: accepted, finalized, rejected, and timed-out counters are visible in `/serverstatus`.
- Deferred: dropped-pending counters, oldest pending age, maximum deadline overrun, and failure reason buckets remain hardening diagnostics for the enabled path.

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

- Covered by focused tests: local state-machine success, protocol mismatch rejection, protocol-request timeout, and client-connect timeout.
- Extended hardening matrix before removing the fallback: legacy and OpenStarbound handshakes, password success/failure, duplicate UUID as admin and non-admin, login bursts with deliberately slow clients, protocol-mismatch flush behavior, asset mismatch refusal on both sides, max-player refusal with administrator priority, and timeout coverage for password/challenge flush edge states.
- Covered by focused tests and live local-client use: local single-player connection still succeeds without requiring TCP-only code paths.

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
- `useAsyncPersistence` enables a dedicated `UniverseServerPersistencePool`; it is now enabled by default in the OpenStarbound game config while synchronous fallback and explicit opt-out remain available.
- `maxQueuedPersistenceSnapshots` bounds queued snapshot count. When the queue is full, the server falls back to synchronous writes instead of dropping required state.
- `maxPersistenceWriteRetries` adds opt-in bounded write retries; it defaults to `0` to preserve current write behavior while reporting retry counts when enabled.
- `ServerClientContext::ShipChunksSnapshot` now captures immutable full-chunk and delta data at the `readChunks()` boundary before the context is mutated, while keeping the existing ship-update path unchanged.
- `SystemWorldServerThread::store()` now has a system-world `System` JSON snapshot boundary: it serializes immutable versioned data first, then writes that snapshot synchronously.
- `UniverseServer::doTriggeredStorage()` now runs celestial cleanup/commit outside the universe locks, and `/serverstatus` reports accumulated celestial commit time and count so the remaining synchronous hot path stays visible.
- Shutdown drains pending async persistence writes before final synchronous universe and temp-world index saves, preventing older queued autosaves from overwriting shutdown state.
- `/serverstatus` reports pending persistence batches, pending snapshots, oldest pending age, completed batches, written snapshots, build/write time, failures, retry attempts, synchronous fallbacks, and queue-full fallbacks.

Completed Phase 3 implementation sequence and gates:

1. Done: the versioned snapshot path covers the current universe-owned JSON writes used by triggered storage: `UniverseSettings`, `TempWorldIndex`, and `ClientContext`.
2. Hardening after default enablement: validate `useAsyncPersistence` under broader save/load and shutdown scenarios before removing any fallback path.
3. Hardening after default enablement: add failure-injection or filesystem-denial tests that prove retries, failures, and queue-full fallbacks are reported.
4. Deferred design decision: ship chunk snapshots currently remain a transport/snapshot boundary; a server-side ship chunk write path should be evaluated only after owner-thread chunk-update boundaries are proven.
5. Done: client context and universe/temp-world JSON writes can run through the executor as immutable versioned JSON snapshots.
6. Deferred: ship chunk update writes remain synchronous/owner-bound until a serialized update path can be proven safe.
7. Done for the safe slice: `SystemWorldServerThread::store()` produces immutable versioned JSON before writing; enqueueing system-world writes remains deferred until result reporting is expanded.
8. Done by deferral: world database `sync()` remains on the world thread until sector-level serialized updates exist without exposing live `WorldStorage` or `BTreeDatabase` to workers.
9. Done: shutdown drains pending async persistence writes before final synchronous saves.

Phase 3 diagnostics status:

- Done: `/serverstatus` reports persistence queue depth, pending snapshots, oldest queued age, completed batches, written snapshots, failed snapshots, retry count, synchronous fallbacks, queue-full fallbacks, snapshot-build time, worker write time, and celestial commit time/count.
- Deferred: bytes written by job type, per-world sync duration, per-system store duration, and crash-report context for pending required persistence jobs remain diagnostics-roadmap/hardening work.

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

Focused tests now cover default-enabled async triggered-storage completion, creation and loadability of `universe.dat` and `tempworlds.index`, shutdown draining of queued async writes, and bounded-queue synchronous fallback when the async queue is full.

Primary risks:

- Writing live mutable objects from background threads would introduce races.
- Reordering critical writes can change crash-recovery behavior.
- Unbounded persistence queues can trade tick spikes for memory growth.
- Moving `BTreeDatabase::commit()` off-thread before serialized sector updates exist can corrupt ownership assumptions.

Tests:

- Covered by focused tests: default-enabled async triggered-storage completion, `universe.dat` and `tempworlds.index` creation/loadability, shutdown drain of queued async writes, and bounded-queue synchronous fallback without dropping required state.
- Hardening matrix after default enablement: normal save/load depth, disconnect while save is pending, ship upgrade while save is pending, simulated write failure, old-save load after async-written state, and system-world flight/arrival survival after restart.

## Phase 4: Strict World Mailbox Ownership

Goal: make world threads true mutation owners and reduce direct cross-thread world locking.

Code areas:

- `source/game/StarWorldServerThread.*`
- `source/game/StarSystemWorldServerThread.*`
- `source/game/StarUniverseServer.cpp`
- `source/game/StarServerClientContext.*`

Implementation tasks:

1. Done: add typed world commands for add/remove client, pause, read chunks snapshot, spawn-target check, messages, weather, dungeon placement, ship upgrades, flying sky, sync, and unload.
2. Done: add reply promises for commands that need a result.
3. Done: drain commands at explicit owner-thread tick boundaries before packet handling/world update work.
4. Done: convert `addClient` and `removeClient` while preserving compatibility wrappers.
5. Done: replace production generic `executeAction()` call sites with typed commands in small batches.
6. Done for the current external mutation paths: convert system-world add/remove client ship mutations to equivalent commands; existing destination/action/packet paths already queue work.
7. Done: add queue-depth and command-latency metrics to aggregate `/serverstatus` output.

Started work:

- `WorldServerThread` now has a command mailbox drained at the beginning of the owner-thread update tick.
- Synchronous wrappers for `spawnTargetValid`, `addClient`, `removeClient`, `playerRevivePosition`, `pullNewPlanetType`, `unloadAll`, and `readChunks` enqueue commands when the world thread is running and fall back to direct execution before start or after stop.
- Named commands also cover pause propagation, weather list/set, dungeon placement, flying-sky start/stop, container item RPC insertion, universe flag RPC mutation, and the admin/scripted `executeForClient` helper.
- `WorldServerThread::applyShipUpgrades()` replaces the remaining ship-upgrade `executeAction()` call with a named command/result path that returns species, upgraded ship state, and ship chunks for the client-context snapshot update.
- `SystemWorldServerThread` now has a small command mailbox for `addClient()` and `removeClient()`, drained before incoming packet handling and system simulation; active instance worlds and clients are read from published thread-side state under locks.
- Command waiters are released with a failure if the world thread exits before their command runs.
- Per-thread command counters exist in `WorldServerThread::CommandStats` and aggregate pending, processed, direct, failed, and wait-time values are visible in `/serverstatus`.

Phase 4 closeout result:

1. Done: `SystemWorldServerThread` publishes read snapshots for clients, ship locations, sky parameters, warp actions, and active instance worlds under the queue lock after owner-thread updates.
2. Deferred: oldest command age and per-world/system-world command details belong in `/worldstats` once that command exists.
3. Done for the safe slice: focused tests cover queued pause mutation and ship-upgrade command/result propagation through the mailbox without command failures.
4. Deferred: command failure propagation, disconnect during queued world work, and broader admin/RPC behaviors remain hardening coverage before removing the generic compatibility wrapper.

Current direct world-entry points are wrapped by named commands or retained as compatibility wrappers:

- `WorldServerThread::spawnTargetValid()`
- `WorldServerThread::addClient()`
- `WorldServerThread::removeClient()`
- `WorldServerThread::playerRevivePosition()`
- `WorldServerThread::pullNewPlanetType()`
- `WorldServerThread::executeAction()`
- `WorldServerThread::unloadAll()`
- `WorldServerThread::readChunks()`
- `WorldServerThread::sync()`

Current system-world entry point status:

- `SystemWorldServerThread::addClient()` and `removeClient()` now enqueue owner-thread commands when the system world is running and fall back to direct execution before start or after stop.
- `setClientDestination()`, `executeClientShipAction()`, and `pushIncomingPacket()` already queue work and should become named diagnostic commands only if command diagnostics are expanded to system worlds.
- `clientShipLocation()`, `clientWarpAction()`, `clientSkyParameters()`, `clients()`, and `activeInstanceWorlds()` read published thread-side state under locks instead of directly reading live `SystemWorldServer` state from callers.

Completed Phase 4 implementation sequence and gates:

1. Done: `WorldServerThread` has a command queue and synchronous compatibility wrappers around commands.
2. Done: read-result wrappers cover client ids, expiration state, new planet type, player revive position, and ship chunks where callers need synchronous results.
3. Done: `spawnTargetValid`, `playerRevivePosition`, and `readChunks` route through the command path while the world thread is running.
4. Done: `addClient` and `removeClient` route through commands while preserving outgoing packet return behavior.
5. Done: generic `executeAction()` production call sites were replaced with named commands for weather, flying sky start/stop, container item RPC, universe flag RPC, ship upgrades, admin actions, and scripted world actions.
6. Done: `sync()` and `unloadAll()` run on the owner-thread command path with completion results for deliberate waiting.
7. Done: `SystemWorldServerThread` uses queued add/remove client ship commands and publishes read snapshots after updates.
8. Gate before wrapper removal: keep the old generic compatibility wrapper until command error propagation, ship/admin/RPC coverage, and old extension points are audited.

Phase 4 diagnostics status:

- Done: `/serverstatus` aggregates world command queue depth, processed/direct/failed counts, and total wait time.
- Done: world-thread update time split into command drain, packet handling, simulation, messages, outgoing packet collection, update callbacks, and sync is visible through timing status.
- Deferred: oldest command age, per-world/system-world command details, and explicit remaining-wrapper audits belong in `/worldstats` or hardening diagnostics.

Phase 4 compatibility checkpoints:

- Commands that return packets must preserve packet order relative to client context updates and warp result packets.
- Reply-waiting from `UniverseServer` must not happen while holding locks that the world command needs to complete.
- Client world and system-world references in `ServerClientContext` remain consistent while add/remove commands are in flight.
- The generic `executeAction()` wrapper currently has no production callers, but keep it until script/admin command coverage has dedicated regression tests and old extension points have been audited.

Acceptance criteria:

- World state mutation from outside the owner thread is removed or explicitly isolated.
- Common warps, beam up/down, ship travel, disconnect, and idle unload behave the same.
- Command queueing does not introduce unbounded one-tick delays in user-visible flows.
- Compatibility wrappers that can still touch direct world-lock paths are documented and temporary.

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
- `m_netStateCache` caches repeated entity delta serialization within one tick, while `queueUpdatePackets()` now caches shared sector tile-array packets and entity store payloads for first-observation entity create packets. First net-state writes are still per-client because `writeNetState(0)` advances the entity net version.
- `WorldStorage::generateQueue()` can sort queued sectors using a comparator that recomputes nearest-player distance repeatedly.

Started work:

- `WorldServer::ClientInfo` tracks `clientMasterEntities`, populated from legal `EntityCreatePacket` ids and cleaned during entity removal and client removal.
- `WorldClient` tracks slave entity ids by `ConnectionId` for incoming `EntityUpdateSetPacket` application.
- Packet update receive paths still pass empty deltas for owner entities without payload entries, preserving `NetElementTop::blankNetDelta()` behavior when interpolation is enabled.
- `WorldServer::update()` now caches each client's monitoring regions once per tick and reuses them for liquid no-processing regions, sector signaling, and packet preparation.
- `WorldStorage::generateQueue()` ordering now memoizes each queued sector's nearest-player distance during sorting instead of recomputing it for every comparator call.
- `WorldServer::queueUpdatePackets()` now carries per-tick caches for sector tile-array update packets and entity store payloads keyed by `NetCompatibilityRules`, reducing repeated serialization when multiple clients observe the same sector or entity in one tick.
- `WorldServer::WorldTickSnapshot` now owns the per-tick client windows, monitoring regions, packet-prep caches, and packet-prep counters, and `WorldServerThread` / `UniverseServer::serverStatus()` expose aggregate packet-prep ticks, monitoring-region builds, and cache hit/miss counts.
- `source/test/multicore_phase_test.cpp` adds a focused Phase 0-6 regression suite: universe timing status, world-thread/update timing status, network worker sharding/wakeups, pending-handshake rejection, async persistence queue fallback, world command mailbox processing and ship-upgrade result propagation, sector packet-prep cache reuse, Phase 6 storage-generation planning guards, and Phase 6 packet-sector prefill guards/equivalence.

Phase 5 closeout result:

1. Done for the compatibility-first slice: `WorldTickSnapshot` owns per-tick client windows, monitoring regions, packet-prep caches, and packet-prep counters used by serial packet preparation.
2. Gate before workerized packet prep: extend `WorldTickSnapshot` with immutable monitored entity ids, client net rules, pending tile/liquid/damage update copies, and entity serialization inputs suitable for worker jobs.
3. Gate before workerized packet prep: add deeper serial packet-equivalence tests around entity create/update traffic, especially around `writeNetState(0)` call counts and later delta behavior. Tile-array sector prefill now has focused serial-versus-prefill packet payload coverage.
4. Deferred diagnostics: packet-prep split timings, generation-priority cache counts, owner-index hit/miss counters, and post-tick job deadline counters should land with the worker-job path they measure.

Completed Phase 5 implementation sequence and Phase 6 gates:

1. Done: non-parallel algorithmic cleanups preserve behavior by applying inbound entity updates through owner-indexed entity sets, computing monitoring regions once per client per tick, and memoizing generation queue priorities during sorting.
2. Done for the safe slice: `WorldTickSnapshot` carries client windows, monitoring regions, caches, and counters used by serial packet preparation.
3. Done: sector packet and entity store cache metrics are live; first-update serialization remains per-client until packet-equivalence tests prove `writeNetState(0)` call-count changes are safe.
4. Phase 6 gate: move packet preparation into a post-tick job only after immutable entity serialization inputs and entity packet-equivalence tests exist; tile-array sector prefill has an initial serial-equivalence guard.
5. Phase 6/post-Phase-5 gate: move metrics and persistence serialization jobs only when they consume immutable summaries and have measured merge/fallback behavior.
6. Phase 6 gate: consider a world-local job scheduler only after shared `WorkerPool` behavior is measured under server load.
7. Still serial by design: liquid, falling blocks, wiring, entity update, and Lua update remain serial until Phase 6 experiments prove deterministic boundaries.

Phase 5 diagnostics now available:

- `/serverstatus` exposes aggregate packet-prep ticks, monitoring-region builds/rect counts, sector packet cache hits/misses, entity store cache hits/misses, and entity net-state cache hits/misses.
- `LogMap` exposes per-world packet-prep counters under `server_<world>_packet_prep`.
- Monitoring-region build count per tick is visible through packet-prep ticks and monitoring-region build counters.

Phase 5 diagnostics deferred to worker-job rollout:

- per-world post-tick job queue depth, job duration, merge duration, and missed-deadline count
- packet preparation time split by tile updates, entity creates, entity deltas, monitored entity collection, and compression/serialization where measurable
- generation-priority cache counts and owner-index hit/miss counters

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

Original gameplay mechanism compatible API track:

This is possible, but it should be treated as a new compatibility contract rather than a hidden rewrite of the legacy API. The goal is to keep original gameplay mechanisms, save formats, packet semantics, and user-facing outcomes compatible while allowing the engine to batch, defer, snapshot, and parallelize internals when an opt-in caller does not depend on undocumented intra-phase ordering.

Compatibility tiers:

1. Legacy exact mode keeps the current Lua/entity/world APIs, current serial update order, and old mod behavior. This remains the default for existing saves and mods.
2. Mechanism-compatible mode is opt-in through mod metadata, server config, or a future world compatibility profile. It promises equivalent results at documented phase boundaries, stable packet/save semantics, and defined hook order, but does not promise every intermediate cell, entity, wire, liquid, or pending-block visit order.
3. Native optimized mode is a future API surface for new or updated mods. It favors batched world queries, command-style deferred writes, immutable per-tick read snapshots, dirty-component subscriptions, and explicit event phases over immediate arbitrary world mutation.

Implementation direction:

1. Define capability flags that scripts and mods can query, such as legacy exact world access, mechanism-compatible deferred mutation, batched tile/liquid access, batched entity queries, and immutable tick snapshots.
2. Add explicit batched APIs before replacing hot legacy calls: tile/material/liquid reads by region, deferred tile/liquid mutation lists, component dirty notifications, and entity query snapshots.
3. Keep the owner thread responsible for the single visible merge. Worker jobs may prepare results, but final mutation, event delivery, and compatibility hooks happen at documented barriers.
4. Add diagnostics that separate legacy API cost from mechanism-compatible API cost, so mod-heavy performance reports can show when legacy compatibility is limiting parallel speedup.
5. Fall back to legacy exact mode per world, per mod, or per subsystem when a mod uses APIs that observe immediate ordering, mutates during forbidden phases, or triggers a serial-versus-optimized divergence.

Good first candidates:

- Liquid, falling-block, and wiring internals, because they already have engine-owned mechanisms and can be measured with fixed-seed signatures before exposing any new script contract.
- Packet/entity serialization inputs, because they already sit near a snapshot boundary and can use byte-equivalence tests.
- Lua/entity mutation only through new staged APIs and explicit event phases; existing immediate-update behavior stays serial by default.

Compatibility limits:

- Mods that inspect every intermediate update, depend on entity iteration order, or perform immediate world mutation from arbitrary hooks will not receive the full optimization benefit until they opt into the new API shape.
- Mechanism-compatible mode can preserve most gameplay compatibility, but it intentionally narrows what is guaranteed compared with legacy exact mode. That tradeoff is the price of getting closer to modern engine optimization without breaking old content by surprise.

Initial research notes:

- Windows thread-pool guidance emphasizes independent work items, avoiding dependency chains between queued jobs, and keeping worker resources bounded. Phase 6 experiments should use explicit worker pools with small item counts rather than unbounded per-sector jobs.
- The double-buffer pattern is the right mental model for simulation snapshots: workers read immutable current-frame data and the owner thread performs the single visible merge or mutation step.
- Deterministic-simulation guidance reinforces exact input/order control. Any experiment that can change floating-point, entity, Lua, liquid, wiring, or boundary order must stay hidden until serial-versus-parallel differential checks are practical.

Remaining Phase 6 research path:

1. Entity packet preparation: add immutable entity serialization inputs and byte-equivalence tests for entity store and net-state packets before expanding workerized packet preparation beyond tile-array sector prefill. `Entity::writeNetState(0)` still advances per-entity net versions, so any worker path must consume an owner-thread snapshot rather than live entities.
2. Liquid: build fixed-seed serial state signatures for active liquid cells and boundary regions before adding a region-partition worker path. Any future worker path must preserve active-cell ordering inside each region, compare merged output against the serial result, and fall back on divergence.
3. Wiring: introduce dirty-network or disconnected-component tracking before parallel evaluation. The current processor recursively loads reachable wire entities and then evaluates the working set, so a naive component split can miss newly loaded entities or change evaluation visibility.
4. Falling blocks: model pending-position dependency regions before dispatching work. Cascades can add neighboring positions during the same update, so parallel regions are only safe when the influence area cannot cross the region boundary.
5. Entity and Lua updates: keep as research-only. Mods can observe update order, shared script state, and immediate entity mutations, so default parallel execution needs a formal compatibility model rather than a performance flag.

Implemented Phase 6 slice:

- `phase6WorldParallelism.storageGenerationPlanning` is available in `worldserver.config` and is enabled by default in the OpenStarbound game config, with explicit per-server opt-out still available.
- When enabled, `WorldServer` snapshots the storage generation queue and player positions, optionally computes sector distance priorities on `WorldServerPhase6WorkerPool`, then calls the existing `WorldStorage::generateQueue()` mutation path serially.
- `phase6WorldParallelism.packetPreparationSectorPrefill` is available in `worldserver.config` and is enabled by default in the OpenStarbound game config, with explicit per-server opt-out still available.
- When enabled, `WorldServer` snapshots pending sector tile-array updates during packet preparation, optionally assembles shared sector update packets on `WorldServerPhase6WorkerPool`, then merges the packet cache before the existing per-client packet queueing path runs.
- Diagnostics report enabled worlds, planning ticks, serial ticks, parallel ticks, planned sectors, serial/parallel/merge microseconds, and fallback count through `WorldServer`, `WorldServerThread`, `UniverseServer::ServerStatus`, and `/serverstatus`.
- Packet-sector prefill diagnostics report enabled worlds, ticks, serial ticks, parallel ticks, prefilled sectors, serial/parallel/merge microseconds, and fallback count through the same status path.
- Both implemented experiments have default-enabled serial-versus-worker differential checks in the game config. The worker result is compared against the serial helper in the same tick, divergence counters are incremented on mismatch, and the serial result is used as the fallback result.
- `phase6WorldParallelism.subsystemBaselineMetrics` is available in `worldserver.config` and is enabled by default in the OpenStarbound game config. When enabled, the existing serial liquid, falling-block, wiring, entity, and Lua phases report baseline work counts without changing update order or enabling any parallel mutation.
- Focused regression coverage verifies the implemented Phase 6 flags are enabled by default in the game config, that explicit opt-out still works, that the enabled paths record planning/prefill work without changing packet output, that differential checks run without divergences for the controlled cases, that serial mutation baseline metrics are stable across repeated controlled runs, and that packet-sector prefill emits the same tile-array update payloads as the serial sector-packet path for a controlled two-client world.
- The Phase 6 release target is `26.5.1a`, `Ethereal Drake`, following the `yy.m.<sub version><a/b/d/y/NA>` release label rule.

Phase 6 diagnostics to add:

- per-experiment enabled flag, serial duration, parallel duration, merge duration, and fallback count; storage generation planning and packet-sector prefill now have this baseline instrumentation
- divergence detector counters for serial-versus-parallel comparison runs; storage generation planning and packet-sector prefill now expose default-enabled differential check counts, check time, and divergence counts
- per-subsystem work item counts: liquid active cells, falling-block pending/processed/moved positions, wiring initial/loaded/evaluated entities and network loads, entity update/tile/destroy counts, Lua context/update counts, generation sectors, packet entities; serial liquid/falling-block/wiring/entity/Lua baseline counts, storage generation planning sectors, and packet-sector prefill sectors are now reported
- config surface that can disable each experiment independently without changing save or packet formats; storage generation planning, packet-sector prefill, differential checks, and subsystem baseline metrics are default-enabled in the OpenStarbound game config with explicit opt-out flags

Phase 6 compatibility checkpoints:

- Implemented snapshot-only worker experiments, differential checks, and serial mutation baseline metrics are enabled by default in the OpenStarbound game config, but can still be disabled independently at startup.
- Serial fallback can be selected at runtime or startup for every experiment.
- Parallel liquid, falling, and wiring experiments must prove no boundary artifacts in fixed-seed differential tests before broader testing.
- Mods must continue to observe the same Lua/entity order in default mode.
- Any original gameplay mechanism compatible API must be explicit opt-in, versioned, capability-queryable from scripts, and able to fall back to legacy exact behavior when a world or mod needs it.

Acceptance criteria:

- Implemented experiment flags are default-enabled in game config and explicitly disable-able.
- Serial fallback remains available and tested.
- Lua-visible order does not change in default mode.
- New mechanism-compatible APIs are documented as phase-boundary contracts, not hidden changes to the legacy exact API.
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

## Phase 0-6 Closeout And Gate Work

The practical Phase 0-6 implementation backlog is closed for the compatibility-first slices described above. The remaining items are validation, diagnostics consumers, or future Phase 6 expansion gates that should stay visible but should not be treated as unfinished implementation work.

Closed Phase 0-6 implemented work:

1. Phase 0 diagnostics now cover universe, world-thread, world-update, network, persistence, and packet-preparation timing/counters through shared status structures and server commands.
2. Phase 1 worker sharding, wakeups, focused many-idle/ordering/remove tests, and worker-stat diagnostics are implemented; socket readiness remains future networking work.
3. Phase 2 pending handshakes are implemented behind `usePendingConnectionStateMachine` with the legacy accept-thread fallback retained for extended-matrix hardening.
4. Phase 3 immutable JSON persistence snapshots, bounded async executor, synchronous fallback, retry accounting, shutdown draining, and focused queue-pressure/shutdown-drain tests are implemented while async persistence is default-enabled in the game config with explicit opt-out.
5. Phase 4 world-thread commands, command diagnostics, ship-upgrade command/result handling, focused queued-result coverage, and system-world add/remove command plus read snapshots are implemented while compatibility wrappers remain for audited rollout.
6. Phase 5 serial snapshot/prep groundwork is implemented: owner-indexed entity updates, per-tick monitoring-region snapshot reuse, generation-priority memoization, sector packet cache, entity store cache, packet-prep counters, and focused regression tests.
7. Phase 6 storage-generation planning and packet-sector prefill are default-enabled in the game config with serial fallback diagnostics, default-enabled differential checks, divergence counters, default-enabled serial liquid/falling-block/wiring/entity/Lua baseline metrics, and focused guard/equivalence tests; deeper liquid, falling-block, wiring, entity, and Lua parallel mutation is intentionally deferred.

Post-audit hardening after default enablement or before Phase 6 expansion:

1. Run broader TCP/high-connection and high-fan-out profiling before changing socket readiness or `sendPackets` behavior.
2. Complete the remaining extended pending-handshake matrix before removing the legacy accept-thread fallback: password, duplicate UUID, asset mismatch, max-player/admin-priority, login bursts, legacy/OpenStarbound TCP paths, and password/challenge flush timeouts.
3. Validate async persistence under save/load depth, disconnect/ship-upgrade while save is pending, failure injection, old-save loading, and latency profiling before removing synchronous fallbacks or broadening async persistence scope.
4. Add `/worldstats` or equivalent per-world/system-world command diagnostics if command wrappers are removed or if system-world command detail becomes operationally important.
5. Add entity packet-equivalence tests and immutable entity serialization inputs before moving packet preparation onto worker jobs; sector tile-array packet prefill already has focused serial-equivalence coverage.
6. Keep Phase 6 experiments isolated, independently disable-able, and backed by differential tests; the first differential-check path now covers the two implemented worker helpers, and liquid/falling/wiring/entity/Lua now have serial baseline counters, but still need fixed-seed state comparisons, dependency analysis, an original gameplay mechanism compatible API contract, or a formal mod-visibility model before any parallel mutation path lands.
7. Revisit lower-level primitive cleanup after ownership boundaries are flatter: recursive mutex reduction, spinlock replacement, and `WorkerPool` backpressure improvements.
8. Update `diagnostics-debugging-roadmap.md` whenever a phase adds counters that should appear in overlays, `/serverstatus`, crash bundles, or structured logs.

Codebase-wide acceptance gates:

- Existing save files, world files, player files, packet formats, and mod-visible Lua/entity ordering remain compatible by default.
- Every phase has a fallback path until tests and profiling prove the new path is stable.
- Performance claims are tied to measurements: p50/p95/p99 tick times, idle CPU, context switches, queue depths, and packet latency.
- Crash and diagnostics output includes enough context to debug the new queues, workers, and ownership boundaries.

## First Optimization Verification Plan

The original Phase 1 verification plan is now mostly covered by focused tests and live counters. Keep this as the profiling checklist for future socket readiness or high-fan-out networking changes:

1. Configure Windows build: `cmake --preset=windows-release` from `source/`.
2. Build at least `game_tests` or the full preset: `cmake --build --preset=windows-release`.
3. Run no-asset tests: `ctest --preset=windows-release`.
4. Use `/servernetstats` and `/serverstatus` for network worker counter dumps.
5. Compare idle CPU and context switches with many connected idle clients before and after readiness changes.
6. Compare packet latency for a low-rate ping/action workload.
7. Stress add/remove/send while workers are active.

Expected result:

- Lower CPU time in network workers under many idle connections.
- Less global connection-map copying.
- Same per-client packet ordering and disconnect behavior.
- Minimal TPS change for one crowded world, because the main simulation lane remains serial.
