#include "StarCellularLiquid.hpp"
#include "StarCelestialDatabase.hpp"
#include "StarConfiguration.hpp"
#include "StarDataStreamDevices.hpp"
#include "StarFile.hpp"
#include "StarGameTypes.hpp"
#include "StarItemDrop.hpp"
#include "StarLiquidsDatabase.hpp"
#include "StarMaterialDatabase.hpp"
#include "StarNetPackets.hpp"
#include "StarObject.hpp"
#include "StarObjectDatabase.hpp"
#include "StarRoot.hpp"
#include "StarSystemWorldServer.hpp"
#include "StarTime.hpp"
#include "StarUniverseConnection.hpp"
#include "StarUniverseServer.hpp"
#include "StarWorldClientState.hpp"
#include "StarWorldServer.hpp"
#include "StarWorldServerThread.hpp"

#include "gtest/gtest.h"

#include <iostream>

using namespace Star;

namespace {

struct TemporaryUniverseStorage {
  TemporaryUniverseStorage() {
    root = File::temporaryDirectory();
    universe = File::relativeTo(root, "universe");
  }

  ~TemporaryUniverseStorage() {
    File::removeDirectoryRecursive(root);
  }

  String root;
  String universe;
};

struct ConfigurationValueGuard {
  ConfigurationValueGuard(String key, Json value)
    : key(std::move(key)), previousValue(Root::singleton().configuration()->get(this->key)) {
    Root::singleton().configuration()->set(this->key, std::move(value));
  }

  ~ConfigurationValueGuard() {
    Root::singleton().configuration()->set(key, previousValue);
  }

  String key;
  Json previousValue;
};

struct TestLiquidWorld : CellularLiquidWorld<int> {
  CellularLiquidCell<int> cell(Vec2I const& location) const override {
    return cells.value(location, CellularLiquidFlowCell<int>{Maybe<int>(), 0.0f, 0.0f});
  }

  void setFlow(Vec2I const& location, CellularLiquidFlowCell<int> const& flow) override {
    cells.set(location, flow);
  }

  HashMap<Vec2I, CellularLiquidFlowCell<int>> cells;
};

LiquidCellEngineParameters testLiquidEngineParameters() {
  return LiquidCellEngineParameters{
      0.0f,
      0.0f,
      0.0f,
      0.0f,
      0.0f,
      0.0f,
      0.0f,
      0.001f,
      0.001f,
      0.001f,
      0.5f};
}

template <typename Predicate>
bool waitUntil(Predicate predicate, unsigned timeoutMillis = 3000) {
  auto timer = Timer::withMilliseconds(timeoutMillis);
  while (!timer.timeUp()) {
    if (predicate())
      return true;
    Thread::sleep(1);
  }
  return predicate();
}

UniverseServer::ServerStatus waitForServerStatus(UniverseServer& server, function<bool(UniverseServer::ServerStatus const&)> predicate) {
  UniverseServer::ServerStatus status;
  EXPECT_TRUE(waitUntil([&]() {
    status = server.serverStatus();
    return predicate(status);
  }));
  return status;
}

size_t totalOwnedConnections(List<UniverseConnectionServer::NetworkWorkerStats> const& stats) {
  size_t total = 0;
  for (auto const& workerStats : stats)
    total += workerStats.ownedConnections;
  return total;
}

uint64_t totalWakeups(List<UniverseConnectionServer::NetworkWorkerStats> const& stats) {
  uint64_t total = 0;
  for (auto const& workerStats : stats)
    total += workerStats.wakeups;
  return total;
}

bool hasTimingSample(List<ServerTimingStatus> const& timings, String const& name) {
  return timings.any([&name](ServerTimingStatus const& timing) {
    return timing.name == name && timing.samples > 0;
  });
}

void acknowledgeClientWindow(WorldServer& worldServer, ConnectionId clientId, RectI const& window) {
  WorldClientState clientState;
  clientState.setWindow(window);
  worldServer.handleIncomingPackets(clientId, {
      make_shared<WorldStartAcknowledgePacket>(),
      make_shared<WorldClientStateUpdatePacket>(clientState.writeDelta())});
}

ByteArray packetPayload(PacketPtr const& packet) {
  DataStreamBuffer buffer;
  packet->write(buffer, {});
  return buffer.takeData();
}

List<ByteArray> tileArrayUpdatePayloads(List<PacketPtr> const& packets) {
  List<ByteArray> payloads;
  for (auto const& packet : packets) {
    if (packet->type() == PacketType::TileArrayUpdate)
      payloads.append(packetPayload(packet));
  }
  sort(payloads);
  return payloads;
}

List<ByteArray> entityPacketPayloads(List<PacketPtr> const& packets) {
  List<ByteArray> payloads;
  for (auto const& packet : packets) {
    if (packet->type() == PacketType::EntityCreate || packet->type() == PacketType::EntityUpdateSet || packet->type() == PacketType::EntityDestroy)
      payloads.append(packetPayload(packet));
  }
  sort(payloads);
  return payloads;
}

List<ByteArray> preparedTileArrayUpdatePayloads(bool packetSectorPrefill) {
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
      {"storageGenerationPlanning", false},
      {"storageGenerationPlanningDifferentialCheck", false},
      {"packetPreparationSectorPrefill", packetSectorPrefill},
      {"packetPreparationSectorPrefillWorkerThreads", 2},
      {"packetPreparationSectorPrefillMinimumSectors", 0},
      {"packetPreparationSectorPrefillDifferentialCheck", false},
      {"subsystemBaselineMetrics", false}}}});

  WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);

  if (!worldServer.addClient(1, SpawnTargetPosition(Vec2F(32, 32)), true))
    return {};
  if (!worldServer.addClient(2, SpawnTargetPosition(Vec2F(32, 32)), true))
    return {};
  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));
  acknowledgeClientWindow(worldServer, 2, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));

  worldServer.update(1.0f / 60.0f);
  auto payloads = tileArrayUpdatePayloads(worldServer.getOutgoingPackets(1));
  EXPECT_FALSE(payloads.empty());
  return payloads;
}

List<ByteArray> preparedEntityPacketPayloads(bool packetSectorPrefill) {
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
      {"storageGenerationPlanning", false},
      {"storageGenerationPlanningDifferentialCheck", false},
      {"packetPreparationSectorPrefill", packetSectorPrefill},
      {"packetPreparationSectorPrefillWorkerThreads", 2},
      {"packetPreparationSectorPrefillMinimumSectors", 0},
      {"packetPreparationSectorPrefillDifferentialCheck", false},
      {"subsystemBaselineMetrics", false}}}});

  WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);

  if (!worldServer.addClient(1, SpawnTargetPosition(Vec2F(32, 32)), true))
    return {};
  if (!worldServer.addClient(2, SpawnTargetPosition(Vec2F(32, 32)), true))
    return {};
  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));
  acknowledgeClientWindow(worldServer, 2, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));

  auto itemDrop = ItemDrop::throwDrop(ItemDescriptor("perfectlygenericitem", 1), Vec2F(32, 32), Vec2F(), Vec2F(), true);
  EXPECT_TRUE(itemDrop);
  worldServer.addEntity(itemDrop, 100);

  worldServer.update(1.0f / 60.0f);
  auto stats = worldServer.packetPreparationStats();
  EXPECT_GT(stats.entityStoreCacheMisses, 0u);
  EXPECT_GT(stats.entityStoreCacheHits, 0u);

  auto payloads = entityPacketPayloads(worldServer.getOutgoingPackets(1));
  payloads.appendAll(entityPacketPayloads(worldServer.getOutgoingPackets(2)));
  sort(payloads);
  EXPECT_FALSE(payloads.empty());
  return payloads;
}

size_t packetTypeCount(List<PacketPtr> const& packets, PacketType packetType) {
  size_t count = 0;
  for (auto const& packet : packets) {
    if (packet->type() == packetType)
      count += 1;
  }
  return count;
}

struct EmptyEntityUpdateSetRun {
  size_t outgoingUpdateSets = 0;
  WorldServer::PacketPreparationStats packetPreparationStats;
};

struct EmptySystemWorldUpdateRun {
  size_t outgoingUpdates = 0;
  size_t emptyOutgoingUpdates = 0;
  SystemWorldServer::PacketStats packetStats;
};

EmptyEntityUpdateSetRun runEmptyEntityUpdateSetSuppressionFixture(bool skipEmptyEntityUpdateSets) {
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"skipEmptyEntityUpdateSets", skipEmptyEntityUpdateSets}, {"phase6WorldParallelism", JsonObject{
      {"storageGenerationPlanning", false},
      {"storageGenerationPlanningDifferentialCheck", false},
      {"packetPreparationSectorPrefill", false},
      {"packetPreparationSectorPrefillDifferentialCheck", false},
      {"subsystemBaselineMetrics", false}}}});

  WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);
  if (!worldServer.addClient(1, SpawnTargetPosition(Vec2F(32, 32)), true)) {
    ADD_FAILURE() << "Could not add empty entity update-set test client";
    return {};
  }
  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));

  worldServer.update(1.0f / 60.0f);
  auto packets = worldServer.getOutgoingPackets(1);
  return EmptyEntityUpdateSetRun{packetTypeCount(packets, PacketType::EntityUpdateSet), worldServer.packetPreparationStats()};
}

