# OpenStarbound 26.5.2a Self-Workload Capture And Compatibility Prep

Capture date: 2026-05-13; updated 2026-05-14

This note records the first repeatable in-process game-mechanism workload added after the queue-only networking and liquid cache rebuild-skip pass. It is not a replacement for fixed real-world captures or modpack smoke. Its job is to give the server optimization work a local, fast, repeatable mechanism load that can be run from `game_tests` while preparing the next compatibility-sensitive paths.

## Harness

Disabled measurement tests:

```text
ServerMeasurement.DISABLED_GameMechanismSelfWorkloadCapture
ServerMeasurement.DISABLED_SaveDisconnectStorageCapture
```

Run commands from `source`:

```powershell
..\dist\game_tests.exe -bootconfig ..\scripts\windows\sbinit.config "--gtest_filter=ServerMeasurement.DISABLED_GameMechanismSelfWorkloadCapture" --gtest_also_run_disabled_tests
..\dist\game_tests.exe -bootconfig ..\scripts\windows\sbinit.config "--gtest_filter=ServerMeasurement.DISABLED_SaveDisconnectStorageCapture" --gtest_also_run_disabled_tests
```

The game-mechanism workload builds a direct `WorldServer` fixture and exercises:

- 4 acknowledged client windows with repeated window movement
- generated active world regions
- liquid placement and collection through incoming client packets
- direct low-damage tile hits and tile-damage update packet generation
- controlled falling-material columns with support removal and moved-block accounting
- a small wire-object chain connected through `WorldServer::wire()`
- item-drop entities observed by multiple clients
- entity create/update/destroy packet preparation, including the opt-in empty update-set suppression path
- sector packet prefill and sector fan-out accounting
- liquid no-processing-limit cache hits and rebuild skips
- falling-block baseline processing with nonzero moved blocks
- storage sync, one baseline full `readChunks()` snapshot export, and incremental chunk-update exports

The fixture intentionally stays in-process. It avoids UI automation and keeps the capture stable enough for CI/manual regression runs while still using real `WorldServer` update phases and packet generation paths.

## Local Capture

2026-05-14 local result with `skipEmptyEntityUpdateSets=true` in the capture fixture:

```text
GameMechanismCapture clients=4 ticks=240 packets=22085 step=960 tileArray=100 tile=3116 liquid=15944 tileDamage=1008 entityCreate=92 entityUpdate=784 entityUpdateDeltas=4800 emptyEntityUpdate=0 entityDestroy=40 giveItem=1 failures=0 packetPrepTicks=240 regions=960/960/960/2160 sectorCache=100/0 entityStoreCache=69/23 netStateCache=7968/7464 updateSets=784/4800/0/176 sectorFanout=5044/20176/0 liquidCache=12/48/48/320/4830/19320/4830 falling=30/1216/1539/324 wiring=48/144/144/144/144 wiringDirty=144/141/3/3/3/141/3 entity=240/3891/720/10/3891/3891/230/162/14890/622 entityTypes=object=720/720/0/1047/75,itemDrop=3171/0/10/13441/113 lua=240/240/240/240/569/12 luaContexts=OpenStarbound=240/240/240/569/12 storage=1/64/64/64/66/65/1/65/6286 dirty=32/32/3/0/32/0 dirtySync=32/0/0 dirtySnapshot=0/32 snapshotSync=32/18360 chunks=65 elapsedUs=166491
```

Important reads:

