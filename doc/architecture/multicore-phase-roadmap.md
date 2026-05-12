# OpenStarbound Multicore Phase Roadmap

This roadmap turns `multicore-engineering-plan.md` into an execution checklist. It keeps the same compatibility-first strategy: improve multicore use around the serial world simulation lane before attempting world-internal parallel simulation.

The first implementation work has started in Phase 1 with worker-owned network connection lists and event wakeups in `source/game/StarUniverseConnection.*`.

The observability and crash-reporting work that supports these phases is tracked in `diagnostics-debugging-roadmap.md`. In short: build on the current `/debug` overlay, `LogMap`, `SpatialLogger`, `Logger`, stack traces, Lua profiles, and `/servernetstats` to provide F3-style status, server diagnostic commands, crash bundles, and structured logs.

## Research Notes

Relevant guidance from C++ and Windows threading references maps cleanly onto the current server problems:

- Prefer waitable work queues or condition variables over short sleep polling. The current network workers used a 1 ms sleep loop, so wakeable worker state is the right first step.
- Avoid high thread churn for short or bursty work. Windows thread-pool guidance specifically calls out reducing thread creation/destruction overhead; this supports replacing one thread per pending handshake with a state machine or bounded executor.
- Keep long blocking operations off latency-sensitive orchestration threads. Disk flush, compression, serialization, and slow handshakes should not sit directly in the universe update lane.
- Do not solve scheduler contention with high thread priority. Windows scheduling guidance recommends brief high-priority work only; server throughput should come from ownership, queues, batching, and reduced contention.
- Preserve per-connection and owner-thread ordering. For this codebase, that means connection packets stay ordered per client and `WorldServer` mutation stays on the owning world thread unless a snapshot boundary exists.

Sources consulted:

- Microsoft Learn, Windows thread pools: `https://learn.microsoft.com/en-us/windows/win32/procthread/thread-pools`
- Microsoft Learn, Windows scheduling priorities: `https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities`
- C++ condition-variable model: `std::condition_variable` behavior and predicate-based waiting patterns

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

Acceptance criteria:

- Login success, failure, timeout, password challenge, asset mismatch, and protocol mismatch behavior match current behavior.
- Slow clients no longer reserve a full thread until timeout.
- `maxPendingConnections` remains an upper bound for memory and work.

Primary risks:

- Timing changes can expose assumptions in client connection code.
- Compression stream enablement must occur at the same protocol point.
- Duplicate UUID resolution still needs safe interaction with the clients map.

Tests:

- Legacy and OpenStarbound handshakes.
- Timeout in every pending state.
- Password success and failure.
- Duplicate UUID as admin and non-admin.
- Login burst with deliberately slow clients.

## Phase 3: Async Persistence And Snapshot Writes

Goal: move serialization, compression, and disk write latency off universe and world hot paths.

Code areas:

- `source/game/StarUniverseServer.cpp`
- `source/game/StarServerClientContext.*`
- `source/game/StarWorldServerThread.*`
- `source/game/StarWorldStorage.*`
- `source/core/StarBTreeDatabase.*`

Implementation tasks:

1. Define immutable snapshot types for client context, ship chunks, universe settings, temp world indexes, and celestial commit requests.
2. Add a bounded persistence executor with queue-depth metrics and shutdown flushing.
3. Produce world snapshots on the world owner thread at safe barriers.
4. Produce client and universe snapshots on the universe thread.
5. Write snapshots on persistence workers.
6. Report completion and failures back to the universe thread.
7. Preserve required shutdown and disconnect flush semantics.
8. Add retry or fatal-failure policy before moving critical writes fully async.

Acceptance criteria:

- Save files remain byte-compatible or semantically compatible with current versioned data.
- Shutdown waits for required writes.
- Simulated write failures are visible and do not silently drop required state.
- Autosave and disconnect p95/p99 spikes improve or stay stable.

Primary risks:

- Writing live mutable objects from background threads would introduce races.
- Reordering critical writes can change crash-recovery behavior.
- Unbounded persistence queues can trade tick spikes for memory growth.

Tests:

- Normal save/load.
- Disconnect while save is pending.
- Ship upgrade while save is pending.
- Shutdown while writes are queued.
- Simulated write failure.
- Old save load after async-written state.

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

Acceptance criteria:

- World state mutation from outside the owner thread is removed or explicitly isolated.
- Common warps, beam up/down, ship travel, disconnect, and idle unload behave the same.
- Command queueing does not introduce unbounded one-tick delays in user-visible flows.

Primary risks:

- Moving work to tick boundaries can shift behavior by a tick.
- Generic `executeAction()` may hide script/admin behaviors that need purpose-built commands.
- Blocking promises can recreate lock coupling if used from the wrong thread.

Tests:

- Planet, ship, party ship, and instance warps.
- Disconnect during world transition.
- Idle world shutdown.
- Admin commands that touch world/player state.
- Entity messages with replies.

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

Acceptance criteria:

- Packet output remains ordered and equivalent.
- Snapshot jobs never read live mutable world state.
- World tick p95/p99 improves in packet-heavy or save-heavy scenarios.

Primary risks:

- Snapshot creation can cost more than the parallel work saves.
- Job completion waits can stall the next tick if queues are overloaded.
- Packet stream equivalence can be difficult when timing-dependent state exists.

Tests:

- Dense entity replication.
- Multiple clients entering and leaving monitored regions.
- Save-heavy world benchmark.
- Deterministic packet comparison for fixed scenarios.

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

Acceptance criteria:

- Experimental flags are off by default.
- Serial fallback remains available and tested.
- Lua-visible order does not change in default mode.
- Any save, packet, or simulation divergence is intentional and documented.

Primary risks:

- Liquid, wiring, entity, and Lua systems may have hidden order dependencies.
- Region boundaries can create edge artifacts.
- Mods can depend on behavior that looks accidental from engine code.

Tests:

- Serial-versus-parallel differential runs.
- Region boundary stress tests.
- Wiring-heavy worlds.
- Liquid-heavy worlds.
- Modded smoke tests.

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