EmptySystemWorldUpdateRun runEmptySystemWorldUpdateSuppressionFixture(bool skipEmptyUpdatePackets) {
  ConfigurationValueGuard configGuard("systemWorldConfigOverrides", JsonObject{{"skipEmptyUpdatePackets", skipEmptyUpdatePackets}});

  JsonObject diskStore{{"location", jsonFromVec3I(Vec3I())}, {"objects", JsonArray()}, {"lastSpawn", 0.0}, {"objectSpawnTime", 1000000.0}};
  SystemWorldServer systemWorld(diskStore, make_shared<Clock>(), make_shared<CelestialMasterDatabase>());
  systemWorld.addClientShip(1, Uuid(), 0.0f, Vec2F());
  systemWorld.pullOutgoingPackets(1);

  systemWorld.update(SystemWorldTimestep);
  systemWorld.pullOutgoingPackets(1);

  systemWorld.update(SystemWorldTimestep);
  auto packets = systemWorld.pullOutgoingPackets(1);

  EmptySystemWorldUpdateRun run;
  run.outgoingUpdates = packetTypeCount(packets, PacketType::SystemWorldUpdate);
  for (auto const& packet : packets) {
    if (auto update = as<SystemWorldUpdatePacket>(packet)) {
      if (update->objectUpdates.empty() && update->shipUpdates.empty())
        run.emptyOutgoingUpdates += 1;
    }
  }
  run.packetStats = systemWorld.packetStats();
  return run;
}

struct PacketCaptureCounts {
  uint64_t total = 0;
  uint64_t stepUpdates = 0;
  uint64_t tileArrays = 0;
  uint64_t tileUpdates = 0;
  uint64_t liquidUpdates = 0;
  uint64_t tileDamageUpdates = 0;
  uint64_t entityCreates = 0;
  uint64_t entityUpdates = 0;
  uint64_t entityUpdateDeltas = 0;
  uint64_t emptyEntityUpdates = 0;
  uint64_t entityDestroys = 0;
  uint64_t giveItems = 0;
  uint64_t modificationFailures = 0;
};

LiquidId firstTestLiquidId();
Maybe<MaterialId> firstTestMaterialId();
Maybe<MaterialId> firstTestFallingMaterialId();
Maybe<String> firstTestObjectName();

void addPacketCaptureCounts(PacketCaptureCounts& counts, List<PacketPtr> const& packets) {
  counts.total += packets.size();
  for (auto const& packet : packets) {
    switch (packet->type()) {
      case PacketType::StepUpdate:
        counts.stepUpdates += 1;
        break;
      case PacketType::TileArrayUpdate:
        counts.tileArrays += 1;
        break;
      case PacketType::TileUpdate:
        counts.tileUpdates += 1;
        break;
      case PacketType::TileLiquidUpdate:
        counts.liquidUpdates += 1;
        break;
      case PacketType::TileDamageUpdate:
        counts.tileDamageUpdates += 1;
        break;
      case PacketType::EntityCreate:
        counts.entityCreates += 1;
        break;
      case PacketType::EntityUpdateSet:
        counts.entityUpdates += 1;
        if (auto updateSet = as<EntityUpdateSetPacket>(packet)) {
          counts.entityUpdateDeltas += updateSet->deltas.size();
          if (updateSet->deltas.empty())
            counts.emptyEntityUpdates += 1;
        }
        break;
      case PacketType::EntityDestroy:
        counts.entityDestroys += 1;
        break;
      case PacketType::GiveItem:
        counts.giveItems += 1;
        break;
      case PacketType::TileModificationFailure:
        counts.modificationFailures += 1;
        break;
      default:
        break;
    }
  }
}

struct GameMechanismWorkloadCapture {
  size_t clients = 0;
  size_t ticks = 0;
  size_t chunks = 0;
  int64_t elapsedMicroseconds = 0;
  PacketCaptureCounts packets;
  WorldServer::PacketPreparationStats packetPreparationStats;
  WorldServer::Phase6WorldParallelismStats phase6Stats;
  WorldStorageTimingStats storageTimingStats;
};

bool setSelfWorkloadForegroundMaterial(WorldServer& worldServer, Vec2I const& position, MaterialId materialId) {
  auto tile = worldServer.modifyServerTile(position, true);
  if (!tile)
    return false;

  tile->foreground = materialId;
  tile->foregroundMod = NoModId;
  tile->foregroundHueShift = 0;
  tile->foregroundColorVariant = DefaultMaterialColorVariant;
  tile->dungeonId = ConstructionDungeonId;

  if (materialId == EmptyMaterialId)
    tile->updateCollision(CollisionKind::None);
  else
    tile->updateCollision(Root::singleton().materialDatabase()->materialCollisionKind(materialId));

  return true;
}

ObjectPtr createSelfWorkloadWireObject(String const& objectName, Vec2I const& position, bool inputNode, bool outputNode) {
  JsonObject orientationOverride{{"anchors", JsonArray()}, {"bgAnchors", JsonArray()}, {"fgAnchors", JsonArray()}, {"spaces", JsonArray{jsonFromVec2I(Vec2I())}}, {"collision", "none"}};
  JsonObject parameters{{"customOrientations", JsonArray{orientationOverride}}, {"scripts", JsonArray()}, {"scriptDelta", 1}, {"keepAlive", true}, {"unbreakable", true}};
  if (inputNode)
    parameters["inputNodes"] = JsonArray{jsonFromVec2I(Vec2I())};
  if (outputNode)
    parameters["outputNodes"] = JsonArray{jsonFromVec2I(Vec2I())};

  auto object = Root::singleton().objectDatabase()->createObject(objectName, parameters);
  object->setTilePosition(position);
  return object;
}

