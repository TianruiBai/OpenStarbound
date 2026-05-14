# OpenStarbound 26.5.2a Self-Workload Capture And Compatibility Prep

Capture date: 2026-05-13

This note records the first repeatable in-process game-mechanism workload added after the queue-only networking and liquid cache rebuild-skip pass. It is not a replacement for fixed real-world captures or modpack smoke. Its job is to give the server optimization work a local, fast, repeatable mechanism load that can be run from `game_tests` while preparing the next compatibility-sensitive paths.

## Harness

New disabled test:

```text
ServerMeasurement.DISABLED_GameMechanismSelfWorkloadCapture
```

Run command from `source`:

```powershell
..\dist\game_tests.exe -bootconfig ..\scripts\windows\sbinit.config "--gtest_filter=ServerMeasurement.DISABLED_GameMechanismSelfWorkloadCapture" --gtest_also_run_disabled_tests
```

The workload builds a direct `WorldServer` fixture and exercises:

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
- storage sync and full `readChunks()` snapshot export

The fixture intentionally stays in-process. It avoids UI automation and keeps the capture stable enough for CI/manual regression runs while still using real `WorldServer` update phases and packet generation paths.

## Local Capture

2026-05-14 local result with `skipEmptyEntityUpdateSets=true` in the capture fixture:

```text
GameMechanismCapture clients=4 ticks=240 packets=23025 step=960 tileArray=100 tile=3116 liquid=16888 tileDamage=1008 entityCreate=92 entityUpdate=784 entityUpdateDeltas=4800 emptyEntityUpdate=0 entityDestroy=36 giveItem=1 failures=0 packetPrepTicks=240 regions=960/960/960/2160 sectorCache=100/0 entityStoreCache=69/23 netStateCache=8094/7506 updateSets=784/4800/0/176 sectorFanout=5280/21120/0 liquidCache=12/48/48/320/5016/20064/5016 falling=30/1216/1540/324 wiring=48/144/144/144/144 wiringDirty=144/141/3/3/3/141/3 entity=240/3932/720/9/3932/3932/45/148/13735/541 lua=240/240/240/530/15 storage=1/64/64/64/66/65/1/65/6284 chunks=65 elapsedUs=174874
```

Important reads:

- Liquid fan-out remains the largest packet class in this synthetic capture: `16888` liquid update packets and `sectorFanout=5280/21120/0`.
- The liquid cache rebuild-skip optimization is active under stable monitoring windows: `liquidCache=12/48/...`.
- Tile-update, tile-damage, and falling-block paths are now nonzero: `tile=3116`, `tileDamage=1008`, `falling=30/1216/1540/324`.
- The minimal wire-object chain gives wiring a real baseline: `wiring=48/144/144/144/144`; `wiringDirty=144/141/3/3/3/141/3` means signature checks/clean networks/dirty networks/topology-dirty networks/output-dirty networks/clean entities/dirty entities, with the full serial scan still running.
- Entity create-store sharing works across clients: `entityStoreCache=69/23`.
- Empty entity update-set suppression is measured but still opt-in: the capture emitted `784` update sets, carried `4800` deltas, emitted `0` empty update sets, and skipped `176` empty update sets. An earlier legacy-compatible run emitted `960` update sets with `4808` deltas and `176` empty update-set packets.
- Delta net-state cache now shows both reuse and misses: `netStateCache=8094/7506`. This should still be investigated, but not by blindly caching first-observation bytes.
- Entity compatibility-layer attribution is now nonzero: `entity=240/3932/720/9/3932/3932/45/148/13735/541` means ticks/updated/tile/destroyed/copied/sorted/copyUs/sortUs/updateUs/metadataUs.
- Lua compatibility-layer attribution is now nonzero for the OpenStarbound world script context: `lua=240/240/240/530/15` means ticks/contexts/updates/updateUs/maxSingleUpdateUs.
- Storage remains measurable: `storage=1/64/64/64/66/65/1/65/6284`, including a full snapshot export.

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