- Liquid fan-out remains the largest packet class in this synthetic capture: `15944` liquid update packets and `sectorFanout=5044/20176/0`.
- The liquid cache rebuild-skip optimization is active under stable monitoring windows: `liquidCache=12/48/...`.
- Tile-update, tile-damage, and falling-block paths are now nonzero: `tile=3116`, `tileDamage=1008`, `falling=30/1216/1539/324`.
- The minimal wire-object chain gives wiring a real baseline: `wiring=48/144/144/144/144`; `wiringDirty=144/141/3/3/3/141/3` means signature checks/clean networks/dirty networks/topology-dirty networks/output-dirty networks/clean entities/dirty entities, with the full serial scan still running.
- Entity create-store sharing works across clients: `entityStoreCache=69/23`.
- Empty entity update-set suppression is measured but still opt-in: the capture emitted `784` update sets, carried `4800` deltas, emitted `0` empty update sets, and skipped `176` empty update sets. An earlier legacy-compatible run emitted `960` update sets with `4808` deltas and `176` empty update-set packets.
- Delta net-state cache now shows both reuse and misses: `netStateCache=7968/7464`. This should still be investigated, but not by blindly caching first-observation bytes.
- Entity compatibility-layer attribution is now nonzero and split by entity type: `entity=240/3891/720/10/3891/3891/230/162/14890/622` means ticks/updated/tile/destroyed/copied/sorted/copyUs/sortUs/updateUs/metadataUs, while `entityTypes=object=720/720/0/1047/75,itemDrop=3171/0/10/13441/113` means updated/tile/destroyed/callbackUs/maxCallbackUs by type.
- Lua compatibility-layer attribution is now nonzero for the OpenStarbound world script context: `lua=240/240/240/240/569/12` means ticks/contexts/updateCalls/readyUpdates/updateUs/maxSingleUpdateUs, and `luaContexts=OpenStarbound=240/240/240/569/12` means contextTicks/updateCalls/readyUpdates/updateUs/maxUpdateUs.
- Storage remains measurable: `storage=1/64/64/64/66/65/1/65/6286` and `snapshotSync=32/18360`, separating the full snapshot export from the pre-export sector sync work.
- Dirty-sector diagnostics are active while the new dirty-sector filtering gate remains default-off: `dirty=32/32/3/0/32/0` means marked sectors/tile/entity/unique/generation/unload reason marks, `dirtySync=32/0/0` means the one explicit sync visited 32 dirty-marked sectors, 0 unmarked sectors, and skipped 0 clean sectors, and `dirtySnapshot=0/32` means the later snapshot pre-sync saw only already-flushed sectors.

## Save/Disconnect Storage Capture

2026-05-14 local result:

```text
SaveDisconnectStorageCapture dirtyTileEdits=64 syncPasses=5 snapshotExports=1 chunkUpdateExports=9 chunks=41 sync=5/100/300 entity=300/0/300/173 tile=300/9524100/63571 copy=300/9838 compress=616/9536032/45699/43246 btree=46/570/4300/41629/516 storeTypes=2/14/0:24/276/0:20/280/0:0/0/0:0/0/0 dirty=24/24/0/0/20/0 dirtySync=24/76/0 dirtySnapshot=0/200 commit=5/32539 snapshot=1/41/3176/22 snapshotSync=20/8774 chunkUpdate=9/4/0/648/271 chunkUpdateSync=180/76696 elapsedUs=164047
```

Important reads:

- The fixture now exercises repeated ordinary sync passes, one baseline full snapshot export, and repeated incremental chunk-update exports, which is closer to the current ship save/disconnect path than the earlier all-full-snapshot capture.
- `sync=5/100/300` means sync calls/sync-pass sectors/all synced sectors; the difference is the snapshot and chunk-update pre-sync work.
- `snapshot=1/41/3176/22` and `snapshotSync=20/8774` capture the initial full baseline export. `chunkUpdate=9/4/0/648/271` and `chunkUpdateSync=180/76696` show that the nine follow-up exports carried only four changed chunks and no removals while still preserving snapshot-style sector pre-sync before export.
- `btree=46/570/4300/41629/516` confirms unchanged-sector insert skipping is active while default config still visits and serializes loaded sectors.
- `storeTypes=2/14/0:24/276/0:20/280/0:0/0/0:0/0/0` means metadata/tile-sector/entity-sector/unique-index/sector-unique writes/skips/removes. In this fixture, repeated tile and entity sector serialization dominate the skip count, while unique-index surfaces stay quiet.
- `dirty=24/24/0/0/20/0`, `dirtySync=24/76/0`, and `dirtySnapshot=0/200` show that the fixture dirtied 24 sectors by tile/generation reasons, then default config serially visited both dirty and clean sectors during sync and saw only clean sectors during export pre-sync work. The third `dirtySync` value is clean sectors skipped; it stays `0` here because `storageDirtySectorFiltering` is default-off.

## Compatibility-Sensitive Prep

### Packet Preparation And Net-State

Entry points:

- `WorldServer::queueUpdatePackets`
- `WorldTickSnapshot::entityCreateCache`
- `WorldServer::m_netStateCache`
- `Entity::writeNetState`

Preparation rule: do not cache first-observation `writeNetState(0)` bytes directly. Existing tests show first writes can be byte-equivalent while still advancing top-level entity net versions. The safe path is an API split:

