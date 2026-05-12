#include "StarUniverseServer.hpp"
#include "StarConfiguration.hpp"
#include "StarFile.hpp"
#include "StarNetPackets.hpp"
#include "StarPlayer.hpp"
#include "StarPlayerFactory.hpp"
#include "StarRoot.hpp"
#include "StarTime.hpp"
#include "StarVersioningDatabase.hpp"

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

Json loadVersionedJsonFile(String const& file, String const& identifier) {
  return Root::singleton().versioningDatabase()->loadVersionedJson(VersionedJson::readFile(file), identifier);
}

}

TEST(ServerTest, Run) {
  TemporaryUniverseStorage storage;
  {
    UniverseServer server(storage.universe);
    server.start();
    server.stop();
    server.join();
  }
}

TEST(ServerTest, ServerStatusIncludesLoopTimings) {
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

TEST(ServerTest, PendingHandshakeStateMachineAcceptsLocalClient) {
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{{"usePendingConnectionStateMachine", true}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto& root = Root::singleton();
  auto player = root.playerFactory()->create();
  player->finalizeCreation();
  player->setName("test");
  player->setSpecies("human");
  player->setShipSpecies("human");

  auto connection = server.addLocalClient();
  connection.pushSingle(make_shared<ProtocolRequestPacket>(StarProtocolVersion));
  ASSERT_TRUE(connection.sendAll(1000));

  PacketPtr protocolPacket;
  ASSERT_TRUE(waitUntil([&]() {
    connection.receive();
    protocolPacket = connection.pullSingle();
    return protocolPacket != nullptr;
  }));

  auto protocolResponse = as<ProtocolResponsePacket>(protocolPacket);
  ASSERT_TRUE(protocolResponse);
  ASSERT_TRUE(protocolResponse->allowed);

  connection.packetSocket().setNetRules(LegacyVersion);
  connection.pushSingle(make_shared<ClientConnectPacket>(root.assets()->digest(), false, player->uuid(), player->name(), player->shipSpecies(), WorldChunks(), player->shipUpgrades(), true, ""));
  ASSERT_TRUE(connection.sendAll(1000));

  PacketPtr connectPacket;
  ASSERT_TRUE(waitUntil([&]() {
    connection.receive();
    connectPacket = connection.pullSingle();
    return connectPacket != nullptr;
  }));

  ASSERT_TRUE(as<ConnectSuccessPacket>(connectPacket));
  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.clients == 1 && status.pendingHandshakes == 0;
  });

  EXPECT_EQ(status.pendingHandshakeAccepted, 1u);
  EXPECT_EQ(status.pendingHandshakeFinalized, 1u);
  EXPECT_EQ(status.pendingHandshakeRejected, 0u);
  EXPECT_EQ(status.pendingHandshakeTimedOut, 0u);

  server.stop();
  server.join();
}

TEST(ServerTest, PendingHandshakeStateMachineRejectsProtocolMismatch) {
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

TEST(ServerTest, AsyncPersistenceCompletesTriggeredStorage) {
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{
      {"useAsyncPersistence", true},
      {"persistenceWorkerThreads", 1},
      {"maxQueuedPersistenceSnapshots", 128},
      {"maxPersistenceWriteRetries", 0}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.persistenceBatchesCompleted > 0 && status.persistenceSnapshotsWritten >= 2;
  });

  EXPECT_EQ(status.persistenceFailures, 0u);
  EXPECT_EQ(status.persistenceQueueFullFallbacks, 0u);
  EXPECT_EQ(status.persistenceSynchronousFallbacks, 0u);
  auto universeSettingsFile = File::relativeTo(storage.universe, "universe.dat");
  auto tempWorldIndexFile = File::relativeTo(storage.universe, "tempworlds.index");
  ASSERT_TRUE(File::isFile(universeSettingsFile));
  ASSERT_TRUE(File::isFile(tempWorldIndexFile));
  EXPECT_TRUE(loadVersionedJsonFile(universeSettingsFile, "UniverseSettings").isType(Json::Type::Object));
  EXPECT_TRUE(loadVersionedJsonFile(tempWorldIndexFile, "TempWorldIndex").isType(Json::Type::Object));

  server.stop();
  server.join();
}

TEST(ServerTest, AsyncPersistenceFallsBackWhenQueueIsFull) {
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
  auto universeSettingsFile = File::relativeTo(storage.universe, "universe.dat");
  auto tempWorldIndexFile = File::relativeTo(storage.universe, "tempworlds.index");
  ASSERT_TRUE(File::isFile(universeSettingsFile));
  ASSERT_TRUE(File::isFile(tempWorldIndexFile));
  EXPECT_TRUE(loadVersionedJsonFile(universeSettingsFile, "UniverseSettings").isType(Json::Type::Object));
  EXPECT_TRUE(loadVersionedJsonFile(tempWorldIndexFile, "TempWorldIndex").isType(Json::Type::Object));

  server.stop();
  server.join();
}