The capture's `netStateCache=8013/7479` makes entity replication a good research target, but the version-advancement contract is the gate.

`skipEmptyEntityUpdateSets` is a guarded packet-stream optimization. The packaged config leaves it `false` for legacy-compatible default behavior because empty update sets can represent a blank delta from a remote master. When enabled, packet prep records `entityUpdateSets=emitted/deltas/empty/skipped` diagnostics and suppresses only update-set packets with no deltas.

`skipEmptyUpdatePackets` is the analogous guarded system-world optimization. The packaged config leaves it `false`; when enabled, `SystemWorldServer` suppresses only `SystemWorldUpdatePacket`s whose object and ship update maps are both empty. A direct fixture verifies the legacy default still emits the empty no-op packet and the opt-in path skips only after a non-empty ship delta has already been delivered.

### Storage Dirty-Sector Filtering

Entry points:

- `WorldServer::sync`
- `WorldStorage::sync`
- `WorldStorage::readChunks`
- `WorldStorageTimingStats`

Preparation rule: dirty-sector filtering is safer than changing full snapshot export semantics, but dirty reasons must be explicit. Required dirty reasons include tile data, entity store, entity movement across sectors, unique entity index changes, sector-unique stores, generation writes, unload, and expiration.

The first implementation should add dirty reason counters and a serial fallback. Full `readChunks()` export reduction should wait for byte-equivalence tests because ship/disconnect snapshots are durable client-context state.

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

Current preflight slice: `mutationParallelismFixedSeedSignatures=true` now enables deterministic preflight seeds for liquid and falling-block random sources and records serial liquid active-cell/region signatures plus falling pending, processed, moved, and next-pending signatures. If liquid, falling-block, or wiring mutation flags are also requested and the dependency-analysis and mod-visibility gates are explicitly enabled, Phase 6 now runs immutable shadow-worker signature jobs and compares them with the owner-thread serial signature. This validates worker scheduling and divergence/fallback counters, not live off-thread mutation. Boundary/interactions/final-flow signatures and dependency-region classification are still required before worker mutation can graduate.

### Entity And Lua Mutation

Entry points:

- `WorldServer::update` entity phase
- `EntityMap::updateAllEntities`
- `WorldServer` script context update loop
- `LuaUpdatableComponent`

Preparation rule: entity and Lua mutation remain blocked by the implementation gate. The current safe step is attribution, not parallel mutation: entity phase metrics now split pointer-copy, sort, update callback, and metadata-refresh costs; Lua metrics now record total script-context update time and the maximum single context update time without exposing unbounded per-context names.

Research path for entity/Lua optimization:

1. Keep the legacy entity and Lua update phases serial and exact-order by default.
2. Add bounded per-entity-type and per-script-context attribution before any behavior change, so mod-heavy worlds can identify whether AI, objects, status effects, or global scripts are dominant.
3. Define mechanism-compatible script capabilities for snapshot reads, batched queries, and phase-boundary deferred writes. Legacy APIs continue to see immediate owner-thread behavior.
4. Allow worker execution only for pure read/snapshot work or explicit deferred command buffers, with serial owner-thread merge and fallback when a legacy-sensitive API is used.
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
4. Add a save/disconnect-style workload that captures repeated `sync()` plus `readChunks()` under dirty and unchanged sectors.
5. Add an A/B variant for default queue-only networking versus `queueOnlyConnectionSend=false` once the mechanism workload is lifted into a full `UniverseServer` dummy-client path.
6. Add an asset/mod startup capture that records source enumeration, patch parse/application, load-script, preload, and queued worker timings for a vanilla asset set and at least one modpack smoke set.

## Go/No-Go For Compatibility-Sensitive Work

Proceed only when the target path has:

- a repeatable capture workload that exercises the mechanism with nonzero work counters
- serial baseline counters and output signatures
- a config gate and serial fallback
- divergence counters or byte/state equivalence tests
- no packet, save, protocol, or Lua-visible behavior change unless explicitly versioned