1. Generate first-observation bytes from immutable state without advancing visible versions.
2. Advance owner-thread per-client net versions explicitly in the existing ordering point.
3. Differential-check generated bytes against current `writeNetState(0)` output.
4. Only then consider caching or workerizing first-observation bytes.

The capture's `netStateCache=7968/7464` makes entity replication a good research target, but the version-advancement contract is the gate.

`skipEmptyEntityUpdateSets` is a guarded packet-stream optimization. The packaged config leaves it `false` for legacy-compatible default behavior because empty update sets can represent a blank delta from a remote master. When enabled, packet prep records `entityUpdateSets=emitted/deltas/empty/skipped` diagnostics and suppresses only update-set packets with no deltas.

`skipEmptyUpdatePackets` is the analogous guarded system-world optimization. The packaged config leaves it `false`; when enabled, `SystemWorldServer` suppresses only `SystemWorldUpdatePacket`s whose object and ship update maps are both empty. A direct fixture verifies the legacy default still emits the empty no-op packet and the opt-in path skips only after a non-empty ship delta has already been delivered.

### Storage Dirty-Sector Filtering

Entry points:

- `WorldServer::sync`
- `WorldStorage::sync`
- `WorldStorage::readChunks`
- `WorldStorage::readChunkUpdate`
- `WorldServerThread::readChunkUpdate`
- `ServerClientContext::applyShipChunksUpdate`
- `WorldStorageTimingStats`

Preparation rule: dirty-sector filtering is safer than changing full snapshot export semantics, but dirty reasons must be explicit. Required dirty reasons include tile data, entity store, entity movement across sectors, unique entity index changes, sector-unique stores, generation writes, unload, and expiration.

The dirty-sector implementation now records dirty reason counters and marked/unmarked/skipped ordinary sync visits, with clean-sector skipping kept behind the default-off `storageDirtySectorFiltering` gate. Full `readChunks()` export reduction has a compatibility-preserving ship/client-context path: `WorldStorage::readChunkUpdate` pre-syncs active sectors, emits changed/new chunks plus removals relative to the caller's last known chunks, and leaves full `readChunks()` available as the fallback/reference path. `UniverseServer` now uses this update path for ship persistence, disconnect, startup, and ship-upgrade chunk refreshes. `MulticorePhaseTest.ServerOptimizationShipChunkSnapshotsAreUpdateEquivalent` verifies server/client context update semantics, and `MulticorePhaseTest.ServerOptimizationWorldStorageChunkUpdateMatchesFullSnapshot` compares real dirty world updates against a full snapshot.

### Wiring Dirty-Network Tracking

Entry points:

- `WireProcessor::process`
- `WireEntity::addNodeConnection`
- `WireEntity::removeNodeConnection`
- `WorldServer::wire`

Preparation rule: a dirty wiring tracker needs stronger controlled fixtures before behavior changes. The self-workload capture now exercises a minimal wire-object chain, but a dirty-network prototype still needs stable and toggled networks that compare dirty-network output states against a full-scan serial pass and fall back on divergence.

### Liquid And Falling Blocks

Entry points:

- `LiquidCellEngine::update`
- `LiquidCellEngine::setNoProcessingLimitRegions`
- `FallingBlocksAgent::update`
- `WorldServer::modifyLiquid`
- `WorldServer::forceModifyTile`

Preparation rule: keep mutation order serial. The current liquid cache skip is safe because unchanged monitoring regions rebuild an equivalent bucket map. Any future per-cell dirty or mutation experiment needs fixed-seed signatures over active cells, boundary cells, liquid interactions, pending falling positions, processed positions, and moved blocks.

Current preflight slice: `mutationParallelismFixedSeedSignatures=true` now enables deterministic preflight seeds for liquid and falling-block random sources and records serial liquid active-cell/region signatures plus falling pending, processed, moved, and next-pending signatures. If liquid, falling-block, wiring, entity, or Lua mutation flags are also requested and the dependency-analysis and mod-visibility gates are explicitly enabled, Phase 6 now runs immutable shadow-worker signature jobs and compares them with the owner-thread serial signature. This validates worker scheduling and divergence/fallback counters, not live off-thread mutation. Boundary/interactions/final-flow signatures and dependency-region classification are still required before worker mutation can graduate.

