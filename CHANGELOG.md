# OpenStarbound Changelog

## 26.5.2a - Ethereal Drake alpha preview

Compared with base commit `ab4f8da2b92ab1b8dc39e339029e4dd3e0da2eab`.

This changelog covers the commits after that base plus the current release-prep worktree changes for CI packaging, macOS server artifacts, and the `26.5.2a` version bump.

### Release summary

This release is the compatibility-first server performance and diagnostics pass after the initial `26.5.1a` Phase 0-6 multicore work. The main theme is reducing hot-thread pressure without changing save formats, packet formats, Lua behavior, or gameplay-visible subsystem order by default.

The largest server changes are queue-only network sends, the pending-connection state machine, bounded async persistence snapshots, world-thread command mailbox coverage, packet-preparation caching, storage-generation planning, packet-sector prefill, storage dirty-sector diagnostics, and incremental ship chunk updates. Riskier live subsystem mutation remains serial; the new worker-side entity/Lua work is limited to immutable signature diagnostics behind explicit gates.

### Build and packaging

- Updated the GitHub Actions workflow to present itself as `Build and Package` and to cover package-script, CMake, library, installer, and workflow edits in its path filters.
- Workflow dispatch now defaults to the nightly desktop package set: Linux Clang, Windows, and macOS. Linux GCC and Linux ARM GCC remain available through the existing manual Linux input; Linux ARM Clang remains tied to the Linux Clang input.
- Existing Windows packaging continues to produce client, server, all-files/dev-only, and installer artifacts through `scripts/ci/windows/post_build.bat` and `scripts/ci/windows/assemble.bat`.
- Existing Linux packaging continues to produce separate `client.tar` and `server.tar` artifacts through `scripts/ci/linux/assemble.sh`.
- macOS packaging now produces a separate `server.tar` in addition to `client.tar`.
- macOS Intel and Apple Silicon CI jobs now upload both client and server package artifacts:
  - `OpenStarbound-macOS-Intel-Client`
  - `OpenStarbound-macOS-Intel-Server`
  - `OpenStarbound-macOS-Silicon-Client`
  - `OpenStarbound-macOS-Silicon-Server`
- The macOS server package includes packed server assets, a mods placeholder, `starbound_server`, `btree_repacker`, copied `.dylib` dependencies, `sbinit.config`, `run-server.sh`, and `steam_appid.txt`.
- README nightly links now expose the new macOS server artifacts beside the existing macOS client links.

### Versioning

- Bumped OpenStarbound release/package metadata from `26.5.1a` to `26.5.2a`:
  - `assets/opensb/_metadata`
  - title-screen version patch data
  - `StarVersion.cpp.in`
  - Windows client resource file
  - Windows server resource file
- Kept the existing `Ethereal Drake` release name for the `26.5.2a` alpha-preview line.

### Server networking

- Added queue-only connection sends behind `queueOnlyConnectionSend`.
- Defaulted queue-only sends on in OpenStarbound config while preserving `queueOnlyConnectionSend=false` as the legacy eager-write fallback.
- Added queued/eager/worker send counters so `/serverstatus` and `/servernetstats` can show whether writes are happening from the caller path or worker path.
- Added in-process dummy-client measurement coverage for eager versus queue-only fan-out behavior.
- Added a narrow core `SocketPoller` API seed with readable/writable interests, unregister, wake, timed waits, and ready socket handles.
- Covered `SocketPoller` with loopback TCP tests without wiring it into live `UniverseConnectionServer` behavior yet.
- Kept readiness-based network worker integration as a future hardening step.

### Pending connections

- Added the default-enabled `usePendingConnectionStateMachine` path for incoming client handshakes.
- Replaced the default thread-per-pending-handshake flow with explicit pending states and deadlines.
- Preserved the old blocking thread-per-handshake path as a fallback.
- Added status counters for pending handshakes by state plus accepted, finalized, rejected, and timed-out totals.
- Added focused coverage for local and remote OpenStarbound/legacy TCP success, protocol rejection, passwords, asset mismatch, duplicate UUID priority, max-player/admin priority, login bursts, and timeout paths.

### Async persistence

- Added bounded async JSON persistence behind default-enabled `useAsyncPersistence`.
- Introduced immutable universe/client/system-world/ship snapshot boundaries before worker writes.
- Added `persistenceWorkerThreads`, `maxQueuedPersistenceSnapshots`, and `maxPersistenceWriteRetries` config entries.
- Added synchronous fallback for queue pressure and write failures.
- Added shutdown drain behavior so queued autosaves cannot overwrite final shutdown state after the final synchronous saves.
- Moved celestial cleanup/commit work out from under the main universe locks where practical.
- Added `/serverstatus` persistence diagnostics for queue depth, oldest pending age, completed batches, written snapshots, failed snapshots, retries, synchronous fallback, queue-full fallback, snapshot-build time, worker-write time, and celestial commit time/count.
- Added tests for async completion, queue-full fallback, readable saved files, shutdown drain, reload of written settings, and write-failure retry accounting.

