#include "StarConfiguration.hpp"
#include "StarFile.hpp"
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
      {"useAsyncPersistence", true},
      {"persistenceWorkerThreads", 1},
      {"maxQueuedPersistenceSnapshots", 1},
      {"maxPersistenceWriteRetries", 0}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.persistenceQueueFullFallbacks > 0 && status.persistenceSnapshotsWritten >= 2;
  });

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

TEST(MulticorePhaseTest, Phase5WorldTickSnapshotReusesSectorPacketPrep) {
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
