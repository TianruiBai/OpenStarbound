#include "StarConfiguration.hpp"
#include "StarDataStreamDevices.hpp"
#include "StarFile.hpp"
#include "StarItemDrop.hpp"
#include "StarLiquidsDatabase.hpp"
#include "StarNetPackets.hpp"
#include "StarRoot.hpp"
#include "StarTime.hpp"
#include "StarUniverseConnection.hpp"
#include "StarUniverseServer.hpp"
#include "StarWorldClientState.hpp"
#include "StarWorldServer.hpp"
#include "StarWorldServerThread.hpp"

#include "gtest/gtest.h"

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

LiquidId firstTestLiquidId() {
  auto liquidsDatabase = Root::singleton().liquidsDatabase();
  for (auto const& liquidName : liquidsDatabase->liquidNames()) {
    if (liquidName != "empty")
      return liquidsDatabase->liquidId(liquidName);
  }
  return EmptyLiquidId;
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

  if (!worldServer.addClient(1, SpawnTargetPosition(Vec2F(32, 32)), true)) {
    ADD_FAILURE() << "Could not add baseline metrics test client";
    return {};
  }
  acknowledgeClientWindow(worldServer, 1, RectI::withSize(Vec2I(24, 24), Vec2I(16, 16)));

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
  EXPECT_GT(stats.fallingBlocksBaselineTicks, 0u);
  EXPECT_GT(stats.wiringBaselineTicks, 0u);
  EXPECT_GT(stats.entityBaselineTicks, 0u);
  EXPECT_GT(stats.luaBaselineTicks, 0u);
  EXPECT_GE(stats.fallingBlocksProcessedPositions, stats.fallingBlocksMovedBlocks);
  EXPECT_GE(stats.wiringLoadedEntities, stats.wiringEvaluatedEntities);
  EXPECT_GE(stats.entityUpdatedEntities, stats.entityTileEntities);
  EXPECT_GE(stats.luaScriptUpdates, stats.luaScriptContexts);

  return List<uint64_t>{
      stats.liquidBaselineTicks,
      stats.liquidActiveCells,
      stats.liquidMonitoringRegions,
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
  EXPECT_GT(stats.sectorPacketCacheMisses, 0u);
  EXPECT_GT(stats.sectorPacketCacheHits, 0u);
  EXPECT_TRUE(hasTimingSample(worldServer.updateTimingStatus(), "packetPreparation"));
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
    EXPECT_TRUE(stats.mutationParallelismRequested);
    EXPECT_TRUE(stats.mutationParallelismBlockedByFixedSeedGate);
    EXPECT_TRUE(stats.mutationParallelismBlockedByDependencyGate);
    EXPECT_TRUE(stats.mutationParallelismBlockedByModVisibilityGate);
    EXPECT_TRUE(stats.mutationParallelismBlockedByImplementationGate);
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
    EXPECT_TRUE(stats.mutationParallelismRequested);
    EXPECT_FALSE(stats.mutationParallelismBlockedByFixedSeedGate);
    EXPECT_FALSE(stats.mutationParallelismBlockedByDependencyGate);
    EXPECT_FALSE(stats.mutationParallelismBlockedByModVisibilityGate);
    EXPECT_TRUE(stats.mutationParallelismBlockedByImplementationGate);
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