### World-thread ownership and commands

- Added a world-thread command mailbox for many external `WorldServerThread` entry points.
- Routed common synchronous world operations through named commands while preserving compatibility wrappers.
- Covered client add/remove, spawn checks, revive position, planet/weather operations, dungeon placement, flying-sky operations, container item RPC, universe flag RPC, admin/scripted client execution, ship upgrades, unload, and chunk reads.
- Added `SystemWorldServerThread` command handling for add/remove client ship mutations.
- Published system-world read snapshots for clients, ship locations, warp actions, sky parameters, and active instance worlds.
- Added command queue diagnostics for pending count, oldest age, processed count, direct count, failures, and wait time.
- Surfaced aggregate command diagnostics in `/serverstatus` and per-world/system-world command details in `/worldstats`.

### Packet preparation and replication

- Added owner-indexed entity update application.
- Added a formal per-tick `WorldTickSnapshot` packet-prep context.
- Cached generation-priority distance calculations.
- Added packet-prep caches for shared sector tile-array updates and immutable entity-create store payloads.
- Added packet-prep diagnostics for monitoring-region builds/reuse, sector cache hits/misses, entity store cache hits/misses, entity net-state cache hits/misses, update-set counts, and byte attribution.
- Added per-entity serialization counters for create-store serialization, first-observation `writeNetState(0)`, and later delta `writeNetState(version)` calls and bytes by entity type.
- Added guarded empty `EntityUpdateSetPacket` suppression behind default-off `skipEmptyEntityUpdateSets`.
- Added guarded empty `SystemWorldUpdatePacket` suppression behind default-off `skipEmptyUpdatePackets`.
- Kept both empty-packet suppression gates default-off for legacy packet behavior.
- Added focused packet-equivalence tests for controlled entity and tile-array update scenarios.

### Storage and chunk export

- Added `WorldStorageTimingStats` for sector copy, entity store, tile store, compression, B-tree insert, commit, snapshot export, snapshot pre-sync, dirty-sector activity, and incremental chunk update export.
- Added store-surface write/skip/remove attribution and dirty-sector reason/visit attribution.
- Added default-off `storageDirtySectorFiltering` with diagnostics that show dirty marked sectors, skipped clean sectors, dirty snapshot visits, and sync behavior.
- Added `WorldStorage::readChunkUpdate(...)` to export only chunk differences relative to an existing baseline.
- Routed ship persistence, disconnect, startup, client-context storage snapshot, and ship-upgrade refresh paths through incremental chunk updates instead of repeated full `readChunks()` exports where a baseline already exists.
- Added `WorldServer::readChunkUpdate(...)` and `WorldServerThread::readChunkUpdate(...)` wrappers for threaded callers.
- Added `ServerClientContext::applyShipChunksUpdate(...)` so server-held ship baselines and pending client update chunks stay aligned.
- Changed ship-upgrade result handling from full ship chunks to ship chunk updates.
- Added `chunkUpdate` and `chunkUpdateSync` timing summaries to `/serverstatus`, `/worldstats`, and `LogMap` output.
- Added tests that verify chunk updates match full snapshots and that ship chunk update application is equivalent to full replacement.

### Phase 6 worker experiments

- Default-enabled storage-generation planning and packet-sector prefill experiments with serial fallback behavior.
- Added worker-side planning/prefill metrics for enabled worlds, ticks, serial ticks, worker ticks, planned/prefilled sectors, serial time, worker time, merge time, fallback count, differential checks, and divergences.
- Kept serial-versus-worker differential checks default-enabled for the safe helper paths.
- Added default-enabled serial subsystem baseline metrics for liquid, falling blocks, wiring, entity, and Lua phases.
- Added mutation-parallelism config flags for liquid, falling blocks, wiring, entity, and Lua, all default-disabled.
- Added fixed-seed mutation signature preflight support for liquid, falling blocks, wiring, entity, and Lua.
- Added explicit mutation gates for requested subsystem, fixed-seed signatures, dependency analysis, mod visibility contract, and worker thread count.
- Advanced entity/Lua from implementation-blocked to immutable shadow-worker signature diagnostics only when every explicit gate is enabled.
- Live liquid, falling-block, wiring, entity, and Lua mutation remain on the owner thread.
- Added gate and shadow-worker diagnostics for requested subsystems, active shadow-worker subsystems, blockers, checks, divergences, fallbacks, and worker counters.

### Server hot-path optimizations