### Entity And Lua Mutation

Entry points:

- `WorldServer::update` entity phase
- `EntityMap::updateAllEntities`
- `WorldServer` script context update loop
- `LuaUpdatableComponent`

Preparation rule: live entity and Lua mutation remain serial and exact-order. Entity and Lua requests are no longer implementation-blocked for immutable signature shadow-worker diagnostics when all explicit gates are enabled: requested subsystem flag, fixed-seed signatures, dependency-analysis gate, mod-visibility gate, and worker threads. The current safe steps are attribution and immutable scalar signature comparison, not off-thread mutation: entity phase metrics split pointer-copy, sort, update callback, and metadata-refresh costs; Lua metrics record total script-context update time and the maximum single context update time without exposing unbounded per-context names.

Research path for entity/Lua optimization:

1. Keep the legacy entity and Lua update phases serial and exact-order by default.
2. Add bounded per-entity-type and per-script-context attribution before any behavior change, so mod-heavy worlds can identify whether AI, objects, status effects, or global scripts are dominant.
3. Define mechanism-compatible script capabilities for snapshot reads, batched queries, and phase-boundary deferred writes. Legacy APIs continue to see immediate owner-thread behavior.
4. Allow worker execution only for pure read/snapshot/signature work or explicit deferred command buffers, with serial owner-thread merge and fallback when a legacy-sensitive API is used.
5. Treat any Lua API that exposes immediate mutation order, entity iteration order, random-source behavior, or world callbacks as a mod-visibility blocker until an opt-in compatibility contract exists.

### Asset And Mod Loading

Entry points:

- `Assets::Assets`
- `Assets::queueAssets`
- `Assets::workerMain`
- `Assets::applyJsonPatches`
- `Assets::applyImagePatches`

Research finding: the asset system already starts worker threads for queued asset loads and post-processing after source registration and preload setup. The higher-risk startup work is deterministic source registration, patch-source ordering, `onLoad` and `postLoad` Lua scripts, Lua patch contexts, and cache invalidation after load scripts. Mods can depend on source priority, patch order, and Lua-generated memory assets, so startup parallelism must merge results in the same source order and keep load scripts serial unless a future mod contract says otherwise.

Safe candidates:

- enumerate directory/packed source asset paths in parallel, then merge by configured source order
- parse independent JSON patch files or frame specifications into immutable intermediate data, then apply patches in existing order
- increase or tune queued asset preload breadth for images/audio/bytes where `Assets::workerMain` already owns the load/post-process queue
- add startup diagnostics for source scan time, patch parse time, load-script time, preload queue depth, worker load/post counts, and Lua patch lock time

Keep serial until a stronger contract exists:

- `onLoad` and `postLoad` Lua scripts that call `assets.add`, `assets.patch`, or `assets.erase`
- patch application order for `.patch`, `.patchlist`, numbered patches, and `.patch.lua`
- mutation of `m_files`, `m_filesByExtension`, memory asset sources, and Lua patch context caches
- any loading path that calls into Lua callbacks sharing the current `LuaEngine` and `m_luaMutex`

## Next Harness Extensions

1. Expand the wire-object fixture into stable and toggled networks before changing wiring behavior.
2. Expand Lua attribution beyond the OpenStarbound worldserver context before changing any script execution behavior.
3. Expand the falling-material fixture into a taller or cascading stress case before changing falling-block behavior.
4. Done: add a save/disconnect-style storage workload that captures repeated `sync()`, one baseline full `readChunks()` export, and incremental chunk updates under dirty and unchanged sectors, including dirty-reason counters. Future expansion should add entity/unique/unload dirty fixtures and old-save reload coverage before broadening storage behavior.
5. Add an A/B variant for default queue-only networking versus `queueOnlyConnectionSend=false` once the mechanism workload is lifted into a full `UniverseServer` dummy-client path.
6. Add an asset/mod startup capture that records source enumeration, patch parse/application, load-script, preload, and queued worker timings for a vanilla asset set and at least one modpack smoke set.

## Go/No-Go For Compatibility-Sensitive Work

Proceed only when the target path has:

- a repeatable capture workload that exercises the mechanism with nonzero work counters
- serial baseline counters and output signatures
- a config gate and serial fallback
- divergence counters or byte/state equivalence tests
- no packet, save, protocol, or Lua-visible behavior change unless explicitly versioned