GameMechanismWorkloadCapture runGameMechanismSelfWorkloadCapture() {
  size_t const ClientCount = 4;
  size_t const Ticks = 240;
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"skipEmptyEntityUpdateSets", true}, {"phase6WorldParallelism", JsonObject{
      {"storageGenerationPlanning", true},
      {"storageGenerationPlanningWorkerThreads", 2},
      {"storageGenerationPlanningMinimumSectors", 0},
      {"storageGenerationPlanningDifferentialCheck", true},
      {"packetPreparationSectorPrefill", true},
      {"packetPreparationSectorPrefillWorkerThreads", 2},
      {"packetPreparationSectorPrefillMinimumSectors", 0},
      {"packetPreparationSectorPrefillDifferentialCheck", true},
      {"subsystemBaselineMetrics", true}}}});

  WorldServer worldServer(Vec2U(256, 128), File::ephemeralFile());
  worldServer.setWorldId("self-workload-capture");
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);
  worldServer.initLua(nullptr);
  worldServer.generateRegion(RectI::withSize(Vec2I(80, 24), Vec2I(96, 72)));

  for (size_t i = 0; i < ClientCount; ++i) {
    ConnectionId clientId = static_cast<ConnectionId>(i + 1);
    if (!worldServer.addClient(clientId, SpawnTargetPosition(Vec2F(112 + static_cast<float>(i * 8), 48)), true)) {
      ADD_FAILURE() << "Could not add self-workload capture client";
      return {};
    }
    acknowledgeClientWindow(worldServer, clientId, RectI::withSize(Vec2I(88 + static_cast<int>(i * 8), 32), Vec2I(40, 32)));
  }

  auto liquidId = firstTestLiquidId();
  EXPECT_NE(liquidId, EmptyLiquidId);
  if (liquidId != EmptyLiquidId) {
    for (int x = 104; x < 120; ++x) {
      for (int y = 48; y < 54; ++y)
        worldServer.modifyLiquid(Vec2I(x, y), liquidId, 1.0f);
    }
  }

  auto supportMaterialId = firstTestMaterialId();
  EXPECT_TRUE((bool)supportMaterialId);
  if (supportMaterialId) {
    for (int x = 96; x < 116; ++x) {
      if (!setSelfWorkloadForegroundMaterial(worldServer, Vec2I(x, 44), *supportMaterialId)) {
        ADD_FAILURE() << "Could not seed self-workload capture damage material";
        return {};
      }
    }
  }

  auto fallingMaterialId = firstTestFallingMaterialId();
  EXPECT_TRUE((bool)fallingMaterialId);
  if (supportMaterialId && fallingMaterialId) {
    for (int x = 124; x < 136; ++x) {
      for (int y = 38; y < 45; ++y) {
        if (!setSelfWorkloadForegroundMaterial(worldServer, Vec2I(x, y), EmptyMaterialId)) {
          ADD_FAILURE() << "Could not clear self-workload capture falling shaft";
          return {};
        }
      }

      if (!setSelfWorkloadForegroundMaterial(worldServer, Vec2I(x, 44), *supportMaterialId)
          || !setSelfWorkloadForegroundMaterial(worldServer, Vec2I(x, 45), *fallingMaterialId)) {
        ADD_FAILURE() << "Could not seed self-workload capture falling block column";
        return {};
      }
    }
  }

  for (size_t i = 0; i < 16; ++i) {
    auto itemDrop = ItemDrop::throwDrop(ItemDescriptor("perfectlygenericitem", 1), Vec2F(108 + static_cast<float>(i), 52), Vec2F(), Vec2F(), true);
    if (!itemDrop) {
      ADD_FAILURE() << "Could not create self-workload capture item drop";
      return {};
    }
    worldServer.addEntity(itemDrop, static_cast<EntityId>(1000 + i));
  }

  auto wireObjectName = firstTestObjectName();
  EXPECT_TRUE((bool)wireObjectName);
  if (wireObjectName) {
    Vec2I sourcePosition(140, 46);
    Vec2I relayPosition(142, 46);
    Vec2I sinkPosition(144, 46);
    for (auto position : {sourcePosition, relayPosition, sinkPosition}) {
      if (!setSelfWorkloadForegroundMaterial(worldServer, position, EmptyMaterialId)) {
        ADD_FAILURE() << "Could not clear self-workload capture wire object position";
        return {};
      }
    }

    worldServer.addEntity(createSelfWorkloadWireObject(*wireObjectName, sourcePosition, false, true), 3000);
    worldServer.addEntity(createSelfWorkloadWireObject(*wireObjectName, relayPosition, true, true), 3001);
    worldServer.addEntity(createSelfWorkloadWireObject(*wireObjectName, sinkPosition, true, false), 3002);
    worldServer.wire(sourcePosition, 0, relayPosition, 0);
    worldServer.wire(relayPosition, 0, sinkPosition, 0);
  }

  PacketCaptureCounts packetCounts;
  auto start = Time::monotonicMicroseconds();
  for (size_t tick = 0; tick < Ticks; ++tick) {
    if (tick % 20 == 0) {
      for (size_t i = 0; i < ClientCount; ++i) {
        ConnectionId clientId = static_cast<ConnectionId>(i + 1);
        int offset = static_cast<int>((tick / 20 + i) % 6) * 4;
        WorldClientState clientState;
        clientState.setWindow(RectI::withSize(Vec2I(84 + offset + static_cast<int>(i * 6), 30), Vec2I(48, 36)));
        worldServer.handleIncomingPackets(clientId, {make_shared<WorldClientStateUpdatePacket>(clientState.writeDelta())});
      }
    }

    if (liquidId != EmptyLiquidId && tick % 15 == 0) {
      ConnectionId clientId = static_cast<ConnectionId>((tick / 15) % ClientCount + 1);
      Vec2I position(104 + static_cast<int>((tick / 15) % 16), 54 + static_cast<int>((tick / 30) % 4));
      worldServer.handleIncomingPackets(clientId, {
          make_shared<ModifyTileListPacket>(TileModificationList{{position, PlaceLiquid{liquidId, 0.75f}}}, true)});
    }

    if (liquidId != EmptyLiquidId && tick % 40 == 10) {
      ConnectionId clientId = static_cast<ConnectionId>((tick / 40) % ClientCount + 1);
      List<Vec2I> positions;
      for (int i = 0; i < 4; ++i)
        positions.append(Vec2I(104 + i, 49 + static_cast<int>((tick / 40) % 4)));
      worldServer.handleIncomingPackets(clientId, {make_shared<CollectLiquidPacket>(std::move(positions), liquidId)});
    }

    if (tick % 45 == 15) {
      ConnectionId clientId = static_cast<ConnectionId>((tick / 45) % ClientCount + 1);
      List<Vec2I> damagePositions;
      for (int i = 0; i < 3; ++i)
        damagePositions.append(Vec2I(96 + static_cast<int>((tick / 45) * 2) + i, 44));
      worldServer.damageTiles(damagePositions, TileLayer::Foreground, Vec2F(112, 52), TileDamage(TileDamageType::Blockish, 0.05f, 1));
      worldServer.handleIncomingPackets(clientId, {
          make_shared<DamageTileGroupPacket>(std::move(damagePositions), TileLayer::Foreground, Vec2F(112, 52), TileDamage(TileDamageType::Blockish, 0.25f, 1), Maybe<EntityId>())});
    }

    if (supportMaterialId && fallingMaterialId && tick == 25) {
      for (int x = 124; x < 136; ++x)
        worldServer.destroyBlock(TileLayer::Foreground, Vec2I(x, 44), false, true);
    }

    if (tick % 60 == 30) {
      auto itemDrop = ItemDrop::throwDrop(ItemDescriptor("perfectlygenericitem", 1), Vec2F(110 + static_cast<float>((tick / 60) * 3), 53), Vec2F(), Vec2F(), true);
      if (!itemDrop) {
        ADD_FAILURE() << "Could not create self-workload capture dynamic item drop";
        return {};
      }
      worldServer.addEntity(itemDrop, static_cast<EntityId>(2000 + tick));
    }

    worldServer.update(1.0f / 60.0f);
    for (size_t i = 0; i < ClientCount; ++i)
      addPacketCaptureCounts(packetCounts, worldServer.getOutgoingPackets(static_cast<ConnectionId>(i + 1)));
  }

  worldServer.sync();
  auto chunks = worldServer.readChunks();

  GameMechanismWorkloadCapture capture;
  capture.clients = ClientCount;
  capture.ticks = Ticks;
  capture.chunks = chunks.size();
  capture.elapsedMicroseconds = Time::monotonicMicroseconds() - start;
  capture.packets = packetCounts;
  capture.packetPreparationStats = worldServer.packetPreparationStats();
  capture.phase6Stats = worldServer.phase6WorldParallelismStats();
  capture.storageTimingStats = worldServer.storageTimingStats();

  std::cout << "GameMechanismCapture clients=" << capture.clients
            << " ticks=" << capture.ticks
            << " packets=" << capture.packets.total
            << " step=" << capture.packets.stepUpdates
            << " tileArray=" << capture.packets.tileArrays
            << " tile=" << capture.packets.tileUpdates
            << " liquid=" << capture.packets.liquidUpdates
            << " tileDamage=" << capture.packets.tileDamageUpdates
            << " entityCreate=" << capture.packets.entityCreates
            << " entityUpdate=" << capture.packets.entityUpdates
            << " entityUpdateDeltas=" << capture.packets.entityUpdateDeltas
            << " emptyEntityUpdate=" << capture.packets.emptyEntityUpdates
            << " entityDestroy=" << capture.packets.entityDestroys
            << " giveItem=" << capture.packets.giveItems
            << " failures=" << capture.packets.modificationFailures
            << " packetPrepTicks=" << capture.packetPreparationStats.ticks
            << " regions=" << capture.packetPreparationStats.monitoringRegionBuilds << "/" << capture.packetPreparationStats.monitoringRegionRects << "/" << capture.packetPreparationStats.monitoringRegionSplitRects << "/" << capture.packetPreparationStats.monitoringRegionReuses
            << " sectorCache=" << capture.packetPreparationStats.sectorPacketCacheHits << "/" << capture.packetPreparationStats.sectorPacketCacheMisses
            << " entityStoreCache=" << capture.packetPreparationStats.entityStoreCacheHits << "/" << capture.packetPreparationStats.entityStoreCacheMisses
            << " netStateCache=" << capture.packetPreparationStats.entityNetStateCacheHits << "/" << capture.packetPreparationStats.entityNetStateCacheMisses
            << " updateSets=" << capture.packetPreparationStats.entityUpdateSetPackets << "/" << capture.packetPreparationStats.entityUpdateSetDeltas << "/" << capture.packetPreparationStats.emptyEntityUpdateSetPackets << "/" << capture.packetPreparationStats.emptyEntityUpdateSetSkips
            << " sectorFanout=" << capture.packetPreparationStats.sectorClientFanoutLookups << "/" << capture.packetPreparationStats.sectorClientFanoutRecipients << "/" << capture.packetPreparationStats.sectorClientFanoutMisses
            << " liquidCache=" << capture.phase6Stats.liquidNoProcessingLimitRegionCacheBuilds << "/" << capture.phase6Stats.liquidNoProcessingLimitRegionCacheRebuildSkips << "/" << capture.phase6Stats.liquidNoProcessingLimitRegionCacheRegions << "/" << capture.phase6Stats.liquidNoProcessingLimitRegionCacheBuckets << "/" << capture.phase6Stats.liquidNoProcessingLimitRegionCacheLookups << "/" << capture.phase6Stats.liquidNoProcessingLimitRegionCacheCandidates << "/" << capture.phase6Stats.liquidNoProcessingLimitRegionCacheHits
            << " falling=" << capture.phase6Stats.fallingBlocksBaselineTicks << "/" << capture.phase6Stats.fallingBlocksPendingPositions << "/" << capture.phase6Stats.fallingBlocksProcessedPositions << "/" << capture.phase6Stats.fallingBlocksMovedBlocks
            << " wiring=" << capture.phase6Stats.wiringBaselineTicks << "/" << capture.phase6Stats.wiringInitialEntities << "/" << capture.phase6Stats.wiringLoadedEntities << "/" << capture.phase6Stats.wiringNetworkLoads << "/" << capture.phase6Stats.wiringEvaluatedEntities
            << " entity=" << capture.phase6Stats.entityBaselineTicks << "/" << capture.phase6Stats.entityUpdatedEntities << "/" << capture.phase6Stats.entityTileEntities << "/" << capture.phase6Stats.entityDestroyedEntities << "/" << capture.phase6Stats.entityIterationCopies << "/" << capture.phase6Stats.entitySortedEntities << "/" << capture.phase6Stats.entityCopyMicroseconds << "/" << capture.phase6Stats.entitySortMicroseconds << "/" << capture.phase6Stats.entityUpdateMicroseconds << "/" << capture.phase6Stats.entityMetadataRefreshMicroseconds
            << " lua=" << capture.phase6Stats.luaBaselineTicks << "/" << capture.phase6Stats.luaScriptContexts << "/" << capture.phase6Stats.luaScriptUpdates << "/" << capture.phase6Stats.luaScriptUpdateMicroseconds << "/" << capture.phase6Stats.luaMaxScriptUpdateMicroseconds
            << " storage=" << capture.storageTimingStats.syncs << "/" << capture.storageTimingStats.syncedSectors << "/" << capture.storageTimingStats.tileStoreSectors << "/" << capture.storageTimingStats.entityStoreSectors << "/" << capture.storageTimingStats.btreeInserts << "/" << capture.storageTimingStats.btreeInsertSkips << "/" << capture.storageTimingStats.fullSnapshotExports << "/" << capture.storageTimingStats.fullSnapshotChunks << "/" << capture.storageTimingStats.fullSnapshotBytes
            << " chunks=" << capture.chunks
            << " elapsedUs=" << capture.elapsedMicroseconds
            << std::endl;

  return capture;
}