- Added sector-to-client fan-out indexing for tile, tile-damage, and liquid update queueing.
- Replaced repeated per-tile full-client scans with sector-local subscriber lookup while preserving per-client packet ordering.
- Added monitoring-region generation/reuse through `WorldTickSnapshot` so liquid and packet-prep consumers can share player-active signal regions.
- Added liquid no-limit membership caching with bucketed candidate lookup and unchanged-region rebuild skips.
- Added wiring dirty-network diagnostics and signature tracking.
- Added falling-block timing and baseline metrics.
- Added entity update phase metrics for pointer copy, sort, callback/update, metadata refresh, and per-type callback attribution.
- Added Lua script-context timing metrics, including total update time and max single-context update time without exposing unbounded per-context names.

### Diagnostics and crash reporting

- Added `StarServerTiming.hpp` as a shared bounded timing/counter foundation for server paths.
- Expanded `/serverstatus` with universe-loop timing, world-thread timing, world-update timing, network worker state, pending handshakes, persistence state, world-command totals, packet-prep summaries, storage timing, and Phase 6 helper/mutation diagnostics.
- Expanded `/worldstats` with per-world command stats, packet-prep stats, storage timings, Phase 6 helper counters, mutation gates, and system-world details.
- Added `/servernetstats` detail for per-worker owned connections, scans, stale scans, packets, callbacks, average callback time, wakeups, timed waits, idle waits, and queued/eager/worker send counts.
- Added a core diagnostics/crash-report writer that can emit version metadata, fatal context, and recent logs under `crashes/`.
- Added redaction helpers for password/token/auth text, IPv4 addresses, and common user-home path forms.
- Added recent-log capture support in the logging layer for future diagnostics bundles and crash reports.
- Added and refreshed diagnostics roadmap documentation for overlays, commands, crash bundles, redaction, and structured output.

### Client/UI/rendering

- Added client render-rate decoupling and render/update reporting.
- Added maximum FPS and VSync options to the graphics menu.
- Expanded graphics menu patching for render-frame-rate controls.
- Added title/interface config patch updates for OpenStarbound version presentation.
- Added graphics-backend roadmap documentation covering OpenGL/Vulkan direction, diagnostics, packaging, and parity gates.

### Configuration defaults

- Added or updated OpenStarbound universe-server defaults:
  - `networkWorkerThreads`
  - `queueOnlyConnectionSend=true`
  - `usePendingConnectionStateMachine=true`
  - `useAsyncPersistence=true`
  - `persistenceWorkerThreads=1`
  - `maxQueuedPersistenceSnapshots=128`
  - `maxPersistenceWriteRetries=0`
  - Lua GC tuning entries
- Added world-server Phase 6 config surface:
  - `storageGenerationPlanning=true`
  - `storageGenerationPlanningWorkerThreads=2`
  - `storageGenerationPlanningMinimumSectors=8`
  - `storageGenerationPlanningDifferentialCheck=true`
  - `packetPreparationSectorPrefill=true`
  - `packetPreparationSectorPrefillWorkerThreads=2`
  - `packetPreparationSectorPrefillMinimumSectors=8`
  - `packetPreparationSectorPrefillDifferentialCheck=true`
  - `storageDirtySectorFiltering=false`
  - `subsystemBaselineMetrics=true`
  - all live mutation-parallelism flags default `false`
  - fixed-seed/dependency/mod-visibility gates default `false`
- Added default-off packet suppression flags:
  - `skipEmptyEntityUpdateSets=false`
  - `skipEmptyUpdatePackets=false`

### Tests and validation coverage

- Added `source/test/multicore_phase_test.cpp` as the focused Phase 0-6 and `26.5.2a` optimization regression suite.
- Added and expanded coverage in:
  - `server_test.cpp`
  - `universe_connection_test.cpp`
  - `net_states_test.cpp`
  - `socket_poller_test.cpp`
  - `diagnostics_test.cpp`
  - `logging_test.cpp`
  - `item_test.cpp`
  - `json_test.cpp`
  - `StarTestUniverse.cpp`
- Added disabled measurement harnesses for repeatable server workload capture, queue-only send comparison, and save/disconnect storage capture.
- Latest local validation recorded for the server optimization slice on 2026-05-14:
  - `starbound`, `starbound_server`, and `game_tests` built from the VS 2022 developer environment.
  - Focused incremental storage/entity-Lua shadow-worker gates plus `ServerMeasurement.DISABLED_SaveDisconnectStorageCapture`: 8/8 passed.
  - Broader `MulticorePhaseTest.*:ServerTest.*:UniverseConnections.*:UniverseConnectionServer.*`: 58/58 passed.
  - Full `game_tests`: 69/69 passed.
  - `core_tests.exe --gtest_filter=NetElements.*`: 17/17 passed.
  - `git diff --check` is clean after the final CI/changelog/version edits.

### Documentation

