#include "StarWorldStorage.hpp"
#include "StarFile.hpp"
#include "StarCompression.hpp"
#include "StarJsonExtra.hpp"
#include "StarDataStreamExtra.hpp"
#include "StarIterator.hpp"
#include "StarLogging.hpp"
#include "StarRoot.hpp"
#include "StarEntityMap.hpp"
#include "StarEntityFactory.hpp"
#include "StarAssets.hpp"
#include "StarMaterialDatabase.hpp"
#include "StarLiquidsDatabase.hpp"
#include "StarTime.hpp"

namespace Star {

void WorldStorageTimingStats::add(WorldStorageTimingStats const& stats) {
  syncs += stats.syncs;
  syncedSectors += stats.syncedSectors;
  syncPassSectors += stats.syncPassSectors;
  entityStoreSectors += stats.entityStoreSectors;
  entityStoreEntities += stats.entityStoreEntities;
  entityStoreBytes += stats.entityStoreBytes;
  entityStoreMicroseconds += stats.entityStoreMicroseconds;
  tileStoreSectors += stats.tileStoreSectors;
  tileStoreBytes += stats.tileStoreBytes;
  tileStoreMicroseconds += stats.tileStoreMicroseconds;
  sectorCopies += stats.sectorCopies;
  sectorCopyMicroseconds += stats.sectorCopyMicroseconds;
  compressionCalls += stats.compressionCalls;
  compressionInputBytes += stats.compressionInputBytes;
  compressionOutputBytes += stats.compressionOutputBytes;
  compressionMicroseconds += stats.compressionMicroseconds;
  btreeInserts += stats.btreeInserts;
  btreeInsertBytes += stats.btreeInsertBytes;
  btreeInsertMicroseconds += stats.btreeInsertMicroseconds;
  btreeInsertSkips += stats.btreeInsertSkips;
  btreeInsertSkipBytes += stats.btreeInsertSkipBytes;
  metadataWrites += stats.metadataWrites;
  metadataWriteSkips += stats.metadataWriteSkips;
  metadataRemoves += stats.metadataRemoves;
  tileSectorWrites += stats.tileSectorWrites;
  tileSectorWriteSkips += stats.tileSectorWriteSkips;
  tileSectorRemoves += stats.tileSectorRemoves;
  entitySectorWrites += stats.entitySectorWrites;
  entitySectorWriteSkips += stats.entitySectorWriteSkips;
  entitySectorRemoves += stats.entitySectorRemoves;
  uniqueIndexWrites += stats.uniqueIndexWrites;
  uniqueIndexWriteSkips += stats.uniqueIndexWriteSkips;
  uniqueIndexRemoves += stats.uniqueIndexRemoves;
  sectorUniqueWrites += stats.sectorUniqueWrites;
  sectorUniqueWriteSkips += stats.sectorUniqueWriteSkips;
  sectorUniqueRemoves += stats.sectorUniqueRemoves;
  dirtyMarkedSectors += stats.dirtyMarkedSectors;
  dirtyTileSectorMarks += stats.dirtyTileSectorMarks;
  dirtyEntitySectorMarks += stats.dirtyEntitySectorMarks;
  dirtyUniqueSectorMarks += stats.dirtyUniqueSectorMarks;
  dirtyGenerationSectorMarks += stats.dirtyGenerationSectorMarks;
  dirtyUnloadSectorMarks += stats.dirtyUnloadSectorMarks;
  dirtySyncMarkedSectors += stats.dirtySyncMarkedSectors;
  dirtySyncUnmarkedSectors += stats.dirtySyncUnmarkedSectors;
  dirtySyncSkippedSectors += stats.dirtySyncSkippedSectors;
  dirtySnapshotMarkedSectors += stats.dirtySnapshotMarkedSectors;
  dirtySnapshotUnmarkedSectors += stats.dirtySnapshotUnmarkedSectors;
  commits += stats.commits;
  commitMicroseconds += stats.commitMicroseconds;
  fullSnapshotExports += stats.fullSnapshotExports;
  fullSnapshotSyncSectors += stats.fullSnapshotSyncSectors;
  fullSnapshotSyncMicroseconds += stats.fullSnapshotSyncMicroseconds;
  fullSnapshotChunks += stats.fullSnapshotChunks;
  fullSnapshotBytes += stats.fullSnapshotBytes;
  fullSnapshotExportMicroseconds += stats.fullSnapshotExportMicroseconds;
  chunkUpdateExports += stats.chunkUpdateExports;
  chunkUpdateSyncSectors += stats.chunkUpdateSyncSectors;
  chunkUpdateSyncMicroseconds += stats.chunkUpdateSyncMicroseconds;
  chunkUpdateChunks += stats.chunkUpdateChunks;
  chunkUpdateRemovedChunks += stats.chunkUpdateRemovedChunks;
  chunkUpdateBytes += stats.chunkUpdateBytes;
  chunkUpdateExportMicroseconds += stats.chunkUpdateExportMicroseconds;
}

WorldChunks WorldStorage::getWorldChunksUpdate(WorldChunks const& oldChunks, WorldChunks const& newChunks) {
  WorldChunks update;
  for (auto const& p : oldChunks) {
    if (!newChunks.contains(p.first))
      update[p.first] = {};
  }

  for (auto const& p : newChunks) {
    if (oldChunks.value(p.first) != p.second)
      update[p.first] = p.second;
  }
  return update;
}

void WorldStorage::applyWorldChunksUpdateToFile(String const& file, WorldChunks const& update) {
  BTreeDatabase db;
  openDatabase(db, File::open(file, IOMode::ReadWrite));

  for (auto const& p : update) {
    if (p.second)
      db.insert(p.first, *p.second);
    else
      db.remove(p.first);
  }
}

WorldChunks WorldStorage::getWorldChunksFromFile(String const& file) {
  BTreeDatabase db;
  openDatabase(db, File::open(file, IOMode::Read));

  WorldChunks chunks;
  db.forAll([&chunks](ByteArray key, ByteArray value) { chunks.add(std::move(key), std::move(value)); });
  return chunks;
}

WorldStorage::WorldStorage(Vec2U const& worldSize, IODevicePtr const& device, WorldGeneratorFacadePtr const& generatorFacade)
  : WorldStorage() {
  Logger::info("N3DS WorldStorage: constructor begin size {}", worldSize);
  m_tileArray = make_shared<ServerTileSectorArray>(worldSize);
  Logger::info("N3DS WorldStorage: tile array ready");
  m_entityMap = make_shared<EntityMap>(worldSize, MinServerEntityId, MaxServerEntityId);
  Logger::info("N3DS WorldStorage: entity map ready");
  m_generatorFacade = generatorFacade;
  Logger::info("N3DS WorldStorage: generator facade ready");
  m_floatingDungeonWorld = false;

  // Creating a new world, clear any existing data.
  Logger::info("N3DS WorldStorage: resize begin");
  device->resize(0);
  Logger::info("N3DS WorldStorage: resize complete");

  Logger::info("N3DS WorldStorage: open database begin");
  openDatabase(m_db, device);
  Logger::info("N3DS WorldStorage: open database complete");

  Logger::info("N3DS WorldStorage: metadata insert begin");
  m_db.insert(metadataKey(), writeWorldMetadata(WorldMetadataStore{worldSize, VersionedJson()}));
  Logger::info("N3DS WorldStorage: metadata insert complete");
  m_db.commit();
  Logger::info("N3DS WorldStorage: metadata commit complete");
}

WorldStorage::WorldStorage(IODevicePtr const& device, WorldGeneratorFacadePtr const& generatorFacade) : WorldStorage() {
  m_generatorFacade = generatorFacade;
  m_floatingDungeonWorld = false;

  openDatabase(m_db, device);

  Vec2U worldSize = readWorldMetadata(*m_db.find(metadataKey())).worldSize;
  m_tileArray = make_shared<ServerTileSectorArray>(worldSize);
  m_entityMap = make_shared<EntityMap>(worldSize, MinServerEntityId, MaxServerEntityId);
}

WorldStorage::WorldStorage(WorldChunks const& chunks, WorldGeneratorFacadePtr const& generatorFacade) : WorldStorage() {
  m_generatorFacade = generatorFacade;
  m_floatingDungeonWorld = false;

  openDatabase(m_db, File::ephemeralFile());

  for (auto const& p : chunks) {
    if (p.second)
      m_db.insert(p.first, *p.second);
  }

  Vec2U worldSize = readWorldMetadata(*m_db.find(metadataKey())).worldSize;
  m_tileArray = make_shared<ServerTileSectorArray>(worldSize);
  m_entityMap = make_shared<EntityMap>(worldSize, MinServerEntityId, MaxServerEntityId);
}

WorldStorage::~WorldStorage() {
  if (m_db.isOpen()) {
    unloadAll(true);
    m_db.close();
  }
}

VersionedJson WorldStorage::worldMetadata() {
  return readWorldMetadata(*m_db.find(metadataKey())).userMetadata;
}

void WorldStorage::setWorldMetadata(VersionedJson const& metadata) {
  insertStoredValue(StoreType::Metadata, metadataKey(), writeWorldMetadataTracked({Vec2U(m_tileArray->size()), metadata}));
}

ServerTileSectorArrayPtr const& WorldStorage::tileArray() const {
  return m_tileArray;
}

EntityMapPtr const& WorldStorage::entityMap() const {
  return m_entityMap;
}

Maybe<WorldStorage::Sector> WorldStorage::sectorForPosition(Vec2I const& position) const {
  auto s = m_tileArray->sectorFor(position);
  if (m_tileArray->sectorValid(s))
    return s;
  return {};
}

List<WorldStorage::Sector> WorldStorage::sectorsForRegion(RectI const& region) const {
  return m_tileArray->validSectorsFor(region);
}

Maybe<RectI> WorldStorage::regionForSector(Sector sector) const {
  if (m_tileArray->sectorValid(sector))
    return m_tileArray->sectorRegion(sector);
  return {};
}

void WorldStorage::markSectorDirty(Sector const& sector, uint32_t reasonMask) {
  if (!m_tileArray->sectorValid(sector) || reasonMask == 0)
    return;

  auto& reasons = m_dirtySectorReasons[sector];
  uint32_t newReasons = reasonMask & ~reasons;
  if (newReasons == 0)
    return;

  if (reasons == 0)
    m_storageTimingStats.dirtyMarkedSectors += 1;
  reasons |= reasonMask;

  if (newReasons & TileDirtySectorReason)
    m_storageTimingStats.dirtyTileSectorMarks += 1;
  if (newReasons & EntityDirtySectorReason)
    m_storageTimingStats.dirtyEntitySectorMarks += 1;
  if (newReasons & UniqueDirtySectorReason)
    m_storageTimingStats.dirtyUniqueSectorMarks += 1;
  if (newReasons & GenerationDirtySectorReason)
    m_storageTimingStats.dirtyGenerationSectorMarks += 1;
  if (newReasons & UnloadDirtySectorReason)
    m_storageTimingStats.dirtyUnloadSectorMarks += 1;
}

void WorldStorage::markPositionDirty(Vec2I const& position, uint32_t reasonMask) {
  if (auto sector = sectorForPosition(position))
    markSectorDirty(*sector, reasonMask);
}

void WorldStorage::setDirtySectorFiltering(bool enabled) {
  m_dirtySectorFilteringEnabled = enabled;
  if (!enabled)
    m_dirtySectorFilteringPrimed = false;
}

SectorLoadLevel WorldStorage::sectorLoadLevel(Sector sector) const {
  return m_sectorMetadata.value(sector).loadLevel;
}

Maybe<SectorGenerationLevel> WorldStorage::sectorGenerationLevel(Sector sector) const {
  if (auto p = m_sectorMetadata.ptr(sector))
    return p->generationLevel;
  return {};
}

bool WorldStorage::sectorActive(Sector sector) const {
  if (auto p = m_sectorMetadata.ptr(sector)) {
    if (p->loadLevel == SectorLoadLevel::Loaded && p->generationLevel == SectorGenerationLevel::Complete)
      return true;
  }
  return false;
}

void WorldStorage::loadSector(Sector sector) {
  try {
    loadSectorToLevel(sector, SectorLoadLevel::Loaded);
    setSectorTimeToLive(sector, randomizedSectorTTL());
  } catch (std::exception const& e) {
    m_db.rollback();
    m_db.close();
    throw WorldStorageException(strf("Failed to load sector {}", sector), e);
  }
}

void WorldStorage::activateSector(Sector sector) {
  try {
    generateSectorToLevel(sector, SectorGenerationLevel::Complete);
    setSectorTimeToLive(sector, randomizedSectorTTL());
  } catch (std::exception const& e) {
    m_db.rollback();
    m_db.close();
    throw WorldStorageException(strf("Failed to load sector {}", sector), e);
  }
}

void WorldStorage::activateDefaultSector(Sector sector) {
  try {
#ifdef STAR_PLATFORM_N3DS
    if (!m_tileArray->sectorValid(sector))
      return;

    if (!m_tileArray->sectorLoaded(sector))
      m_tileArray->loadSector(sector, make_unique<ServerTileSectorArray::Array>());
    auto& metadata = m_sectorMetadata[sector];
    metadata.loadLevel = SectorLoadLevel::Loaded;
    metadata.generationLevel = SectorGenerationLevel::Complete;
    metadata.timeToLive = randomizedSectorTTL();
    markSectorDirty(sector, TileDirtySectorReason | GenerationDirtySectorReason);
#else
    activateSector(sector);
#endif
  } catch (std::exception const& e) {
    m_db.rollback();
    m_db.close();
    throw WorldStorageException(strf("Failed to load default sector {}", sector), e);
  }
}

void WorldStorage::queueSectorActivation(Sector sector) {
  if (auto p = m_sectorMetadata.ptr(sector)) {
    p->timeToLive = randomizedSectorTTL();
    // Don't bother queueing the sector if it is already fully loaded
    if (p->loadLevel == SectorLoadLevel::Loaded && p->generationLevel == SectorGenerationLevel::Complete)
      return;
  }

  auto p = m_generationQueue.insert(sector, m_generationQueueTimeToLive);
  m_generationQueue.toFront(p.first);
}

void WorldStorage::triggerTerraformSector(Sector sector) {
  try {
    loadSectorToLevel(sector, SectorLoadLevel::Loaded);
    if (auto p = m_sectorMetadata.ptr(sector)) {
      if (p->generationLevel < SectorGenerationLevel::Complete)
        generateSectorToLevel(sector, SectorGenerationLevel::Complete);

      p->generationLevel = SectorGenerationLevel::Terraform;
    } else {
      throw WorldStorageException(strf("Couldn't flag sector {} for terraforming; metadata unavailable", sector));
    }
  } catch (std::exception const& e) {
    m_db.rollback();
    m_db.close();
    throw WorldStorageException(strf("Failed to terraform sector {}", sector), e);
  }
}

RpcPromise<Vec2I> WorldStorage::enqueuePlacement(List<BiomeItemDistribution> distributions, Maybe<DungeonId> id) {
  return m_generatorFacade->enqueuePlacement(std::move(distributions), id);
}

Maybe<float> WorldStorage::sectorTimeToLive(Sector sector) const {
  if (auto p = m_sectorMetadata.ptr(sector))
    return p->timeToLive;
  return {};
}

bool WorldStorage::setSectorTimeToLive(Sector sector, float newTimeToLive) {
  if (auto p = m_sectorMetadata.ptr(sector)) {
    p->timeToLive = newTimeToLive;
    return true;
  }
  return false;
}

Maybe<Vec2F> WorldStorage::findUniqueEntity(String const& uniqueId) {
  if (auto entity = m_entityMap->entity(m_entityMap->uniqueEntityId(uniqueId)))
    return entity->position();

  // Only return the unique index entry for the entity IF that stored sector is
  // not loaded, if the stored sector is loaded then the entity ought to have
  // been in the live entity map.
  if (auto sectorAndPosition = getUniqueIndexEntry(uniqueId)) {
    if (m_sectorMetadata.value(sectorAndPosition->first).loadLevel < SectorLoadLevel::Entities)
      return sectorAndPosition->second;
  }

  return {};
}

EntityId WorldStorage::loadUniqueEntity(String const& uniqueId) {
  EntityId entityId = m_entityMap->uniqueEntityId(uniqueId);
  if (entityId != NullEntityId)
    return entityId;

  if (auto sectorAndPosition = getUniqueIndexEntry(uniqueId)) {
    loadSector(sectorAndPosition->first);
    return m_entityMap->uniqueEntityId(uniqueId);
  }

  return {};
}

List<WorldStorage::Sector> WorldStorage::generationQueueSectors() const {
  List<Sector> sectors;
  sectors.reserve(m_generationQueue.size());
  for (auto const& queuedSector : m_generationQueue)
    sectors.append(queuedSector.first);
  return sectors;
}

void WorldStorage::generateQueue(Maybe<size_t> sectorGenerationLevelLimit, function<bool(Sector, Sector)> sectorOrdering) {
  try {
    if (sectorOrdering) {
      m_generationQueue.sort([&sectorOrdering](auto const& a, auto const& b) {
          return sectorOrdering(a.first, b.first);
        });
    }

    while (!m_generationQueue.empty()) {
      if (sectorGenerationLevelLimit && *sectorGenerationLevelLimit == 0)
        break;

      auto p = generateSectorToLevel(m_generationQueue.firstKey(), SectorGenerationLevel::Complete, sectorGenerationLevelLimit.value(NPos));
      if (p.first)
        m_generationQueue.removeFirst();
      if (sectorGenerationLevelLimit)
        *sectorGenerationLevelLimit -= p.second;
    }
  } catch (std::exception const& e) {
    m_db.rollback();
    m_db.close();
    throw WorldStorageException("WorldStorage generation failed while generating from queue", e);
  }
}

void WorldStorage::tick(float dt, String const* worldId) {
  try {
    // Tick down generation queue entries, and erase any that are expired.
    eraseWhere(m_generationQueue, [dt](auto& p) {
        p.second -= dt;
        return p.second <= 0.0f;
      });

    // Tick down sector TTL values
    for (auto& p : m_sectorMetadata)
      p.second.timeToLive -= dt;

    // Loop over every loaded sector, figure out whether the sector needs to be
    // unloaded, kept alive by a keep-alive entity, or has any entities that need
    // to be stored because they moved into an entity-unloaded sector (zombies).
    auto entityFactory = Root::singleton().entityFactory();
    unsigned unloaded = 0, skipped = 0;
    for (auto const& p : m_sectorMetadata.pairs()) {
      auto const& sector = p.first;
      auto const& metadata = p.second;

      bool needsUnload = metadata.timeToLive <= 0.0f;

      // If it is not time to unload the sector, then we don't need to scan for
      // keep-alive entities.  If the sector is fully loaded, it can not have any
      // zombie entities.  If both of these are true, there is no work to do.
      if (!needsUnload && metadata.loadLevel == SectorLoadLevel::Entities)
        continue;

      bool keepAlive = false;
      List<EntityPtr> zombieEntities;
      m_entityMap->forEachEntity(RectF(m_tileArray->sectorRegion(sector)), [&](EntityPtr const& entity) {
          if (belongsInSector(sector, entity->position())) {
            if (!keepAlive && m_generatorFacade->entityKeepAlive(this, entity))
              keepAlive = true;
            else if (metadata.loadLevel < SectorLoadLevel::Entities)
              zombieEntities.append(entity);
          }
        });

      if (keepAlive) {
        setSectorTimeToLive(sector, randomizedSectorTTL());
      } else if (needsUnload) {
        (unloadSectorToLevel(sector, SectorLoadLevel::None) ? unloaded : skipped)++;
      } else if (!zombieEntities.empty()) {
        List<EntityPtr> zombiesToStore;
        List<EntityPtr> zombiesToRemove;
        for (auto const& entity : zombieEntities) {
          if (m_generatorFacade->entityPersistent(this, entity))
            zombiesToStore.append(entity);
          else
            zombiesToRemove.append(entity);
        }

        for (auto const& entity : zombiesToRemove) {
          m_entityMap->removeEntity(entity->entityId());
          m_generatorFacade->destructEntity(this, entity);
        }

        if (!zombiesToStore.empty()) {
          EntitySectorStore sectorStore;
          if (auto res = m_db.find(entitySectorKey(sector)))
            sectorStore = readEntitySector(*res);

          UniqueIndexStore storedUniques;
          auto entityStoreStart = Time::monotonicMicroseconds();
          for (auto const& entity : zombiesToStore) {
            m_entityMap->removeEntity(entity->entityId());
            m_generatorFacade->destructEntity(this, entity);
            if (auto uniqueId = entity->uniqueId())
              storedUniques.add(*uniqueId, {sector, entity->position()});
            sectorStore.append(entityFactory->storeVersionedEntity(entity));
          }
          m_storageTimingStats.entityStoreSectors += 1;
          m_storageTimingStats.entityStoreEntities += zombiesToStore.size();
          m_storageTimingStats.entityStoreMicroseconds += Time::monotonicMicroseconds() - entityStoreStart;
          markSectorDirty(sector, EntityDirtySectorReason | UniqueDirtySectorReason);
          insertStoredValue(StoreType::EntitySector, entitySectorKey(sector), writeEntitySectorTracked(sectorStore));
          mergeSectorUniques(sector, storedUniques);
        }
      }
    }
    if (worldId) {
      LogMap::set(strf("server_{}_storage", *worldId),
        strf("{} active, {}/{} unloaded ({} held)", m_sectorMetadata.size(), unloaded, skipped + unloaded, skipped));
    }
  } catch (std::exception const& e) {
    m_db.rollback();
    m_db.close();
    throw WorldStorageException("WorldStorage exception during tick", e);
  }
}

void WorldStorage::unloadAll(bool force) {
  try {
    auto storageConfig = Root::singleton().assets()->json("/worldstorage.config");
    auto sectors = m_sectorMetadata.keys();

    // Entities can do some strange things during unload, such as repeatedly
    // creating new entities during uninit, or setting their bounding box null
    // or being entirely outside of the world geometry.  This limits the number
    // of tries to completely uninit and store all entities before giving up
    // and just letting some entities not be stored.
    if (m_entityMap) {
      unsigned forceUnloadTries = storageConfig.getUInt("forceUnloadTries");
      for (unsigned i = 0; i < forceUnloadTries; ++i) {
        for (auto& sector : sectors)
          unloadSectorToLevel(sector, SectorLoadLevel::Tiles, force);

        if (!force || m_entityMap->size() == 0)
          break;
      }
    }
    for (auto& sector : sectors)
      unloadSectorToLevel(sector, SectorLoadLevel::None, force);

  } catch (std::exception const& e) {
    m_db.rollback();
    m_db.close();
    throw WorldStorageException("WorldStorage exception during unload", e);
  }
}

void WorldStorage::sync() {
  try {
    m_storageTimingStats.syncs += 1;
    bool filterCleanSectors = m_dirtySectorFilteringEnabled && m_dirtySectorFilteringPrimed;
    for (auto const& pair : m_sectorMetadata) {
      m_storageTimingStats.syncPassSectors += 1;
      recordDirtySectorVisit(pair.first, false);
      if (filterCleanSectors && !m_dirtySectorReasons.contains(pair.first)) {
        m_storageTimingStats.dirtySyncSkippedSectors += 1;
        continue;
      }
      syncSector(pair.first);
    }
    commitStoredValues();
    clearDirtySectorMarks();
    if (m_dirtySectorFilteringEnabled)
      m_dirtySectorFilteringPrimed = true;
  } catch (std::exception const& e) {
    m_db.rollback();
    m_db.close();
    throw WorldStorageException("WorldStorage exception during sync", e);
  }
}

WorldChunks WorldStorage::readChunks() {
  try {
    uint64_t snapshotSyncSectors = 0;
    uint64_t snapshotSyncMicroseconds = 0;
    syncActiveSectorsForSnapshot(snapshotSyncSectors, snapshotSyncMicroseconds);
    m_storageTimingStats.fullSnapshotSyncSectors += snapshotSyncSectors;
    m_storageTimingStats.fullSnapshotSyncMicroseconds += snapshotSyncMicroseconds;

    WorldChunks chunks;
    auto exportStart = Time::monotonicMicroseconds();
    m_db.forAll([&chunks](ByteArray k, ByteArray v) {
        chunks.add(std::move(k), std::move(v));
      });
    m_storageTimingStats.fullSnapshotExports += 1;
    m_storageTimingStats.fullSnapshotExportMicroseconds += Time::monotonicMicroseconds() - exportStart;
    m_storageTimingStats.fullSnapshotChunks += chunks.size();
    for (auto const& chunk : chunks) {
      m_storageTimingStats.fullSnapshotBytes += chunk.first.size();
      if (chunk.second)
        m_storageTimingStats.fullSnapshotBytes += chunk.second->size();
    }

    return WorldChunks(chunks);

  } catch (std::exception const& e) {
    m_db.rollback();
    m_db.close();
    throw WorldStorageException("WorldStorage exception during readChunks", e);
  }
}

WorldChunks WorldStorage::readChunkUpdate(WorldChunks const& oldChunks) {
  try {
    uint64_t snapshotSyncSectors = 0;
    uint64_t snapshotSyncMicroseconds = 0;
    syncActiveSectorsForSnapshot(snapshotSyncSectors, snapshotSyncMicroseconds);
    m_storageTimingStats.chunkUpdateSyncSectors += snapshotSyncSectors;
    m_storageTimingStats.chunkUpdateSyncMicroseconds += snapshotSyncMicroseconds;

    WorldChunks update;
    HashSet<ByteArray> seenKeys;
    auto exportStart = Time::monotonicMicroseconds();
    m_db.forAll([&](ByteArray key, ByteArray value) {
        auto oldValue = oldChunks.ptr(key);
        if (!oldValue || !*oldValue || **oldValue != value)
          update[key] = value;
        seenKeys.add(std::move(key));
      });

    for (auto const& oldChunk : oldChunks) {
      if (!seenKeys.contains(oldChunk.first))
        update[oldChunk.first] = {};
    }

    m_storageTimingStats.chunkUpdateExports += 1;
    m_storageTimingStats.chunkUpdateExportMicroseconds += Time::monotonicMicroseconds() - exportStart;
    m_storageTimingStats.chunkUpdateChunks += update.size();
    for (auto const& chunk : update) {
      m_storageTimingStats.chunkUpdateBytes += chunk.first.size();
      if (chunk.second)
        m_storageTimingStats.chunkUpdateBytes += chunk.second->size();
      else
        m_storageTimingStats.chunkUpdateRemovedChunks += 1;
    }

    return update;

  } catch (std::exception const& e) {
    m_db.rollback();
    m_db.close();
    throw WorldStorageException("WorldStorage exception during readChunkUpdate", e);
  }
}

WorldStorageTimingStats WorldStorage::storageTimingStats() const {
  return m_storageTimingStats;
}

bool WorldStorage::floatingDungeonWorld() const {
  return m_floatingDungeonWorld;
}

void WorldStorage::setFloatingDungeonWorld(bool floatingDungeonWorld) {
  m_floatingDungeonWorld = floatingDungeonWorld;
}

WorldStorage::TileSectorStore::TileSectorStore()
  : tileSerializationVersion(ServerTile::CurrentSerializationVersion) {}

WorldStorage::SectorMetadata::SectorMetadata()
  : loadLevel(SectorLoadLevel::None), generationLevel(SectorGenerationLevel::None), timeToLive(0.0f) {}

ByteArray WorldStorage::metadataKey() {
  DataStreamBuffer metadata(5);
  metadata.write(StoreType::Metadata);
  return metadata.takeData();
}

WorldStorage::WorldMetadataStore WorldStorage::readWorldMetadata(ByteArray const& data) {
#ifdef STAR_PLATFORM_N3DS
  DataStreamBuffer ds(data);
#else
  DataStreamBuffer ds(uncompressData(data));
#endif

  WorldMetadataStore metadata;
  ds.read(metadata.worldSize);
  ds.read(metadata.userMetadata);
  VersionedJson::readSubVersioning(ds, metadata.userMetadata);
  return metadata;
}

ByteArray WorldStorage::writeWorldMetadata(WorldMetadataStore const& metadata) {
#ifdef STAR_PLATFORM_N3DS
  return serializeWorldMetadata(metadata);
#else
  return compressData(serializeWorldMetadata(metadata));
#endif
}

ByteArray WorldStorage::serializeWorldMetadata(WorldMetadataStore const& metadata) {
  DataStreamBuffer ds;

  ds.write(metadata.worldSize);
  ds.write(metadata.userMetadata);
  VersionedJson::writeSubVersioning(ds, metadata.userMetadata);
  return ds.takeData();
}

ByteArray WorldStorage::entitySectorKey(Sector const& sector) {
  DataStreamBuffer ds(5);
  ds.write(StoreType::EntitySector);
  ds.cwrite<uint16_t>(sector[0]);
  ds.cwrite<uint16_t>(sector[1]);
  return ds.takeData();
}

WorldStorage::EntitySectorStore WorldStorage::readEntitySector(ByteArray const& data) {
#ifdef STAR_PLATFORM_N3DS
  DataStreamBuffer ds(data);
#else
  DataStreamBuffer ds(uncompressData(data));
#endif
  auto store = ds.read<EntitySectorStore>();
  for (auto& entity : store) {
    VersionedJson::readSubVersioning(ds, entity);
  }
  return store;
}

ByteArray WorldStorage::writeEntitySector(EntitySectorStore const& store) {
#ifdef STAR_PLATFORM_N3DS
  return serializeEntitySector(store);
#else
  return compressData(serializeEntitySector(store));
#endif
}

ByteArray WorldStorage::serializeEntitySector(EntitySectorStore const& store) {
  DataStreamBuffer ds;
  ds.write(store);
  for (auto& entity : store) {
    VersionedJson::writeSubVersioning(ds, entity);
  }
  return ds.takeData();
}

ByteArray WorldStorage::tileSectorKey(Sector const& sector) {
  DataStreamBuffer ds(5);
  ds.write(StoreType::TileSector);
  ds.cwrite<uint16_t>(sector[0]);
  ds.cwrite<uint16_t>(sector[1]);
  return ds.takeData();
}

WorldStorage::TileSectorStore WorldStorage::readTileSector(ByteArray const& data) {
#ifdef STAR_PLATFORM_N3DS
  DataStreamBuffer ds(data);
  TileSectorStore store;
  ds.vuread(store.generationLevel);
  ds.vuread(store.tileSerializationVersion);

  store.tiles.reset(new TileArray());
  for (size_t y = 0; y < WorldSectorSize; ++y) {
    for (size_t x = 0; x < WorldSectorSize; ++x) {
      ServerTile tile;
      tile.read(ds, store.tileSerializationVersion);
      (*store.tiles)(x, y) = tile;
    }
  }

  return store;
#else
  auto& root = Root::singleton();
  auto matDatabase = root.materialDatabase();
  auto liqDatabase = root.liquidsDatabase();
  auto storageConfig = root.assets()->json("/worldstorage.config");

  DataStreamBuffer ds(uncompressData(data));
  TileSectorStore store;
  ds.vuread(store.generationLevel);
  ds.vuread(store.tileSerializationVersion);

  store.tiles.reset(new TileArray());
  for (size_t y = 0; y < WorldSectorSize; ++y) {
    for (size_t x = 0; x < WorldSectorSize; ++x) {
      ServerTile tile;
      tile.read(ds, store.tileSerializationVersion);

      if (!matDatabase->isValidMaterialId(tile.foreground))
        tile.foreground = storageConfig.getUInt("replacementMaterialId");
      if (!matDatabase->isValidMaterialId(tile.background))
        tile.background = storageConfig.getUInt("replacementMaterialId");
      if (!matDatabase->isValidModId(tile.foregroundMod))
        tile.foregroundMod = storageConfig.getUInt("replacementModId");
      if (!matDatabase->isValidModId(tile.backgroundMod))
        tile.backgroundMod = storageConfig.getUInt("replacementModId");
      if (!liqDatabase->isValidLiquidId(tile.liquid.liquid)) {
        LiquidId replacementLiquid = storageConfig.getUInt("replacementLiquidId");
        if (replacementLiquid == EmptyLiquidId)
          tile.liquid = LiquidStore();
        else
          tile.liquid.liquid = replacementLiquid;
      }

      (*store.tiles)(x, y) = tile;
    }
  }

  return store;
#endif
}

ByteArray WorldStorage::writeTileSector(TileSectorStore const& store) {
#ifdef STAR_PLATFORM_N3DS
  return serializeTileSector(store);
#else
  return compressData(serializeTileSector(store));
#endif
}

ByteArray WorldStorage::serializeTileSector(TileSectorStore const& store) {
  DataStreamBuffer ds;
  ds.vuwrite(store.generationLevel);
  ds.vuwrite(store.tileSerializationVersion);
  starAssert(store.tiles);
  for (size_t y = 0; y < WorldSectorSize; ++y) {
    for (size_t x = 0; x < WorldSectorSize; ++x)
      (*store.tiles)(x, y).write(ds);
  }
  return ds.takeData();
}

ByteArray WorldStorage::uniqueIndexKey(String const& uniqueId) {
  DataStreamBuffer ds(5);
  ds.write(StoreType::UniqueIndex);
  ds.write(xxHash32(uniqueId));
  return ds.takeData();
}

WorldStorage::UniqueIndexStore WorldStorage::readUniqueIndexStore(ByteArray const& data) {
#ifdef STAR_PLATFORM_N3DS
  return DataStreamBuffer::deserializeMapContainer<UniqueIndexStore>(data,
#else
  return DataStreamBuffer::deserializeMapContainer<UniqueIndexStore>(uncompressData(data),
#endif
      [](DataStream& ds, String& key, SectorAndPosition& value) {
        ds.read(key);
        ds.cread<uint16_t>(value.first[0]);
        ds.cread<uint16_t>(value.first[1]);
        ds.read(value.second);
      });
}

ByteArray WorldStorage::writeUniqueIndexStore(UniqueIndexStore const& store) {
#ifdef STAR_PLATFORM_N3DS
  return serializeUniqueIndexStore(store);
#else
  return compressData(serializeUniqueIndexStore(store));
#endif
}

ByteArray WorldStorage::serializeUniqueIndexStore(UniqueIndexStore const& store) {
  return DataStreamBuffer::serializeMapContainer(store,
      [](DataStream& ds, String const& key, SectorAndPosition const& value) {
        ds.write(key);
        ds.cwrite<uint16_t>(value.first[0]);
        ds.cwrite<uint16_t>(value.first[1]);
        ds.write(value.second);
      });
}

ByteArray WorldStorage::sectorUniqueKey(Sector const& sector) {
  DataStreamBuffer ds(5);
  ds.write(StoreType::SectorUniques);
  ds.cwrite<uint16_t>(sector[0]);
  ds.cwrite<uint16_t>(sector[1]);
  return ds.takeData();
}

WorldStorage::SectorUniqueStore WorldStorage::readSectorUniqueStore(ByteArray const& data) {
#ifdef STAR_PLATFORM_N3DS
  return DataStreamBuffer::deserialize<SectorUniqueStore>(data);
#else
  return DataStreamBuffer::deserialize<SectorUniqueStore>(uncompressData(data));
#endif
}

ByteArray WorldStorage::writeSectorUniqueStore(SectorUniqueStore const& store) {
#ifdef STAR_PLATFORM_N3DS
  return serializeSectorUniqueStore(store);
#else
  return compressData(serializeSectorUniqueStore(store));
#endif
}

ByteArray WorldStorage::serializeSectorUniqueStore(SectorUniqueStore const& store) {
  return DataStreamBuffer::serialize(store);
}

ByteArray WorldStorage::compressStorageData(ByteArray const& data) {
#ifdef STAR_PLATFORM_N3DS
  m_storageTimingStats.compressionCalls += 1;
  m_storageTimingStats.compressionInputBytes += data.size();
  m_storageTimingStats.compressionOutputBytes += data.size();
  return data;
#else
  auto compressionStart = Time::monotonicMicroseconds();
  auto compressed = compressData(data);
  m_storageTimingStats.compressionCalls += 1;
  m_storageTimingStats.compressionInputBytes += data.size();
  m_storageTimingStats.compressionOutputBytes += compressed.size();
  m_storageTimingStats.compressionMicroseconds += Time::monotonicMicroseconds() - compressionStart;
  return compressed;
#endif
}

ByteArray WorldStorage::writeWorldMetadataTracked(WorldMetadataStore const& metadata) {
  return compressStorageData(serializeWorldMetadata(metadata));
}

ByteArray WorldStorage::writeEntitySectorTracked(EntitySectorStore const& store) {
  auto storeStart = Time::monotonicMicroseconds();
  auto data = serializeEntitySector(store);
  m_storageTimingStats.entityStoreMicroseconds += Time::monotonicMicroseconds() - storeStart;
  m_storageTimingStats.entityStoreBytes += data.size();
  return compressStorageData(data);
}

ByteArray WorldStorage::writeTileSectorTracked(TileSectorStore const& store) {
  auto storeStart = Time::monotonicMicroseconds();
  auto data = serializeTileSector(store);
  m_storageTimingStats.tileStoreMicroseconds += Time::monotonicMicroseconds() - storeStart;
  m_storageTimingStats.tileStoreBytes += data.size();
  return compressStorageData(data);
}

ByteArray WorldStorage::writeUniqueIndexStoreTracked(UniqueIndexStore const& store) {
  return compressStorageData(serializeUniqueIndexStore(store));
}

ByteArray WorldStorage::writeSectorUniqueStoreTracked(SectorUniqueStore const& store) {
  return compressStorageData(serializeSectorUniqueStore(store));
}

bool WorldStorage::insertStoredValue(StoreType storeType, ByteArray const& key, ByteArray const& value) {
  if (auto existing = m_db.find(key)) {
    if (*existing == value) {
      m_storageTimingStats.btreeInsertSkips += 1;
      m_storageTimingStats.btreeInsertSkipBytes += value.size();
      recordStoredValueSkip(storeType);
      return false;
    }
  }

  auto insertStart = Time::monotonicMicroseconds();
  bool replaced = m_db.insert(key, value);
  m_storageTimingStats.btreeInserts += 1;
  m_storageTimingStats.btreeInsertBytes += key.size() + value.size();
  m_storageTimingStats.btreeInsertMicroseconds += Time::monotonicMicroseconds() - insertStart;
  recordStoredValueWrite(storeType);
  return replaced;
}

bool WorldStorage::removeStoredValue(StoreType storeType, ByteArray const& key) {
  auto insertStart = Time::monotonicMicroseconds();
  bool removed = m_db.remove(key);
  if (removed) {
    m_storageTimingStats.btreeInserts += 1;
    m_storageTimingStats.btreeInsertBytes += key.size();
    m_storageTimingStats.btreeInsertMicroseconds += Time::monotonicMicroseconds() - insertStart;
    recordStoredValueRemove(storeType);
  }
  return removed;
}

void WorldStorage::syncActiveSectorsForSnapshot(uint64_t& syncedSectors, uint64_t& syncMicroseconds) {
  auto snapshotSyncStart = Time::monotonicMicroseconds();
  for (auto const& pair : m_sectorMetadata) {
    recordDirtySectorVisit(pair.first, true);
    syncSector(pair.first);
    syncedSectors += 1;
  }
  syncMicroseconds += static_cast<uint64_t>(Time::monotonicMicroseconds() - snapshotSyncStart);
}

void WorldStorage::recordDirtySectorVisit(Sector const& sector, bool snapshotSync) {
  bool markedDirty = m_dirtySectorReasons.contains(sector);
  if (snapshotSync) {
    if (markedDirty)
      m_storageTimingStats.dirtySnapshotMarkedSectors += 1;
    else
      m_storageTimingStats.dirtySnapshotUnmarkedSectors += 1;
  } else {
    if (markedDirty)
      m_storageTimingStats.dirtySyncMarkedSectors += 1;
    else
      m_storageTimingStats.dirtySyncUnmarkedSectors += 1;
  }
}

void WorldStorage::clearDirtySectorMarks() {
  m_dirtySectorReasons.clear();
}

void WorldStorage::recordStoredValueWrite(StoreType storeType) {
  switch (storeType) {
    case StoreType::Metadata:
      m_storageTimingStats.metadataWrites += 1;
      break;
    case StoreType::TileSector:
      m_storageTimingStats.tileSectorWrites += 1;
      break;
    case StoreType::EntitySector:
      m_storageTimingStats.entitySectorWrites += 1;
      break;
    case StoreType::UniqueIndex:
      m_storageTimingStats.uniqueIndexWrites += 1;
      break;
    case StoreType::SectorUniques:
      m_storageTimingStats.sectorUniqueWrites += 1;
      break;
  }
}

void WorldStorage::recordStoredValueSkip(StoreType storeType) {
  switch (storeType) {
    case StoreType::Metadata:
      m_storageTimingStats.metadataWriteSkips += 1;
      break;
    case StoreType::TileSector:
      m_storageTimingStats.tileSectorWriteSkips += 1;
      break;
    case StoreType::EntitySector:
      m_storageTimingStats.entitySectorWriteSkips += 1;
      break;
    case StoreType::UniqueIndex:
      m_storageTimingStats.uniqueIndexWriteSkips += 1;
      break;
    case StoreType::SectorUniques:
      m_storageTimingStats.sectorUniqueWriteSkips += 1;
      break;
  }
}

void WorldStorage::recordStoredValueRemove(StoreType storeType) {
  switch (storeType) {
    case StoreType::Metadata:
      m_storageTimingStats.metadataRemoves += 1;
      break;
    case StoreType::TileSector:
      m_storageTimingStats.tileSectorRemoves += 1;
      break;
    case StoreType::EntitySector:
      m_storageTimingStats.entitySectorRemoves += 1;
      break;
    case StoreType::UniqueIndex:
      m_storageTimingStats.uniqueIndexRemoves += 1;
      break;
    case StoreType::SectorUniques:
      m_storageTimingStats.sectorUniqueRemoves += 1;
      break;
  }
}

void WorldStorage::commitStoredValues() {
  auto commitStart = Time::monotonicMicroseconds();
  m_db.commit();
  m_storageTimingStats.commits += 1;
  m_storageTimingStats.commitMicroseconds += Time::monotonicMicroseconds() - commitStart;
}

void WorldStorage::openDatabase(BTreeDatabase& db, IODevicePtr device) {
  db.setContentIdentifier("World4");
  db.setKeySize(5);
  db.setIODevice(std::move(device));
#ifdef STAR_PLATFORM_N3DS
  db.setBlockSize(512);
  db.setIndexCacheSize(4);
#else
  db.setBlockSize(2048);
#endif
  db.setAutoCommit(false);
  db.open();

  if (db.contentIdentifier() != "World4" || db.keySize() != 5)
    throw WorldStorageException::format("World database format is too old or unrecognized!");
}

WorldStorage::WorldStorage() {
#ifdef STAR_PLATFORM_N3DS
  m_sectorTimeToLive = Vec2F(1.0f, 2.0f);
  m_generationQueueTimeToLive = 1.0f;
  Logger::info("N3DS WorldStorage: using compact storage config");
  return;
#endif

  auto storageConfig = Root::singleton().assets()->json("/worldstorage.config");
  m_sectorTimeToLive = jsonToVec2F(storageConfig.get("sectorTimeToLive"));
  m_generationQueueTimeToLive = storageConfig.getFloat("generationQueueTimeToLive");
}

bool WorldStorage::belongsInSector(Sector const& sector, Vec2F const& position) const {
  WorldGeometry geometry(m_tileArray->size());
  return RectF(m_tileArray->sectorRegion(sector)).belongs(geometry.limit(position));
}

float WorldStorage::randomizedSectorTTL() const {
  return Random::randf(m_sectorTimeToLive[0], m_sectorTimeToLive[1]);
}

pair<bool, size_t> WorldStorage::generateSectorToLevel(Sector const& sector, SectorGenerationLevel targetGenerationLevel, size_t sectorGenerationLevelLimit) {
  if (!m_tileArray->sectorValid(sector))
    return {false, 0};

  loadSectorToLevel(sector, SectorLoadLevel::Loaded);

  auto& metadata = m_sectorMetadata[sector];

  if (targetGenerationLevel == SectorGenerationLevel::Complete && metadata.generationLevel == SectorGenerationLevel::Terraform) {
    m_generatorFacade->terraformSector(this, sector);
    metadata.generationLevel = SectorGenerationLevel::Complete;
    metadata.timeToLive = randomizedSectorTTL();
    markSectorDirty(sector, TileDirtySectorReason | GenerationDirtySectorReason);
    return {true, 1};
  }

  if (metadata.generationLevel >= targetGenerationLevel)
    return {true, 0};

  metadata.timeToLive = randomizedSectorTTL();

  size_t totalGeneratedLevels = 0;
  for (uint8_t i = (uint8_t)metadata.generationLevel + 1; i <= (uint8_t)targetGenerationLevel; ++i) {
    SectorGenerationLevel currentGeneration = (SectorGenerationLevel)i;
    SectorGenerationLevel stepDownGeneration = (SectorGenerationLevel)(i - 1);

    if (stepDownGeneration != SectorGenerationLevel::None) {
      for (auto adjacentSector : adjacentSectors(sector)) {
        auto p = generateSectorToLevel(adjacentSector, stepDownGeneration, sectorGenerationLevelLimit - totalGeneratedLevels);
        totalGeneratedLevels += p.second;
        if (!p.first || totalGeneratedLevels >= sectorGenerationLevelLimit)
          return {false, totalGeneratedLevels};
      }
    }

    m_generatorFacade->generateSectorLevel(this, sector, currentGeneration);
    metadata.generationLevel = currentGeneration;
    markSectorDirty(sector, TileDirtySectorReason | GenerationDirtySectorReason);

    ++totalGeneratedLevels;
    if (totalGeneratedLevels >= sectorGenerationLevelLimit)
      return {metadata.generationLevel == targetGenerationLevel, totalGeneratedLevels};
  }

  return {true, totalGeneratedLevels};
}

void WorldStorage::loadSectorToLevel(Sector const& sector, SectorLoadLevel targetLoadLevel) {
  if (!m_tileArray->sectorValid(sector))
    return;

  auto& metadata = m_sectorMetadata[sector];
  if (metadata.loadLevel >= targetLoadLevel)
    return;

  metadata.timeToLive = randomizedSectorTTL();

  for (uint8_t i = (uint8_t)metadata.loadLevel + 1; i <= (uint8_t)targetLoadLevel; ++i) {
    SectorLoadLevel currentLoad = (SectorLoadLevel)i;
    SectorLoadLevel stepDownLoad = (SectorLoadLevel)(i - 1);

    if (stepDownLoad != SectorLoadLevel::None) {
      for (auto adjacentSector : adjacentSectors(sector))
        loadSectorToLevel(adjacentSector, stepDownLoad);
    }

    if (currentLoad == SectorLoadLevel::Tiles) {
      if (auto res = m_db.find(tileSectorKey(sector))) {
        TileSectorStore sectorStore = readTileSector(*res);

        m_tileArray->loadSector(sector, std::move(sectorStore.tiles));

        metadata.generationLevel = sectorStore.generationLevel;
      } else {
        if (!m_tileArray->sectorLoaded(sector))
          m_tileArray->loadDefaultSector(sector);
      }

      metadata.loadLevel = currentLoad;
      m_generatorFacade->sectorLoadLevelChanged(this, sector, currentLoad);

    } else if (currentLoad == SectorLoadLevel::Entities) {
      List<EntityPtr> addedEntities;
      if (auto res = m_db.find(entitySectorKey(sector))) {
        auto entityFactory = Root::singleton().entityFactory();
        EntitySectorStore sectorStore = readEntitySector(*res);
        for (auto const& entityStore : sectorStore) {
          try {
            addedEntities.append(entityFactory->loadVersionedEntity(entityStore));
          } catch (std::exception const& e) {
            Logger::warn("Failed to deserialize entity '{}'. {}", entityStore.toJson(), outputException(e, true));
          }
        }
      }

      UniqueIndexStore readUniques;
      for (auto const& entity : addedEntities) {
        m_generatorFacade->initEntity(this, m_entityMap->reserveEntityId(), entity);
        m_entityMap->addEntity(entity);
        if (auto uniqueId = entity->uniqueId())
          readUniques.add(*uniqueId, {sector, entity->position()});
      }

      // Update the stored unique ids on load, in case a desync has happened
      // and there are stale entries in the index.
      if (!readUniques.empty() || m_db.find(sectorUniqueKey(sector)))
        markSectorDirty(sector, UniqueDirtySectorReason);
      updateSectorUniques(sector, readUniques);

      metadata.loadLevel = currentLoad;
      m_generatorFacade->sectorLoadLevelChanged(this, sector, currentLoad);
    }
  }
}

bool WorldStorage::unloadSectorToLevel(Sector const& sector, SectorLoadLevel targetLoadLevel, bool force) {
  if (!m_tileArray->sectorValid(sector) || targetLoadLevel == SectorLoadLevel::Loaded)
    return true;

  auto& metadata = m_sectorMetadata[sector];
  bool entitiesOverlap = false;
  if (m_entityMap) {
    auto entityFactory = Root::singleton().entityFactory();
    List<EntityPtr> entitiesToStore;
    List<EntityPtr> entitiesToRemove;

    for (auto& entity : m_entityMap->entityQuery(RectF(m_tileArray->sectorRegion(sector)))) {
      // Only store / remove entities who belong to this sector.  If an entity
      // overlaps with this sector but does not belong to it, we may not want to
      // completely unload it.
      auto position = entity->position();
      if (!belongsInSector(sector, position)) {
        if (auto entitySector = sectorForPosition(Vec2I(position))) {
          if (auto p = m_sectorMetadata.ptr(*entitySector))
            entitiesOverlap |= p->timeToLive > 0.0f;
        }
        continue;
      }

      bool keepAlive = m_generatorFacade->entityKeepAlive(this, entity);
      if (keepAlive && !force)
        return false;

      if (m_generatorFacade->entityPersistent(this, entity))
        entitiesToStore.append(std::move(entity));
      else
        entitiesToRemove.append(std::move(entity));
    }

    for (auto const& entity : entitiesToRemove) {
      m_entityMap->removeEntity(entity->entityId());
      m_generatorFacade->destructEntity(this, entity);
    }

    if (metadata.loadLevel == SectorLoadLevel::Entities || !entitiesToStore.empty()) {
      EntitySectorStore sectorStore;

      // If our current load level indicates that we might have entities that are
      // not loaded, we need to load and merge with them, otherwise we should be
      // overwriting them.
      if (metadata.loadLevel < SectorLoadLevel::Entities) {
        if (auto res = m_db.find(entitySectorKey(sector)))
          sectorStore = readEntitySector(*res);
      }

      UniqueIndexStore storedUniques;
      auto entityStoreStart = Time::monotonicMicroseconds();
      for (auto const& entity : entitiesToStore) {
        m_entityMap->removeEntity(entity->entityId());
        m_generatorFacade->destructEntity(this, entity);
        auto position = entity->position();
        if (auto uniqueId = entity->uniqueId())
          storedUniques.add(*uniqueId, {sector, position});
        sectorStore.append(entityFactory->storeVersionedEntity(entity));
      }
      m_storageTimingStats.entityStoreSectors += 1;
      m_storageTimingStats.entityStoreEntities += entitiesToStore.size();
      m_storageTimingStats.entityStoreMicroseconds += Time::monotonicMicroseconds() - entityStoreStart;
      markSectorDirty(sector, EntityDirtySectorReason | UniqueDirtySectorReason | UnloadDirtySectorReason);
      insertStoredValue(StoreType::EntitySector, entitySectorKey(sector), writeEntitySectorTracked(sectorStore));
      if (metadata.loadLevel < SectorLoadLevel::Entities)
        mergeSectorUniques(sector, storedUniques);
      else
        updateSectorUniques(sector, storedUniques);

      if (metadata.loadLevel == SectorLoadLevel::Entities) {
        metadata.loadLevel = SectorLoadLevel::Tiles;
        m_generatorFacade->sectorLoadLevelChanged(this, sector, SectorLoadLevel::Tiles);
      }
    }
  }

  if (targetLoadLevel == SectorLoadLevel::None) {
    if (metadata.loadLevel > SectorLoadLevel::None && !entitiesOverlap) {
      TileSectorStore sectorStore;
      sectorStore.tiles = m_tileArray->unloadSector(sector);
      sectorStore.generationLevel = metadata.generationLevel;
      m_storageTimingStats.tileStoreSectors += 1;
      markSectorDirty(sector, TileDirtySectorReason | UnloadDirtySectorReason);
      insertStoredValue(StoreType::TileSector, tileSectorKey(sector), writeTileSectorTracked(sectorStore));
      m_sectorMetadata.remove(sector);
      m_generatorFacade->sectorLoadLevelChanged(this, sector, SectorLoadLevel::None);
      return true;
    }
    return false;
  }
  return true;
}

void WorldStorage::syncSector(Sector const& sector) {
  if (!m_tileArray->sectorValid(sector))
    return;

  auto entityFactory = Root::singleton().entityFactory();
  auto& metadata = m_sectorMetadata[sector];
  m_storageTimingStats.syncedSectors += 1;

  // Only sync the levels that we know are loaded.  It is possible that this
  // sector is at load level < Entities but has zombie entities in it,  but
  // storing those without unloading them will lead to duplication.  Zombie
  // entities will be unloaded in update eventually anyway.

  if (metadata.loadLevel >= SectorLoadLevel::Entities) {
    EntitySectorStore sectorStore;
    UniqueIndexStore storedUniques;
    auto entityStoreStart = Time::monotonicMicroseconds();
    for (auto const& entity : m_entityMap->entityQuery(RectF(m_tileArray->sectorRegion(sector)))) {
      if (!belongsInSector(sector, entity->position()))
        continue;

      if (m_generatorFacade->entityPersistent(this, entity)) {
        if (auto uniqueId = entity->uniqueId())
          storedUniques.add(*uniqueId, {sector, entity->position()});
        sectorStore.append(entityFactory->storeVersionedEntity(entity));
      }
    }
    m_storageTimingStats.entityStoreSectors += 1;
    m_storageTimingStats.entityStoreEntities += sectorStore.size();
    m_storageTimingStats.entityStoreMicroseconds += Time::monotonicMicroseconds() - entityStoreStart;
    insertStoredValue(StoreType::EntitySector, entitySectorKey(sector), writeEntitySectorTracked(sectorStore));
    updateSectorUniques(sector, storedUniques);
  }

  if (metadata.loadLevel >= SectorLoadLevel::Tiles) {
    TileSectorStore sectorStore;
    auto copyStart = Time::monotonicMicroseconds();
    sectorStore.tiles = m_tileArray->copySector(sector);
    m_storageTimingStats.sectorCopies += 1;
    m_storageTimingStats.sectorCopyMicroseconds += Time::monotonicMicroseconds() - copyStart;
    sectorStore.generationLevel = metadata.generationLevel;
    m_storageTimingStats.tileStoreSectors += 1;
    insertStoredValue(StoreType::TileSector, tileSectorKey(sector), writeTileSectorTracked(sectorStore));
  }
}

List<WorldStorage::Sector> WorldStorage::adjacentSectors(Sector const& sector) const {
  auto tiles = m_tileArray->sectorRegion(sector);
  return m_tileArray->validSectorsFor(tiles.padded(WorldSectorSize));
}

void WorldStorage::updateSectorUniques(Sector const& sector, UniqueIndexStore const& sectorUniques) {
  // If there was an old unique sector store here, then we need to remove all
  // the unique index entries for uniques that used to be in this sector but
  // now aren't, in case they are now gone.
  if (auto oldSectorUniques = m_db.find(sectorUniqueKey(sector)).apply(readSectorUniqueStore)) {
    for (auto const& uniqueId : *oldSectorUniques) {
      if (!sectorUniques.contains(uniqueId))
        removeUniqueIndexEntry(uniqueId, sector);
    }
  }

  for (auto const& p : sectorUniques)
    setUniqueIndexEntry(p.first, p.second);

  if (sectorUniques.empty())
    removeStoredValue(StoreType::SectorUniques, sectorUniqueKey(sector));
  else
    insertStoredValue(StoreType::SectorUniques, sectorUniqueKey(sector), writeSectorUniqueStoreTracked(HashSet<String>::from(sectorUniques.keys())));
}

void WorldStorage::mergeSectorUniques(Sector const& sector, UniqueIndexStore const& sectorUniques) {
  auto sectorUniqueStore = m_db.find(sectorUniqueKey(sector)).apply(readSectorUniqueStore).value();
  for (auto const& p : sectorUniques) {
    setUniqueIndexEntry(p.first, p.second);
    sectorUniqueStore.add(p.first);
  }

  if (sectorUniqueStore.empty())
    removeStoredValue(StoreType::SectorUniques, sectorUniqueKey(sector));
  else
    insertStoredValue(StoreType::SectorUniques, sectorUniqueKey(sector), writeSectorUniqueStoreTracked(sectorUniqueStore));
}

auto WorldStorage::getUniqueIndexEntry(String const& uniqueId) -> Maybe<SectorAndPosition> {
  if (auto uniqueIndex = m_db.find(uniqueIndexKey(uniqueId)).apply(readUniqueIndexStore))
    return uniqueIndex->maybe(uniqueId);
  return {};
}

void WorldStorage::setUniqueIndexEntry(String const& uniqueId, SectorAndPosition const& sectorAndPosition) {
  UniqueIndexStore uniqueIndex = m_db.find(uniqueIndexKey(uniqueId)).apply(readUniqueIndexStore).value();
  auto p = uniqueIndex.insert(uniqueId, sectorAndPosition);
  if (!p.second) {
    // Don't need to update the index if the entry was already there and the
    // sector and position haven't changed
    if (p.first->second == sectorAndPosition)
      return;
    p.first->second = sectorAndPosition;
  }
  insertStoredValue(StoreType::UniqueIndex, uniqueIndexKey(uniqueId), writeUniqueIndexStoreTracked(uniqueIndex));
}

void WorldStorage::removeUniqueIndexEntry(String const& uniqueId, Sector const& sector) {
  if (auto uniqueIndex = m_db.find(uniqueIndexKey(uniqueId)).apply(readUniqueIndexStore)) {
    if (auto sectorAndPosition = uniqueIndex->maybe(uniqueId)) {
      if (sectorAndPosition->first == sector) {
        uniqueIndex->remove(uniqueId);
        if (uniqueIndex->empty())
          removeStoredValue(StoreType::UniqueIndex, uniqueIndexKey(uniqueId));
        else
          insertStoredValue(StoreType::UniqueIndex, uniqueIndexKey(uniqueId), writeUniqueIndexStoreTracked(*uniqueIndex));
      }
    }
  }
}

}