LiquidId firstTestLiquidId() {
  auto liquidsDatabase = Root::singleton().liquidsDatabase();
  for (auto const& liquidName : liquidsDatabase->liquidNames()) {
    if (liquidName != "empty")
      return liquidsDatabase->liquidId(liquidName);
  }
  return EmptyLiquidId;
}

Maybe<MaterialId> firstTestMaterialId() {
  auto materialDatabase = Root::singleton().materialDatabase();
  for (auto const& materialName : materialDatabase->materialNames()) {
    auto materialId = materialDatabase->materialId(materialName);
    if (materialId != EmptyMaterialId && materialDatabase->canPlaceInLayer(materialId, TileLayer::Foreground))
      return materialId;
  }
  return {};
}

Maybe<MaterialId> firstTestFallingMaterialId() {
  auto materialDatabase = Root::singleton().materialDatabase();
  for (auto const& materialName : materialDatabase->materialNames()) {
    auto materialId = materialDatabase->materialId(materialName);
    if (materialId != EmptyMaterialId && materialDatabase->canPlaceInLayer(materialId, TileLayer::Foreground)
        && (materialDatabase->isFallingMaterial(materialId) || materialDatabase->isCascadingFallingMaterial(materialId)))
      return materialId;
  }
  return {};
}

Maybe<String> firstTestObjectName() {
  auto objectDatabase = Root::singleton().objectDatabase();
  for (auto const& objectName : objectDatabase->allObjects()) {
    try {
      auto config = objectDatabase->getConfig(objectName);
      if (config->type == "object" && !config->orientations.empty())
        return objectName;
    } catch (std::exception const&) {}
  }
  return {};
}

List<uint64_t> phase6SubsystemBaselineSignature() {
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
  {"storageGenerationPlanning", false},
  {"storageGenerationPlanningDifferentialCheck", false},
  {"packetPreparationSectorPrefill", false},
  {"packetPreparationSectorPrefillDifferentialCheck", false},
      {"subsystemBaselineMetrics", true}}}});

  WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);
  worldServer.initLua(nullptr);

  if (!worldServer.addClient(1, SpawnTargetPosition(Vec2F(32, 32)), true)) {
    ADD_FAILURE() << "Could not add baseline metrics test client";
    return {};
  }
  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));

  auto itemDrop = ItemDrop::throwDrop(ItemDescriptor("perfectlygenericitem", 1), Vec2F(32, 32), Vec2F(), Vec2F(), true);
  EXPECT_TRUE(itemDrop);
  if (itemDrop)
    worldServer.addEntity(itemDrop, 100);

  auto liquidId = firstTestLiquidId();
  EXPECT_NE(liquidId, EmptyLiquidId);
  if (liquidId != EmptyLiquidId)
    worldServer.modifyLiquid(Vec2I(32, 36), liquidId, 1.0f);

  WorldServer::Phase6WorldParallelismStats stats;
  for (size_t i = 0; i < 180; ++i) {
    worldServer.update(1.0f / 60.0f);
    stats = worldServer.phase6WorldParallelismStats();
    if (stats.liquidBaselineTicks > 0 && stats.fallingBlocksBaselineTicks > 0 && stats.wiringBaselineTicks > 0
      && stats.entityBaselineTicks > 0 && stats.luaBaselineTicks > 0)
      break;
  }

  EXPECT_TRUE(stats.subsystemBaselineMetricsEnabled);
  EXPECT_GT(stats.liquidBaselineTicks, 0u);
  EXPECT_GT(stats.liquidActiveCells, 0u);
  EXPECT_GT(stats.liquidMonitoringRegions, 0u);
  EXPECT_GT(stats.liquidNoProcessingLimitRegionCacheBuilds, 0u);
  EXPECT_GT(stats.liquidNoProcessingLimitRegionCacheRebuildSkips, 0u);
  EXPECT_GT(stats.liquidNoProcessingLimitRegionCacheRegions, 0u);
  EXPECT_GT(stats.liquidNoProcessingLimitRegionCacheBuckets, 0u);
  EXPECT_GT(stats.fallingBlocksBaselineTicks, 0u);
  EXPECT_GT(stats.wiringBaselineTicks, 0u);
  EXPECT_GT(stats.entityBaselineTicks, 0u);
  EXPECT_GT(stats.entityIterationCopies, 0u);
  EXPECT_GT(stats.entitySortedEntities, 0u);
  EXPECT_GT(stats.luaBaselineTicks, 0u);
  EXPECT_GT(stats.luaScriptContexts, 0u);
  EXPECT_GT(stats.luaScriptUpdates, 0u);
  EXPECT_GE(stats.fallingBlocksProcessedPositions, stats.fallingBlocksMovedBlocks);
  EXPECT_GE(stats.wiringLoadedEntities, stats.wiringEvaluatedEntities);
  EXPECT_GE(stats.entityUpdatedEntities, stats.entityTileEntities);
  EXPECT_GE(stats.entityIterationCopies, stats.entityUpdatedEntities);
  EXPECT_GE(stats.entitySortedEntities, stats.entityUpdatedEntities);
  EXPECT_GE(stats.luaScriptUpdates, stats.luaScriptContexts);
  EXPECT_GE(stats.luaScriptUpdateMicroseconds, stats.luaMaxScriptUpdateMicroseconds);

  return List<uint64_t>{
      stats.liquidBaselineTicks,
      stats.liquidActiveCells,
      stats.liquidMonitoringRegions,
      stats.liquidNoProcessingLimitRegionCacheBuilds,
      stats.liquidNoProcessingLimitRegionCacheRebuildSkips,
      stats.liquidNoProcessingLimitRegionCacheRegions,
      stats.liquidNoProcessingLimitRegionCacheBuckets,
      stats.liquidNoProcessingLimitRegionCacheLookups,
      stats.liquidNoProcessingLimitRegionCacheCandidates,
      stats.liquidNoProcessingLimitRegionCacheHits,
      stats.fallingBlocksBaselineTicks,
      stats.fallingBlocksPendingPositions,
      stats.fallingBlocksProcessedPositions,
      stats.fallingBlocksMovedBlocks,
      stats.wiringBaselineTicks,
      stats.wiringInitialEntities,
      stats.wiringLoadedEntities,
      stats.wiringNetworkLoads,
      stats.wiringEvaluatedEntities,
      stats.entityBaselineTicks,
      stats.entityUpdatedEntities,
      stats.entityTileEntities,
      stats.entityDestroyedEntities,
      stats.entityIterationCopies,
      stats.entitySortedEntities,
      stats.luaBaselineTicks,
      stats.luaScriptContexts,
      stats.luaScriptUpdates};
}

}