- Added the detailed multicore phase roadmap and updated it through Phase 6.
- Added a diagnostics and debugging roadmap.
- Added a graphics backend roadmap.
- Added the `26.5.2a` server optimization roadmap.
- Added the `26.5.2a` server performance follow-up review.
- Added the `26.5.2a` self-workload capture notes.
- Updated architecture and performance-threading documents with the new ownership, diagnostics, persistence, packet-prep, and future-work boundaries.

### Compatibility notes

- Save and packet formats are not intentionally changed by the server optimization work.
- Queue-only sends are default-on, but legacy eager sends remain available with `queueOnlyConnectionSend=false`.
- Pending connection state-machine handling is default-on, but the old blocking handshake path remains available as a fallback.
- Async persistence is default-on, but synchronous fallback remains available under pressure or write failure.
- Empty entity/system update packet suppression remains default-off.
- Dirty-sector filtering remains default-off.
- Live subsystem mutation parallelism remains default-off and is not implemented for gameplay mutation; the new worker jobs are immutable diagnostics only.
- `SocketPoller` is a tested API seed, not yet the active network worker readiness backend.

### Commit inventory since `ab4f8da2b92ab1b8dc39e339029e4dd3e0da2eab`

- `153168d9` Enhance World Storage and Ship Chunk Update Mechanisms
- `6497704a` Enhance WorldStorageTimingStats and Implement Dirty Sector Filtering
- `9780b9a9` feat(mutation): Add condition for mutation parallelism worker threads in subsystem eligibility
- `72b46d98` feat(mutation): Implement Phase 6 mutation parallelism worker tracking and diagnostics
- `f8f42104` Implement Phase 6 Mutation Fixed Seed Signatures
- `c3f591a8` feat(wiring): Enhance wiring network signature tracking and diagnostics
- `81271918` feat(socket-poller): Implement SocketPoller class for managing socket interests and polling
- `a5639cd0` feat(mutation): Implement phase 6 mutation parallelism tracking and diagnostics
- `4d0d5216` feat(optimization): Enhance entity update metrics and diagnostics across server components
- `d74c8cf3` Implement entity update set suppression and packet optimization
- `cfb3e3f9` Add server performance measurement and workload capture for optimization
- `af7816fc` feat(network): Implement queue-only connection send feature with metrics tracking
- `5d1e330b` Enhance World Storage Timing Metrics and Diagnostics
- `86913103` feat(optimization): Add entity serialization metrics to packet preparation diagnostics
- `ab7a87d2` Refactor WorldServer for improved sector client fan-out and liquid processing
- `67c75f92` feat(tests): Enable pending connection state machine for remote client acceptance and add status assertions
- `61296185` Enhance WorldServer and CommandProcessor with Mutation Parallelism Metrics
- `65491545` feat(multicore): Add world stats command for detailed per-world diagnostics
- `b320b917` feat(multicore): Enhance Phase 6 diagnostics with entity and Lua metrics tracking
- `192b75da` feat(multicore): Enable default settings for Phase 6 storage generation planning and packet sector prefill with diagnostics
- `1ca92bf4` feat(multicore): Enhance Phase 6 with differential checks and subsystem metrics
- `b4225ecf` Implement pending connection state wait limit and enhance Phase 6 packet sector prefill diagnostics
- `b82caae6` Update version to 26.5.1a "Ethereal Drake" and implement Phase 6 packet preparation sector prefill with diagnostics
- `9290d7bf` Implement Phase 6 storage generation planning with parallelism support and diagnostics
- `19452671` Enhance multicore phase roadmap with detailed implementation updates and diagnostics improvements across phases 0 to 5
- `de0d36fb` Refactor applyShipUpgrades method to pass speciesShips by const reference for improved performance
- `15be287e` Add server timing and packet preparation statistics to WorldServer
- `f87b6f0f` Enhance world command handling and server status reporting with new command statistics and improved command execution methods
- `4a47af31` Enhance WorldClient and WorldServer with command execution framework and entity management improvements
- `aa3d21d3` Bump version to 0.1.15.0 and enhance server status reporting with universe timing metrics
- `335ac831` Enhance persistence system with max write retries and improved server status reporting
- `3cd1191c` Implement async persistence with bounded snapshot queue and enhance server status reporting
- `cb5f2c05` Implement pending connection state machine and enhance server status reporting
- `f6971b92` Add maximum FPS and VSync options to graphics menu configuration
- `693917b7` Add client render rate decoupling and reporting for improved performance
- `1aa40355` Implement diagnostics and crash reporting features with redaction for sensitive data
- `79584ee5` Add diagnostics and debugging features
- `a161567a` Add server network statistics command and enhance worker stats tracking
- `fa00bdd9` Add multicore phase roadmap and enhance UniverseConnectionServer with worker states