TEST(MulticorePhaseTest, Phase0UniverseLoopTimingStatus) {
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.universeTimings.any([](UniverseServer::ServerStatus::TimingStatus const& timing) {
      return timing.name == "loop" && timing.samples > 0;
    });
  });

  Maybe<UniverseServer::ServerStatus::TimingStatus> loopTiming;
  for (auto const& timing : status.universeTimings) {
    if (timing.name == "loop") {
      loopTiming = timing;
      break;
    }
  }
  ASSERT_TRUE(loopTiming);
  EXPECT_GT(loopTiming->samples, 0u);
  EXPECT_GE(loopTiming->maxMicroseconds, loopTiming->p50Microseconds);
  EXPECT_GE(loopTiming->p99Microseconds, loopTiming->p95Microseconds);
  EXPECT_GE(loopTiming->p95Microseconds, loopTiming->p50Microseconds);

  server.stop();
  server.join();
}

TEST(MulticorePhaseTest, Phase1NetworkWorkersShardAndWake) {
  UniverseConnectionServer server([](UniverseConnectionServer*, ConnectionId, List<PacketPtr>) {}, 2);

  List<UniverseConnection> clients;
  for (ConnectionId clientId = 1; clientId <= 4; ++clientId) {
    auto pair = LocalPacketSocket::openPair();
    server.addConnection(clientId, UniverseConnection(std::move(pair.first)));
    clients.append(UniverseConnection(std::move(pair.second)));
  }

  ASSERT_TRUE(waitUntil([&server]() { return totalOwnedConnections(server.workerStats()) == 4; }));
  auto stats = server.workerStats();
  ASSERT_EQ(2, stats.size());
  EXPECT_EQ(2, stats[0].ownedConnections);
  EXPECT_EQ(2, stats[1].ownedConnections);

  auto beforeWakeups = totalWakeups(server.workerStats());
  server.sendPackets(3, {make_shared<ProtocolRequestPacket>(99)});

  shared_ptr<ProtocolRequestPacket> received;
  ASSERT_TRUE(waitUntil([&]() {
    clients[2].receive();
    if (auto packet = clients[2].pullSingle())
      received = as<ProtocolRequestPacket>(packet);
    return (bool)received;
  }));
  EXPECT_EQ(99, received->requestProtocolVersion);
  EXPECT_GT(totalWakeups(server.workerStats()), beforeWakeups);
}

TEST(MulticorePhaseTest, Phase2PendingHandshakeStateMachineRejectsBadProtocol) {
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{{"usePendingConnectionStateMachine", true}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto connection = server.addLocalClient();
  connection.pushSingle(make_shared<ProtocolRequestPacket>(StarProtocolVersion + 1));
  ASSERT_TRUE(connection.sendAll(1000));

  PacketPtr response;
  ASSERT_TRUE(waitUntil([&]() {
    connection.receive();
    response = connection.pullSingle();
    return response != nullptr;
  }));
  auto protocolResponse = as<ProtocolResponsePacket>(response);
  ASSERT_TRUE(protocolResponse);
  EXPECT_FALSE(protocolResponse->allowed);

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.pendingHandshakeRejected == 1 && status.pendingHandshakes == 0;
  });
  EXPECT_EQ(status.pendingHandshakeFinalized, 0u);

  server.stop();
  server.join();
}

TEST(MulticorePhaseTest, Phase3AsyncPersistenceSnapshotsAndFallbacks) {
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{
      {"persistenceWorkerThreads", 1},
      {"maxQueuedPersistenceSnapshots", 1},
      {"maxPersistenceWriteRetries", 0}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.persistenceQueueFullFallbacks > 0 && status.persistenceSnapshotsWritten >= 2;
  });

  EXPECT_TRUE(status.persistenceAsyncEnabled);
  EXPECT_EQ(status.persistenceFailures, 0u);
  EXPECT_GT(status.persistenceSynchronousFallbacks, 0u);
  EXPECT_EQ(status.persistenceBatchesPending, 0u);
  EXPECT_EQ(status.persistenceSnapshotsPending, 0u);

  server.stop();
  server.join();
}

TEST(MulticorePhaseTest, Phase4WorldCommandMailboxProcessesQueuedCommands) {
  auto worldServer = make_shared<WorldServer>(Vec2U(64, 64), File::ephemeralFile());
  worldServer->setSpawningEnabled(false);

  WorldServerThread worldThread(worldServer, InstanceWorldId("multicorephasetest"));
  worldThread.start();
  ASSERT_TRUE(waitUntil([&worldThread]() { return worldThread.packetPreparationStats().ticks > 0; }, 5000));

  auto beforeCommands = worldThread.commandStats().processed;
  worldThread.setWorldPause(true);
  auto commandStats = worldThread.commandStats();
  EXPECT_GT(commandStats.processed, beforeCommands);
  EXPECT_EQ(commandStats.failed, 0u);
  EXPECT_TRUE(hasTimingSample(worldThread.threadTimingStatus(), "worldUpdate"));
  EXPECT_TRUE(hasTimingSample(worldThread.worldTimingStatus(), "packetPreparation"));

  worldThread.stop();
}

TEST(MulticorePhaseTest, Phase4WorldCommandMailboxPropagatesQueuedCommandResults) {
  auto worldServer = make_shared<WorldServer>(Vec2U(64, 64), File::ephemeralFile());
  worldServer->setSpawningEnabled(false);

  WorldServerThread worldThread(worldServer, InstanceWorldId("multicorephasecommandresulttest"));
  worldThread.start();
  ASSERT_TRUE(waitUntil([&worldThread]() { return worldThread.packetPreparationStats().ticks > 0; }, 5000));

  auto beforeCommands = worldThread.commandStats();
  ShipUpgrades shipUpgrades(JsonObject{
      {"shipLevel", 0},
      {"maxFuel", 123},
      {"crewSize", 4},
      {"fuelEfficiency", 0.75},
      {"shipSpeed", 7}});
  StringMap<StringList> speciesShips{{"human", StringList{"/ships/human/humant0.structure"}}};
  auto result = worldThread.applyShipUpgrades("human", shipUpgrades, speciesShips);
  auto commandStats = worldThread.commandStats();

  EXPECT_GT(commandStats.processed, beforeCommands.processed);
  EXPECT_EQ(commandStats.failed, beforeCommands.failed);
  EXPECT_FALSE(worldThread.serverErrorOccurred());
  EXPECT_EQ(result.species, "human");
  EXPECT_EQ(result.shipUpgrades, shipUpgrades);

  worldThread.stop();
}

TEST(MulticorePhaseTest, Phase5WorldTickSnapshotReusesSectorPacketPrep) {
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
      {"storageGenerationPlanning", false},
      {"storageGenerationPlanningDifferentialCheck", false},
      {"packetPreparationSectorPrefill", false},
      {"packetPreparationSectorPrefillDifferentialCheck", false},
      {"subsystemBaselineMetrics", false}}}});

  WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);

  ASSERT_TRUE(worldServer.addClient(1, SpawnTargetPosition(Vec2F(32, 32)), true));
  ASSERT_TRUE(worldServer.addClient(2, SpawnTargetPosition(Vec2F(32, 32)), true));
  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));
  acknowledgeClientWindow(worldServer, 2, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));

  worldServer.update(1.0f / 60.0f);
  auto stats = worldServer.packetPreparationStats();
  EXPECT_EQ(stats.ticks, 1u);
  EXPECT_EQ(stats.monitoringRegionBuilds, 2u);
  EXPECT_GE(stats.monitoringRegionRects, 2u);
  EXPECT_GE(stats.monitoringRegionSplitRects, stats.monitoringRegionRects);
  EXPECT_GE(stats.monitoringRegionReuses, stats.monitoringRegionRects * 2);
  EXPECT_GT(stats.sectorPacketCacheMisses, 0u);
  EXPECT_GT(stats.sectorPacketCacheHits, 0u);
  EXPECT_TRUE(hasTimingSample(worldServer.updateTimingStatus(), "packetPreparation"));
}

TEST(MulticorePhaseTest, ServerOptimizationSectorClientFanoutQueuesOnlySubscribedClients) {
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
      {"storageGenerationPlanning", false},
      {"storageGenerationPlanningDifferentialCheck", false},
      {"packetPreparationSectorPrefill", false},
      {"packetPreparationSectorPrefillDifferentialCheck", false},
      {"subsystemBaselineMetrics", false}}}});

  WorldServer worldServer(Vec2U(2048, 64), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);

  ASSERT_TRUE(worldServer.addClient(1, SpawnTargetPosition(Vec2F(16, 36)), true));
  ASSERT_TRUE(worldServer.addClient(2, SpawnTargetPosition(Vec2F(1536, 36)), true));
  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(0, 32), Vec2I(16, 16)));
  acknowledgeClientWindow(worldServer, 2, RectI::withSize(Vec2I(1536, 32), Vec2I(16, 16)));

  worldServer.update(1.0f / 60.0f);
  worldServer.getOutgoingPackets(1);
  worldServer.getOutgoingPackets(2);

  auto liquidId = firstTestLiquidId();
  ASSERT_NE(liquidId, EmptyLiquidId);

  auto beforeFanout = worldServer.packetPreparationStats();
  worldServer.modifyLiquid(Vec2I(8, 36), liquidId, 1.0f);
  auto afterFanout = worldServer.packetPreparationStats();
  EXPECT_EQ(afterFanout.sectorClientFanoutLookups, beforeFanout.sectorClientFanoutLookups + 1);
  EXPECT_EQ(afterFanout.sectorClientFanoutRecipients, beforeFanout.sectorClientFanoutRecipients + 1);
  EXPECT_EQ(afterFanout.sectorClientFanoutMisses, beforeFanout.sectorClientFanoutMisses);

  worldServer.update(1.0f / 60.0f);
  EXPECT_GT(packetTypeCount(worldServer.getOutgoingPackets(1), PacketType::TileLiquidUpdate), 0u);
  EXPECT_EQ(packetTypeCount(worldServer.getOutgoingPackets(2), PacketType::TileLiquidUpdate), 0u);

  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(1536, 32), Vec2I(16, 16)));
  beforeFanout = worldServer.packetPreparationStats();
  worldServer.modifyLiquid(Vec2I(8, 36), liquidId, 0.5f);
  afterFanout = worldServer.packetPreparationStats();
  EXPECT_EQ(afterFanout.sectorClientFanoutLookups, beforeFanout.sectorClientFanoutLookups + 1);
  EXPECT_EQ(afterFanout.sectorClientFanoutRecipients, beforeFanout.sectorClientFanoutRecipients);
  EXPECT_EQ(afterFanout.sectorClientFanoutMisses, beforeFanout.sectorClientFanoutMisses + 1);
}

TEST(MulticorePhaseTest, ServerOptimizationLiquidNoProcessingLimitCacheUsesBucketedCandidates) {
  auto liquidWorld = make_shared<TestLiquidWorld>();
  List<Vec2I> activePositions{{-36, 0}, {0, 0}, {32, 0}, {100, 0}, {260, 0}};
  for (auto const& position : activePositions) {
    liquidWorld->cells.set(position, CellularLiquidFlowCell<int>{1, 1.0f, 0.0f});
  }

  LiquidCellEngine<int> liquidEngine(testLiquidEngineParameters(), liquidWorld);
  for (auto const& position : activePositions)
    liquidEngine.visitLocation(position);
  liquidEngine.update();

  liquidEngine.setProcessingLimit(0);
  liquidEngine.setNoProcessingLimitRegions({
      RectI::withSize(Vec2I(-40, -8), Vec2I(16, 16)),
      RectI::withSize(Vec2I(96, -8), Vec2I(16, 16)),
      RectI::withSize(Vec2I(256, -8), Vec2I(16, 16))});

  auto builtStats = liquidEngine.noProcessingLimitRegionCacheStats();
  EXPECT_EQ(builtStats.builds, 1u);
  EXPECT_EQ(builtStats.rebuildSkips, 0u);
  EXPECT_EQ(builtStats.regions, 3u);
  EXPECT_GT(builtStats.buckets, 0u);

  liquidEngine.setNoProcessingLimitRegions({
      RectI::withSize(Vec2I(-40, -8), Vec2I(16, 16)),
      RectI::withSize(Vec2I(96, -8), Vec2I(16, 16)),
      RectI::withSize(Vec2I(256, -8), Vec2I(16, 16))});

  auto skippedStats = liquidEngine.noProcessingLimitRegionCacheStats();
  EXPECT_EQ(skippedStats.builds, 1u);
  EXPECT_EQ(skippedStats.rebuildSkips, 1u);
  EXPECT_EQ(skippedStats.regions, 3u);

  liquidEngine.update();
  auto lookupStats = liquidEngine.noProcessingLimitRegionCacheStats();
  EXPECT_GT(lookupStats.lookups, 0u);
  EXPECT_GT(lookupStats.hits, 0u);
  EXPECT_LT(lookupStats.candidates, lookupStats.lookups * lookupStats.regions);
}

TEST(MulticorePhaseTest, ServerOptimizationEntitySerializationStatsAttributePacketPrepByType) {
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
      {"storageGenerationPlanning", false},
      {"storageGenerationPlanningDifferentialCheck", false},
      {"packetPreparationSectorPrefill", false},
      {"packetPreparationSectorPrefillDifferentialCheck", false},
      {"subsystemBaselineMetrics", false}}}});

  WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);

  ASSERT_TRUE(worldServer.addClient(1, SpawnTargetPosition(Vec2F(32, 32)), true));
  ASSERT_TRUE(worldServer.addClient(2, SpawnTargetPosition(Vec2F(32, 32)), true));
  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));
  acknowledgeClientWindow(worldServer, 2, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));

  auto itemDrop = ItemDrop::throwDrop(ItemDescriptor("perfectlygenericitem", 1), Vec2F(32, 32), Vec2F(), Vec2F(), true);
  ASSERT_TRUE(itemDrop);
  worldServer.addEntity(itemDrop, 100);

  worldServer.update(1.0f / 60.0f);
  auto packetPrepStats = worldServer.packetPreparationStats();
  auto createStats = packetPrepStats.entitySerializationStats.ptr(EntityType::ItemDrop);
  ASSERT_TRUE(createStats);
  EXPECT_EQ(createStats->createStoreCalls, 1u);
  EXPECT_GT(createStats->createStoreBytes, 0u);
  EXPECT_EQ(createStats->initialNetStateCalls, 2u);
  EXPECT_GT(createStats->initialNetStateBytes, 0u);
  EXPECT_EQ(createStats->deltaNetStateCalls, 0u);

  worldServer.getOutgoingPackets(1);
  worldServer.getOutgoingPackets(2);
  worldServer.update(1.0f / 60.0f);

  auto packetPrepStatsAfterDelta = worldServer.packetPreparationStats();
  auto deltaStats = packetPrepStatsAfterDelta.entitySerializationStats.ptr(EntityType::ItemDrop);
  ASSERT_TRUE(deltaStats);
  EXPECT_EQ(deltaStats->createStoreCalls, 1u);
  EXPECT_EQ(deltaStats->initialNetStateCalls, 2u);
  EXPECT_EQ(deltaStats->deltaNetStateCalls, createStats->deltaNetStateCalls + 2);
}

TEST(MulticorePhaseTest, ServerOptimizationSkipEmptyEntityUpdateSetsIsGuarded) {
  auto legacyRun = runEmptyEntityUpdateSetSuppressionFixture(false);
  EXPECT_GT(legacyRun.outgoingUpdateSets, 0u);
  EXPECT_EQ(legacyRun.packetPreparationStats.entityUpdateSetPackets, legacyRun.outgoingUpdateSets);
  EXPECT_EQ(legacyRun.packetPreparationStats.emptyEntityUpdateSetPackets, legacyRun.outgoingUpdateSets);
  EXPECT_EQ(legacyRun.packetPreparationStats.emptyEntityUpdateSetSkips, 0u);

  auto optimizedRun = runEmptyEntityUpdateSetSuppressionFixture(true);
  EXPECT_EQ(optimizedRun.outgoingUpdateSets, 0u);
  EXPECT_EQ(optimizedRun.packetPreparationStats.entityUpdateSetPackets, 0u);
  EXPECT_EQ(optimizedRun.packetPreparationStats.emptyEntityUpdateSetPackets, 0u);
  EXPECT_GT(optimizedRun.packetPreparationStats.emptyEntityUpdateSetSkips, 0u);
}

TEST(MulticorePhaseTest, ServerOptimizationSkipEmptySystemWorldUpdatesIsGuarded) {
  auto legacyRun = runEmptySystemWorldUpdateSuppressionFixture(false);
  EXPECT_GT(legacyRun.outgoingUpdates, 0u);
  EXPECT_EQ(legacyRun.emptyOutgoingUpdates, legacyRun.outgoingUpdates);
  EXPECT_EQ(legacyRun.packetStats.updatePackets, legacyRun.outgoingUpdates + 1);
  EXPECT_GT(legacyRun.packetStats.shipUpdateDeltas, 0u);
  EXPECT_GT(legacyRun.packetStats.emptyUpdatePackets, 0u);
  EXPECT_EQ(legacyRun.packetStats.emptyUpdateSkips, 0u);

  auto optimizedRun = runEmptySystemWorldUpdateSuppressionFixture(true);
  EXPECT_EQ(optimizedRun.outgoingUpdates, 0u);
  EXPECT_GT(optimizedRun.packetStats.shipUpdateDeltas, 0u);
  EXPECT_EQ(optimizedRun.packetStats.emptyUpdatePackets, 0u);
  EXPECT_GT(optimizedRun.packetStats.emptyUpdateSkips, 0u);
}

TEST(MulticorePhaseTest, ServerOptimizationWorldStorageTimingStatsTrackSyncAndSkipUnchangedInserts) {
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
      {"storageGenerationPlanning", false},
      {"storageGenerationPlanningDifferentialCheck", false},
      {"packetPreparationSectorPrefill", false},
      {"packetPreparationSectorPrefillDifferentialCheck", false},
      {"subsystemBaselineMetrics", false}}}});

  WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);
  worldServer.generateRegion(RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));

  worldServer.sync();
  auto firstSyncStats = worldServer.storageTimingStats();
  EXPECT_GE(firstSyncStats.syncs, 1u);
  EXPECT_GT(firstSyncStats.syncedSectors, 0u);
  EXPECT_GT(firstSyncStats.tileStoreSectors, 0u);
  EXPECT_GT(firstSyncStats.tileStoreBytes, 0u);
  EXPECT_GT(firstSyncStats.sectorCopies, 0u);
  EXPECT_GT(firstSyncStats.compressionCalls, 0u);
  EXPECT_GT(firstSyncStats.btreeInserts, 0u);
  EXPECT_GT(firstSyncStats.commits, 0u);

  worldServer.sync();
  auto secondSyncStats = worldServer.storageTimingStats();
  EXPECT_EQ(secondSyncStats.syncs, firstSyncStats.syncs + 1);
  EXPECT_GT(secondSyncStats.syncedSectors, firstSyncStats.syncedSectors);
  EXPECT_GT(secondSyncStats.tileStoreSectors, firstSyncStats.tileStoreSectors);
  EXPECT_GT(secondSyncStats.btreeInsertSkips, firstSyncStats.btreeInsertSkips);

  auto chunks = worldServer.readChunks();
  EXPECT_FALSE(chunks.empty());
  auto snapshotStats = worldServer.storageTimingStats();
  EXPECT_EQ(snapshotStats.fullSnapshotExports, secondSyncStats.fullSnapshotExports + 1);
  EXPECT_GE(snapshotStats.fullSnapshotChunks, chunks.size());
  EXPECT_GT(snapshotStats.fullSnapshotBytes, 0u);
}

TEST(ServerMeasurement, DISABLED_GameMechanismSelfWorkloadCapture) {
  auto capture = runGameMechanismSelfWorkloadCapture();

  EXPECT_EQ(capture.clients, 4u);
  EXPECT_EQ(capture.ticks, 240u);
  EXPECT_GT(capture.packets.total, 0u);
  EXPECT_GT(capture.packets.stepUpdates, 0u);
  EXPECT_GT(capture.packets.tileArrays, 0u);
  EXPECT_GT(capture.packets.tileDamageUpdates, 0u);
  EXPECT_GT(capture.packets.entityCreates, 0u);
  EXPECT_GT(capture.packetPreparationStats.ticks, 0u);
  EXPECT_GT(capture.packetPreparationStats.monitoringRegionBuilds, 0u);
  EXPECT_GT(capture.packetPreparationStats.sectorPacketCacheHits + capture.packetPreparationStats.sectorPacketCacheMisses, 0u);
  EXPECT_GT(capture.packetPreparationStats.entityStoreCacheMisses, 0u);
  EXPECT_EQ(capture.packetPreparationStats.entityUpdateSetPackets, capture.packets.entityUpdates);
  EXPECT_GT(capture.packetPreparationStats.entityUpdateSetDeltas, 0u);
  EXPECT_EQ(capture.packets.emptyEntityUpdates, 0u);
  EXPECT_GT(capture.packetPreparationStats.emptyEntityUpdateSetSkips, 0u);
  EXPECT_GT(capture.phase6Stats.liquidBaselineTicks, 0u);
  EXPECT_GT(capture.phase6Stats.liquidNoProcessingLimitRegionCacheRebuildSkips, 0u);
  EXPECT_GT(capture.phase6Stats.fallingBlocksMovedBlocks, 0u);
  EXPECT_GT(capture.phase6Stats.wiringInitialEntities, 0u);
  EXPECT_GT(capture.phase6Stats.wiringEvaluatedEntities, 0u);
  EXPECT_GT(capture.phase6Stats.entityBaselineTicks, 0u);
  EXPECT_GT(capture.phase6Stats.entityUpdatedEntities, 0u);
  EXPECT_GT(capture.phase6Stats.entityIterationCopies, 0u);
  EXPECT_GT(capture.phase6Stats.entitySortedEntities, 0u);
  EXPECT_GT(capture.phase6Stats.entityUpdateMicroseconds, 0u);
  EXPECT_GT(capture.phase6Stats.luaBaselineTicks, 0u);
  EXPECT_GT(capture.phase6Stats.luaScriptContexts, 0u);
  EXPECT_GT(capture.phase6Stats.luaScriptUpdates, 0u);
  EXPECT_GE(capture.phase6Stats.luaScriptUpdateMicroseconds, capture.phase6Stats.luaMaxScriptUpdateMicroseconds);
  EXPECT_GT(capture.storageTimingStats.syncs, 0u);
  EXPECT_GT(capture.storageTimingStats.syncedSectors, 0u);
  EXPECT_GT(capture.storageTimingStats.fullSnapshotExports, 0u);
  EXPECT_GT(capture.storageTimingStats.fullSnapshotBytes, 0u);
  EXPECT_EQ(capture.phase6Stats.storageGenerationPlanningDivergences, 0u);
  EXPECT_EQ(capture.phase6Stats.packetPreparationSectorPrefillDivergences, 0u);

  RecordProperty("packets", static_cast<int64_t>(capture.packets.total));
  RecordProperty("elapsedUs", capture.elapsedMicroseconds);
  RecordProperty("entityUpdated", static_cast<int64_t>(capture.phase6Stats.entityUpdatedEntities));
  RecordProperty("liquidCacheRebuildSkips", static_cast<int64_t>(capture.phase6Stats.liquidNoProcessingLimitRegionCacheRebuildSkips));
  RecordProperty("snapshotBytes", static_cast<int64_t>(capture.storageTimingStats.fullSnapshotBytes));
}

TEST(MulticorePhaseTest, Phase6EntityInitialNetStateWritesAdvancePerClientVersions) {
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
      {"storageGenerationPlanning", false},
      {"storageGenerationPlanningDifferentialCheck", false},
      {"packetPreparationSectorPrefill", false},
      {"packetPreparationSectorPrefillDifferentialCheck", false},
      {"subsystemBaselineMetrics", false}}}});

  WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);

  ASSERT_TRUE(worldServer.addClient(1, SpawnTargetPosition(Vec2F(32, 32)), true));
  ASSERT_TRUE(worldServer.addClient(2, SpawnTargetPosition(Vec2F(32, 32)), true));
  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));
  acknowledgeClientWindow(worldServer, 2, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));

  auto itemDrop = ItemDrop::throwDrop(ItemDescriptor("perfectlygenericitem", 1), Vec2F(32, 32), Vec2F(), Vec2F(), true);
  ASSERT_TRUE(itemDrop);
  worldServer.addEntity(itemDrop, 100);

  worldServer.update(1.0f / 60.0f);
  auto statsAfterCreate = worldServer.packetPreparationStats();
  auto createStats = statsAfterCreate.entitySerializationStats.ptr(EntityType::ItemDrop);
  ASSERT_TRUE(createStats);
  EXPECT_EQ(createStats->createStoreCalls, 1u);
  EXPECT_EQ(createStats->initialNetStateCalls, 2u);
  EXPECT_EQ(createStats->deltaNetStateCalls, 0u);
  EXPECT_EQ(statsAfterCreate.entityStoreCacheMisses, 1u);
  EXPECT_EQ(statsAfterCreate.entityStoreCacheHits, 1u);

  worldServer.getOutgoingPackets(1);
  worldServer.getOutgoingPackets(2);
  worldServer.update(1.0f / 60.0f);

  auto statsAfterDelta = worldServer.packetPreparationStats();
  auto deltaStats = statsAfterDelta.entitySerializationStats.ptr(EntityType::ItemDrop);
  ASSERT_TRUE(deltaStats);
  EXPECT_EQ(deltaStats->createStoreCalls, 1u);
  EXPECT_EQ(deltaStats->initialNetStateCalls, 2u);
  EXPECT_EQ(deltaStats->deltaNetStateCalls, 2u);
  EXPECT_GE(statsAfterDelta.entityNetStateCacheMisses, statsAfterCreate.entityNetStateCacheMisses + 2);
}

TEST(MulticorePhaseTest, Phase6PacketSectorPrefillMatchesSerialSectorPackets) {
  auto serialPayloads = preparedTileArrayUpdatePayloads(false);
  auto prefilledPayloads = preparedTileArrayUpdatePayloads(true);

  ASSERT_EQ(serialPayloads.size(), prefilledPayloads.size());
  EXPECT_EQ(serialPayloads, prefilledPayloads);
}

TEST(MulticorePhaseTest, Phase6EntityPacketPreparationMatchesWithImmutableCreateSnapshots) {
  auto serialPayloads = preparedEntityPacketPayloads(false);
  auto prefilledPayloads = preparedEntityPacketPayloads(true);

  ASSERT_EQ(serialPayloads.size(), prefilledPayloads.size());
  EXPECT_EQ(serialPayloads, prefilledPayloads);
}

TEST(MulticorePhaseTest, Phase6StorageGenerationPlanningIsGuarded) {
  {
    WorldServer worldServer(Vec2U(256, 128), File::ephemeralFile());
    auto stats = worldServer.phase6WorldParallelismStats();
    EXPECT_TRUE(stats.storageGenerationPlanningEnabled);
    EXPECT_TRUE(stats.storageGenerationPlanningDifferentialCheckEnabled);
    EXPECT_TRUE(stats.packetPreparationSectorPrefillEnabled);
    EXPECT_TRUE(stats.packetPreparationSectorPrefillDifferentialCheckEnabled);
    EXPECT_TRUE(stats.subsystemBaselineMetricsEnabled);
    EXPECT_FALSE(stats.mutationParallelismRequested);
    EXPECT_FALSE(stats.mutationParallelismBlockedByImplementationGate);
  }

  {
    ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
        {"storageGenerationPlanning", false},
        {"storageGenerationPlanningDifferentialCheck", false},
        {"packetPreparationSectorPrefill", false},
        {"packetPreparationSectorPrefillDifferentialCheck", false},
        {"subsystemBaselineMetrics", false}}}});

    WorldServer worldServer(Vec2U(256, 128), File::ephemeralFile());
    auto stats = worldServer.phase6WorldParallelismStats();
    EXPECT_FALSE(stats.storageGenerationPlanningEnabled);
    EXPECT_FALSE(stats.storageGenerationPlanningDifferentialCheckEnabled);
    EXPECT_FALSE(stats.packetPreparationSectorPrefillEnabled);
    EXPECT_FALSE(stats.packetPreparationSectorPrefillDifferentialCheckEnabled);
    EXPECT_FALSE(stats.subsystemBaselineMetricsEnabled);
    EXPECT_FALSE(stats.mutationParallelismRequested);
  }

  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
      {"storageGenerationPlanning", true},
      {"storageGenerationPlanningWorkerThreads", 2},
      {"storageGenerationPlanningMinimumSectors", 0},
      {"storageGenerationPlanningDifferentialCheck", true}}}});

  WorldServer worldServer(Vec2U(512, 256), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::High);
  worldServer.setSpawningEnabled(false);
  worldServer.signalRegion(RectI::withSize(Vec2I(384, 64), Vec2I(96, 96)));

  WorldServer::Phase6WorldParallelismStats stats;
  for (size_t i = 0; i < 20; ++i) {
    worldServer.update(1.0f / 60.0f);
    stats = worldServer.phase6WorldParallelismStats();
    if (stats.storageGenerationPlanningTicks > 0)
      break;
  }

  EXPECT_TRUE(stats.storageGenerationPlanningEnabled);
  EXPECT_TRUE(stats.storageGenerationPlanningDifferentialCheckEnabled);
  EXPECT_GT(stats.storageGenerationPlanningTicks, 0u);
  EXPECT_GT(stats.storageGenerationPlanningSectors, 0u);
  EXPECT_GT(stats.storageGenerationPlanningSerialTicks + stats.storageGenerationPlanningParallelTicks, 0u);
  EXPECT_GT(stats.storageGenerationPlanningDifferentialChecks, 0u);
  EXPECT_EQ(stats.storageGenerationPlanningDivergences, 0u);
}

TEST(MulticorePhaseTest, Phase6MutationParallelismRequiresExplicitGates) {
  {
    ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
        {"storageGenerationPlanning", false},
        {"packetPreparationSectorPrefill", false},
        {"subsystemBaselineMetrics", false},
        {"liquidMutationParallelism", true},
        {"entityMutationParallelism", true}}}});

    WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
    auto stats = worldServer.phase6WorldParallelismStats();
    uint32_t expectedSubsystems = WorldServer::LiquidMutationParallelismSubsystem | WorldServer::EntityMutationParallelismSubsystem;
    EXPECT_TRUE(stats.mutationParallelismRequested);
    EXPECT_TRUE(stats.mutationParallelismBlockedByFixedSeedGate);
    EXPECT_TRUE(stats.mutationParallelismBlockedByDependencyGate);
    EXPECT_TRUE(stats.mutationParallelismBlockedByModVisibilityGate);
    EXPECT_TRUE(stats.mutationParallelismBlockedByImplementationGate);
    EXPECT_EQ(stats.mutationParallelismRequestedSubsystems, expectedSubsystems);
    EXPECT_EQ(stats.mutationParallelismBlockedByFixedSeedGateSubsystems, expectedSubsystems);
    EXPECT_EQ(stats.mutationParallelismBlockedByDependencyGateSubsystems, expectedSubsystems);
    EXPECT_EQ(stats.mutationParallelismBlockedByModVisibilityGateSubsystems, expectedSubsystems);
    EXPECT_EQ(stats.mutationParallelismBlockedByImplementationGateSubsystems, expectedSubsystems);
  }

  {
    ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
        {"storageGenerationPlanning", false},
        {"packetPreparationSectorPrefill", false},
        {"subsystemBaselineMetrics", false},
        {"liquidMutationParallelism", true},
        {"fallingBlockMutationParallelism", true},
        {"wiringMutationParallelism", true},
        {"entityMutationParallelism", true},
        {"luaMutationParallelism", true},
        {"mutationParallelismFixedSeedSignatures", true},
        {"mutationParallelismDependencyAnalysis", true},
        {"mutationParallelismModVisibilityContract", true}}}});

    WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
    auto stats = worldServer.phase6WorldParallelismStats();
    uint32_t expectedSubsystems = WorldServer::LiquidMutationParallelismSubsystem | WorldServer::FallingBlockMutationParallelismSubsystem | WorldServer::WiringMutationParallelismSubsystem | WorldServer::EntityMutationParallelismSubsystem | WorldServer::LuaMutationParallelismSubsystem;
    EXPECT_TRUE(stats.mutationParallelismRequested);
    EXPECT_FALSE(stats.mutationParallelismBlockedByFixedSeedGate);
    EXPECT_FALSE(stats.mutationParallelismBlockedByDependencyGate);
    EXPECT_FALSE(stats.mutationParallelismBlockedByModVisibilityGate);
    EXPECT_TRUE(stats.mutationParallelismBlockedByImplementationGate);
    EXPECT_EQ(stats.mutationParallelismRequestedSubsystems, expectedSubsystems);
    EXPECT_EQ(stats.mutationParallelismBlockedByFixedSeedGateSubsystems, 0u);
    EXPECT_EQ(stats.mutationParallelismBlockedByDependencyGateSubsystems, 0u);
    EXPECT_EQ(stats.mutationParallelismBlockedByModVisibilityGateSubsystems, 0u);
    EXPECT_EQ(stats.mutationParallelismBlockedByImplementationGateSubsystems, expectedSubsystems);
  }
}

TEST(MulticorePhaseTest, Phase6PacketSectorPrefillIsGuarded) {
  ConfigurationValueGuard configGuard("worldServerConfigOverrides", JsonObject{{"phase6WorldParallelism", JsonObject{
      {"packetPreparationSectorPrefill", true},
      {"packetPreparationSectorPrefillWorkerThreads", 2},
      {"packetPreparationSectorPrefillMinimumSectors", 0},
      {"packetPreparationSectorPrefillDifferentialCheck", true}}}});

  WorldServer worldServer(Vec2U(64, 64), File::ephemeralFile());
  worldServer.setFidelity(WorldServerFidelity::Minimum);
  worldServer.setSpawningEnabled(false);

  ASSERT_TRUE(worldServer.addClient(1, SpawnTargetPosition(Vec2F(32, 32)), true));
  ASSERT_TRUE(worldServer.addClient(2, SpawnTargetPosition(Vec2F(32, 32)), true));
  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));
  acknowledgeClientWindow(worldServer, 2, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));

  worldServer.update(1.0f / 60.0f);
  auto stats = worldServer.phase6WorldParallelismStats();
  EXPECT_TRUE(stats.packetPreparationSectorPrefillEnabled);
  EXPECT_TRUE(stats.packetPreparationSectorPrefillDifferentialCheckEnabled);
  EXPECT_GT(stats.packetPreparationSectorPrefillTicks, 0u);
  EXPECT_GT(stats.packetPreparationSectorPrefillSectors, 0u);
  EXPECT_GT(stats.packetPreparationSectorPrefillSerialTicks + stats.packetPreparationSectorPrefillParallelTicks, 0u);
  EXPECT_GT(stats.packetPreparationSectorPrefillDifferentialChecks, 0u);
  EXPECT_EQ(stats.packetPreparationSectorPrefillDivergences, 0u);
}

TEST(MulticorePhaseTest, Phase6SubsystemBaselineMetricsAreGuardedAndStable) {
  auto firstSignature = phase6SubsystemBaselineSignature();
  auto secondSignature = phase6SubsystemBaselineSignature();

  EXPECT_EQ(firstSignature, secondSignature);
}
