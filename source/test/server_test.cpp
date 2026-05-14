#include "StarUniverseServer.hpp"
#include "StarUniverseConnection.hpp"
#include "StarConfiguration.hpp"
#include "StarFile.hpp"
#include "StarHostAddress.hpp"
#include "StarLogging.hpp"
#include "StarNetPackets.hpp"
#include "StarPlayer.hpp"
#include "StarPlayerFactory.hpp"
#include "StarRoot.hpp"
#include "StarSha256.hpp"
#include "StarTcp.hpp"
#include "StarTime.hpp"
#include "StarVersioningDatabase.hpp"

#include "gtest/gtest.h"
#include "gtest/gtest-spi.h"

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

UniverseServer::ServerStatus waitForServerStatus(UniverseServer& server, function<bool(UniverseServer::ServerStatus const&)> predicate,
    unsigned timeoutMillis = 3000) {
  UniverseServer::ServerStatus status;
  EXPECT_TRUE(waitUntil([&]() {
    status = server.serverStatus();
    return predicate(status);
  }, timeoutMillis));
  return status;
}

Json loadVersionedJsonFile(String const& file, String const& identifier) {
  return Root::singleton().versioningDatabase()->loadVersionedJson(VersionedJson::readFile(file), identifier);
}

PlayerPtr makeTestPlayer(String const& name) {
  auto player = Root::singleton().playerFactory()->create();
  player->finalizeCreation();
  player->setName(name);
  player->setSpecies("human");
  player->setShipSpecies("human");
  return player;
}

PacketPtr waitForPacket(UniverseConnection& connection, unsigned timeoutMillis = 3000) {
  PacketPtr packet;
  EXPECT_TRUE(waitUntil([&]() {
    connection.receive();
    packet = connection.pullSingle();
    return packet != nullptr;
  }, timeoutMillis));
  return packet;
}

shared_ptr<ClientConnectPacket> makeClientConnectPacket(PlayerPtr const& player, String account = {}, bool allowAssetsMismatch = true,
    ByteArray assetsDigest = {}, bool openStarbound = true) {
  if (assetsDigest.empty())
    assetsDigest = Root::singleton().assets()->digest();

  Json info;
  if (openStarbound)
    info = JsonObject{{"brand", "OpenStarbound"}, {"openProtocolVersion", OpenProtocolVersion}};

  return make_shared<ClientConnectPacket>(std::move(assetsDigest), allowAssetsMismatch, player->uuid(), player->name(), player->shipSpecies(),
      WorldChunks(), player->shipUpgrades(), true, std::move(account), std::move(info));
}

struct RemoteConnectResult {
  UniverseConnection connection;
  PacketPtr packet;
};

PacketPtr completeClientHandshake(UniverseConnection& connection, PlayerPtr const& player, String account = {}, String password = {},
  bool openStarbound = true, bool allowAssetsMismatch = true, ByteArray assetsDigest = {}, bool sendHandshakeResponse = true,
  unsigned timeoutMillis = 3000) {
  auto protocolRequest = make_shared<ProtocolRequestPacket>(StarProtocolVersion);
  if (openStarbound)
    protocolRequest->setCompressionMode(PacketCompressionMode::Enabled);
  connection.pushSingle(protocolRequest);
  EXPECT_TRUE(connection.sendAll(1000));

  auto protocolPacket = waitForPacket(connection, timeoutMillis);
  auto protocolResponse = as<ProtocolResponsePacket>(protocolPacket);
  EXPECT_TRUE(protocolResponse);
  if (!protocolResponse || !protocolResponse->allowed)
    return protocolPacket;

  NetCompatibilityRules netRules(LegacyVersion);
  bool legacyServer = !openStarbound || protocolResponse->compressionMode() != PacketCompressionMode::Enabled;
  if (!legacyServer) {
    if (protocolResponse->info) {
      netRules.setVersion(protocolResponse->info.getUInt("openProtocolVersion", 1));
      auto compressionMode = NetCompressionModeNames.maybeLeft(protocolResponse->info.getString("compression", "None"));
      EXPECT_TRUE(compressionMode);
      if (!compressionMode)
        return protocolPacket;
      if (auto compressedSocket = as<CompressedPacketSocket>(&connection.packetSocket()))
        compressedSocket->setCompressionStreamEnabled(*compressionMode == NetCompressionMode::Zstd);
    } else {
      netRules.setVersion(1);
      if (auto compressedSocket = as<CompressedPacketSocket>(&connection.packetSocket()))
        compressedSocket->setCompressionStreamEnabled(true);
    }
  }
  connection.packetSocket().setNetRules(netRules);
  connection.pushSingle(makeClientConnectPacket(player, account, allowAssetsMismatch, std::move(assetsDigest), openStarbound));
  EXPECT_TRUE(connection.sendAll(1000));

  auto packet = waitForPacket(connection, timeoutMillis);
  if (auto challenge = as<HandshakeChallengePacket>(packet)) {
    if (!sendHandshakeResponse)
      return packet;

    ByteArray passAccountSalt = (password + account).utf8Bytes();
    passAccountSalt.append(challenge->passwordSalt);
    connection.pushSingle(make_shared<HandshakeResponsePacket>(sha256(passAccountSalt)));
    EXPECT_TRUE(connection.sendAll(1000));
    packet = waitForPacket(connection, timeoutMillis);
  }

  return packet;
}

uint16_t findAvailableTcpPort() {
  uint16_t const firstPort = 56000 + static_cast<uint16_t>(Time::monotonicMilliseconds() % 5000);
  for (uint16_t i = 0; i < 5000; ++i) {
    uint16_t port = 56000 + static_cast<uint16_t>((firstPort + i - 56000) % 5000);
    try {
      TcpServer server({HostAddress::localhost(), port});
      server.stop();
      return port;
    } catch (std::exception const&) {}
  }

  throw StarException("Could not find an available TCP port for server test");
}

RemoteConnectResult connectRemoteClient(UniverseServer& server, PlayerPtr const& player, String account = {}, String password = {},
    bool openStarbound = true, bool allowAssetsMismatch = true, ByteArray assetsDigest = {}, bool sendHandshakeResponse = true) {
  auto connection = server.addRemoteLocalClient(HostAddress::localhost());
  auto packet = completeClientHandshake(connection, player, std::move(account), std::move(password), openStarbound,
      allowAssetsMismatch, std::move(assetsDigest), sendHandshakeResponse);
  return {std::move(connection), packet};
}

size_t drainAvailablePackets(List<UniverseConnection>& connections) {
  size_t received = 0;
  for (auto& connection : connections) {
    connection.receive();
    while (connection.pullSingle())
      ++received;
  }
  return received;
}

size_t drainPacketBursts(List<UniverseConnection>& connections, unsigned quietMillis = 20) {
  size_t received = 0;
  auto quietDeadline = Time::monotonicMilliseconds() + quietMillis;
  while (Time::monotonicMilliseconds() < quietDeadline) {
    size_t drained = drainAvailablePackets(connections);
    received += drained;
    if (drained != 0)
      quietDeadline = Time::monotonicMilliseconds() + quietMillis;
    else
      Thread::sleep(1);
  }
  return received;
}

size_t waitForReceivedPackets(List<UniverseConnection>& connections, size_t expectedPackets, unsigned timeoutMillis = 10000) {
  size_t received = 0;
  EXPECT_TRUE(waitUntil([&]() {
    received += drainAvailablePackets(connections);
    return received >= expectedPackets;
  }, timeoutMillis));
  return received;
}

uint64_t counterDelta(uint64_t after, uint64_t before) {
  return after >= before ? after - before : 0;
}

uint64_t totalOwnedConnections(List<UniverseConnectionServer::NetworkWorkerStats> const& stats) {
  uint64_t total = 0;
  for (auto const& workerStats : stats)
    total += workerStats.ownedConnections;
  return total;
}

uint64_t totalQueuedSendPackets(List<UniverseConnectionServer::NetworkWorkerStats> const& stats) {
  uint64_t total = 0;
  for (auto const& workerStats : stats)
    total += workerStats.queuedSendPackets;
  return total;
}

uint64_t totalEagerSendPackets(List<UniverseConnectionServer::NetworkWorkerStats> const& stats) {
  uint64_t total = 0;
  for (auto const& workerStats : stats)
    total += workerStats.eagerSendPackets;
  return total;
}

uint64_t totalWorkerSendPackets(List<UniverseConnectionServer::NetworkWorkerStats> const& stats) {
  uint64_t total = 0;
  for (auto const& workerStats : stats)
    total += workerStats.workerSendPackets;
  return total;
}

uint64_t totalWakeups(List<UniverseConnectionServer::NetworkWorkerStats> const& stats) {
  uint64_t total = 0;
  for (auto const& workerStats : stats)
    total += workerStats.wakeups;
  return total;
}

uint64_t totalIdleTimedWaits(List<UniverseConnectionServer::NetworkWorkerStats> const& stats) {
  uint64_t total = 0;
  for (auto const& workerStats : stats)
    total += workerStats.idleTimedWaits;
  return total;
}

struct QueueSendMeasurement {
  bool queueOnly = false;
  size_t clients = 0;
  size_t packetsSent = 0;
  size_t packetsReceived = 0;
  uint64_t queuedPackets = 0;
  uint64_t eagerPackets = 0;
  uint64_t workerPackets = 0;
  uint64_t wakeups = 0;
  uint64_t idleTimedWaits = 0;
  int64_t elapsedMicroseconds = 0;
};

QueueSendMeasurement runQueueSendMeasurement(bool queueOnly) {
  size_t const ClientCount = 8;
  size_t const Rounds = 16;

  UniverseConnectionServer server([](UniverseConnectionServer*, ConnectionId, List<PacketPtr>) {}, 2, queueOnly);
  List<UniverseConnection> connections;
  List<ConnectionId> clientIds;
  for (ConnectionId clientId = 1; clientId <= static_cast<ConnectionId>(ClientCount); ++clientId) {
    auto pair = LocalPacketSocket::openPair();
    server.addConnection(clientId, UniverseConnection(std::move(pair.first)));
    connections.append(UniverseConnection(std::move(pair.second)));
    clientIds.append(clientId);
  }

  EXPECT_EQ(clientIds.size(), ClientCount);
  EXPECT_TRUE(waitUntil([&]() {
    return totalOwnedConnections(server.workerStats()) == ClientCount;
  }, 10000));
  drainPacketBursts(connections);

  auto beforeStats = server.workerStats();
  auto sendStart = Time::monotonicMicroseconds();

  size_t packetsSent = 0;
  size_t packetsReceived = 0;
  for (size_t round = 0; round < Rounds; ++round) {
    for (auto clientId : clientIds) {
      server.sendPackets(clientId, {make_shared<PausePacket>((round % 2) == 0, 1.0f)});
      packetsSent += 1;
    }
    packetsReceived += drainPacketBursts(connections, 2);
  }

  if (packetsReceived < packetsSent)
    packetsReceived += waitForReceivedPackets(connections, packetsSent - packetsReceived);
  List<UniverseConnectionServer::NetworkWorkerStats> afterStats;
  EXPECT_TRUE(waitUntil([&]() {
    afterStats = server.workerStats();
    auto queuedDelta = counterDelta(totalQueuedSendPackets(afterStats), totalQueuedSendPackets(beforeStats));
    auto eagerDelta = counterDelta(totalEagerSendPackets(afterStats), totalEagerSendPackets(beforeStats));
    auto workerDelta = counterDelta(totalWorkerSendPackets(afterStats), totalWorkerSendPackets(beforeStats));
    return queuedDelta >= packetsSent && (queueOnly ? workerDelta >= packetsSent : eagerDelta >= packetsSent);
  }, 10000));

  QueueSendMeasurement measurement;
  measurement.queueOnly = queueOnly;
  measurement.clients = ClientCount;
  measurement.packetsSent = packetsSent;
  measurement.packetsReceived = packetsReceived;
  measurement.queuedPackets = counterDelta(totalQueuedSendPackets(afterStats), totalQueuedSendPackets(beforeStats));
  measurement.eagerPackets = counterDelta(totalEagerSendPackets(afterStats), totalEagerSendPackets(beforeStats));
  measurement.workerPackets = counterDelta(totalWorkerSendPackets(afterStats), totalWorkerSendPackets(beforeStats));
  measurement.wakeups = counterDelta(totalWakeups(afterStats), totalWakeups(beforeStats));
  measurement.idleTimedWaits = counterDelta(totalIdleTimedWaits(afterStats), totalIdleTimedWaits(beforeStats));
  measurement.elapsedMicroseconds = Time::monotonicMicroseconds() - sendStart;

  Logger::info("ServerMeasurement queueOnly={} clients={} sent={} received={} queuedPackets={} eagerPackets={} workerPackets={} wakeups={} idleTimedWaits={} elapsedUs={}",
      measurement.queueOnly,
      measurement.clients,
      measurement.packetsSent,
      measurement.packetsReceived,
      measurement.queuedPackets,
      measurement.eagerPackets,
      measurement.workerPackets,
      measurement.wakeups,
      measurement.idleTimedWaits,
      measurement.elapsedMicroseconds);
  std::cout << "ServerMeasurement queueOnly=" << measurement.queueOnly
            << " clients=" << measurement.clients
            << " sent=" << measurement.packetsSent
            << " received=" << measurement.packetsReceived
            << " queuedPackets=" << measurement.queuedPackets
            << " eagerPackets=" << measurement.eagerPackets
            << " workerPackets=" << measurement.workerPackets
            << " wakeups=" << measurement.wakeups
            << " idleTimedWaits=" << measurement.idleTimedWaits
            << " elapsedUs=" << measurement.elapsedMicroseconds
            << std::endl;

  return measurement;
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

TEST(ServerTest, WorldStatsCommandReportsDiagnostics) {
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);

  auto output = server.adminCommand("worldstats");
  EXPECT_TRUE(output.contains("World stats: active=0, system=0"));

  auto statusOutput = server.adminCommand("serverstatus");
  EXPECT_TRUE(statusOutput.contains("regions="));
  EXPECT_TRUE(statusOutput.contains("sectorFanout="));
  EXPECT_TRUE(statusOutput.contains("entityUpdateSets="));
  EXPECT_TRUE(statusOutput.contains("liquidCache="));
  EXPECT_TRUE(statusOutput.contains("entitySerialize="));
  EXPECT_TRUE(statusOutput.contains("storageTiming="));
  EXPECT_TRUE(statusOutput.contains("queueOnly="));
  EXPECT_TRUE(statusOutput.contains("sends=q:"));
  EXPECT_TRUE(statusOutput.contains("Phase 6 mutation gates:"));
  EXPECT_TRUE(statusOutput.contains("requested=none"));
}

TEST(ServerMeasurement, DISABLED_QueueOnlySendFanoutComparison) {
  auto eager = runQueueSendMeasurement(false);
  auto queueOnly = runQueueSendMeasurement(true);

  EXPECT_EQ(eager.clients, queueOnly.clients);
  EXPECT_EQ(eager.packetsSent, queueOnly.packetsSent);
  EXPECT_GE(eager.queuedPackets, eager.packetsSent);
  EXPECT_GE(eager.eagerPackets, eager.packetsSent);
  EXPECT_EQ(eager.workerPackets, 0u);
  EXPECT_GE(queueOnly.queuedPackets, queueOnly.packetsSent);
  EXPECT_EQ(queueOnly.eagerPackets, 0u);
  EXPECT_GE(queueOnly.workerPackets, queueOnly.packetsSent);

  RecordProperty("eagerElapsedUs", static_cast<int64_t>(eager.elapsedMicroseconds));
  RecordProperty("queueOnlyElapsedUs", static_cast<int64_t>(queueOnly.elapsedMicroseconds));
  RecordProperty("eagerIdleTimedWaits", static_cast<int64_t>(eager.idleTimedWaits));
  RecordProperty("queueOnlyIdleTimedWaits", static_cast<int64_t>(queueOnly.idleTimedWaits));
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

TEST(ServerTest, PendingHandshakeStateMachineAcceptsRemoteOpenStarboundAndLegacyClients) {
  ConfigurationValueGuard stateMachineGuard("universeServerConfigOverrides", JsonObject{{"usePendingConnectionStateMachine", true}});
  ConfigurationValueGuard anonymousGuard("allowAnonymousConnections", true);
  ConfigurationValueGuard anonymousAdminGuard("anonymousConnectionsAreAdmin", false);
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto openStarboundPlayer = makeTestPlayer("remote-openstarbound");
  auto openStarboundResult = connectRemoteClient(server, openStarboundPlayer, {}, {}, true);
  ASSERT_TRUE(as<ConnectSuccessPacket>(openStarboundResult.packet));

  auto legacyPlayer = makeTestPlayer("remote-legacy");
  auto legacyResult = connectRemoteClient(server, legacyPlayer, {}, {}, false);
  ASSERT_TRUE(as<ConnectSuccessPacket>(legacyResult.packet));

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.clients == 2 && status.pendingHandshakeFinalized == 2 && status.pendingHandshakes == 0;
  });
  EXPECT_EQ(status.clients, 2u);
  EXPECT_EQ(status.pendingHandshakeFinalized, 2u);
  EXPECT_EQ(status.pendingHandshakes, 0u);
  EXPECT_EQ(status.pendingHandshakeRejected, 0u);
  EXPECT_EQ(status.pendingHandshakeTimedOut, 0u);

  server.stop();
  server.join();
}

TEST(ServerTest, TcpPacketSocketTransfersCompressedProtocolRequest) {
  uint16_t const tcpPort = findAvailableTcpPort();
  Mutex packetMutex;
  PacketPtr receivedPacket;

  TcpServer tcpServer({HostAddress::localhost(), tcpPort});
  tcpServer.setAcceptCallback([&](TcpSocketPtr socket) {
    UniverseConnection serverConnection(TcpPacketSocket::open(std::move(socket)));
    serverConnection.receiveAny(10000);
    MutexLocker locker(packetMutex);
    receivedPacket = serverConnection.pullSingle();
  });

  auto socket = TcpSocket::connectTo({HostAddress::localhost(), tcpPort});
  UniverseConnection clientConnection(TcpPacketSocket::open(std::move(socket)));
  auto protocolRequest = make_shared<ProtocolRequestPacket>(StarProtocolVersion);
  protocolRequest->setCompressionMode(PacketCompressionMode::Enabled);
  clientConnection.pushSingle(protocolRequest);
  ASSERT_TRUE(clientConnection.sendAll(1000));

  ASSERT_TRUE(waitUntil([&]() {
    MutexLocker locker(packetMutex);
    return receivedPacket != nullptr;
  }, 10000));
  ASSERT_TRUE(as<ProtocolRequestPacket>(receivedPacket));
}

TEST(ServerTest, PendingHandshakeStateMachineAcceptsOpenStarboundAndLegacyTcpClients) {
  uint16_t const tcpPort = findAvailableTcpPort();
  ConfigurationValueGuard stateMachineGuard("universeServerConfigOverrides", JsonObject{{"usePendingConnectionStateMachine", true}});
  ConfigurationValueGuard anonymousGuard("allowAnonymousConnections", true);
  ConfigurationValueGuard anonymousAdminGuard("anonymousConnectionsAreAdmin", false);
  ConfigurationValueGuard bindGuard("gameServerBind", "127.0.0.1");
  ConfigurationValueGuard portGuard("gameServerPort", tcpPort);
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.setListeningTcp(true);
  server.start();

  auto connectTcp = [&]() {
    TcpSocketPtr socket;
    EXPECT_TRUE(waitUntil([&]() {
      try {
        socket = TcpSocket::connectTo({HostAddress::localhost(), tcpPort});
        return true;
      } catch (std::exception const&) {
        return false;
      }
    }, 10000));
    if (socket)
      socket->setNonBlocking(true);
    return socket;
  };

  auto openStarboundSocket = connectTcp();
  ASSERT_TRUE(openStarboundSocket);
  UniverseConnection openStarboundConnection(TcpPacketSocket::open(std::move(openStarboundSocket)));
  auto openStarboundPlayer = makeTestPlayer("tcp-openstarbound");
  ASSERT_TRUE(as<ConnectSuccessPacket>(completeClientHandshake(openStarboundConnection, openStarboundPlayer, {}, {}, true, true, {}, true, 10000)));

  auto legacySocket = connectTcp();
  ASSERT_TRUE(legacySocket);
  UniverseConnection legacyConnection(TcpPacketSocket::open(std::move(legacySocket)));
  auto legacyPlayer = makeTestPlayer("tcp-legacy");
  ASSERT_TRUE(as<ConnectSuccessPacket>(completeClientHandshake(legacyConnection, legacyPlayer, {}, {}, false, true, {}, true, 10000)));

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.clients == 2 && status.pendingHandshakeFinalized == 2 && status.pendingHandshakes == 0;
  });
  EXPECT_EQ(status.pendingHandshakeRejected, 0u);
  EXPECT_EQ(status.pendingHandshakeTimedOut, 0u);

  server.stop();
  server.join();
}

TEST(ServerTest, PendingHandshakeStateMachineAuthenticatesPasswordAccounts) {
  ConfigurationValueGuard stateMachineGuard("universeServerConfigOverrides", JsonObject{{"usePendingConnectionStateMachine", true}});
  ConfigurationValueGuard usersGuard("serverUsers", JsonObject{{"phaseuser", JsonObject{{"password", "secret"}, {"admin", false}}}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto player = makeTestPlayer("password-success");
  auto result = connectRemoteClient(server, player, "phaseuser", "secret");
  ASSERT_TRUE(as<ConnectSuccessPacket>(result.packet));

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.clients == 1 && status.pendingHandshakeFinalized == 1 && status.pendingHandshakes == 0;
  });
  EXPECT_EQ(status.pendingHandshakeRejected, 0u);

  server.stop();
  server.join();
}

TEST(ServerTest, PendingHandshakeStateMachineRejectsBadPassword) {
  ConfigurationValueGuard stateMachineGuard("universeServerConfigOverrides", JsonObject{{"usePendingConnectionStateMachine", true}});
  ConfigurationValueGuard usersGuard("serverUsers", JsonObject{{"phaseuser", JsonObject{{"password", "secret"}, {"admin", false}}}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto player = makeTestPlayer("password-failure");
  auto result = connectRemoteClient(server, player, "phaseuser", "wrong");
  ASSERT_TRUE(as<ConnectFailurePacket>(result.packet));

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.pendingHandshakeRejected == 1 && status.pendingHandshakes == 0;
  });
  EXPECT_EQ(status.clients, 0u);
  EXPECT_EQ(status.pendingHandshakeFinalized, 0u);

  server.stop();
  server.join();
}

TEST(ServerTest, PendingHandshakeStateMachineRejectsAssetMismatch) {
  ConfigurationValueGuard stateMachineGuard("universeServerConfigOverrides", JsonObject{{"usePendingConnectionStateMachine", true}});
  ConfigurationValueGuard anonymousGuard("allowAnonymousConnections", true);
  ConfigurationValueGuard mismatchGuard("allowAssetsMismatch", false);
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto player = makeTestPlayer("asset-mismatch");
  auto result = connectRemoteClient(server, player, {}, {}, true, true, String("bad-assets").utf8Bytes());
  ASSERT_TRUE(as<ConnectFailurePacket>(result.packet));

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.pendingHandshakeRejected == 1 && status.pendingHandshakes == 0;
  });
  EXPECT_EQ(status.clients, 0u);
  EXPECT_EQ(status.pendingHandshakeFinalized, 0u);

  server.stop();
  server.join();
}

TEST(ServerTest, PendingHandshakeStateMachineHandlesMaxPlayersAndAdminPriority) {
  ConfigurationValueGuard stateMachineGuard("universeServerConfigOverrides", JsonObject{{"usePendingConnectionStateMachine", true}});
  ConfigurationValueGuard maxPlayersGuard("maxPlayers", 1);
  ConfigurationValueGuard anonymousGuard("allowAnonymousConnections", true);
  ConfigurationValueGuard anonymousAdminGuard("anonymousConnectionsAreAdmin", false);
  ConfigurationValueGuard usersGuard("serverUsers", JsonObject{{"admin", JsonObject{{"password", "secret"}, {"admin", true}}}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto firstPlayer = makeTestPlayer("max-first");
  auto firstResult = connectRemoteClient(server, firstPlayer);
  ASSERT_TRUE(as<ConnectSuccessPacket>(firstResult.packet));

  auto rejectedPlayer = makeTestPlayer("max-rejected");
  auto rejectedResult = connectRemoteClient(server, rejectedPlayer);
  ASSERT_TRUE(as<ConnectFailurePacket>(rejectedResult.packet));

  auto adminPlayer = makeTestPlayer("max-admin");
  auto adminResult = connectRemoteClient(server, adminPlayer, "admin", "secret");
  ASSERT_TRUE(as<ConnectSuccessPacket>(adminResult.packet));

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.clients == 2 && status.pendingHandshakeFinalized == 2 && status.pendingHandshakeRejected == 1 && status.pendingHandshakes == 0;
  });
  EXPECT_EQ(status.pendingHandshakeTimedOut, 0u);

  server.stop();
  server.join();
}

TEST(ServerTest, PendingHandshakeStateMachineHandlesDuplicateUuidPriority) {
  ConfigurationValueGuard stateMachineGuard("universeServerConfigOverrides", JsonObject{{"usePendingConnectionStateMachine", true}});
  ConfigurationValueGuard anonymousGuard("allowAnonymousConnections", true);
  ConfigurationValueGuard anonymousAdminGuard("anonymousConnectionsAreAdmin", false);
  ConfigurationValueGuard usersGuard("serverUsers", JsonObject{{"admin", JsonObject{{"password", "secret"}, {"admin", true}}}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto player = makeTestPlayer("duplicate-uuid");
  auto firstResult = connectRemoteClient(server, player);
  ASSERT_TRUE(as<ConnectSuccessPacket>(firstResult.packet));

  auto rejectedDuplicate = connectRemoteClient(server, player);
  ASSERT_TRUE(as<ConnectFailurePacket>(rejectedDuplicate.packet));

  auto adminDuplicate = connectRemoteClient(server, player, "admin", "secret");
  ASSERT_TRUE(as<ConnectSuccessPacket>(adminDuplicate.packet));

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.clients == 1 && status.pendingHandshakeFinalized == 2 && status.pendingHandshakeRejected == 1 && status.pendingHandshakes == 0;
  });
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

TEST(ServerTest, PendingHandshakeStateMachineTimesOutProtocolRequest) {
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{
      {"usePendingConnectionStateMachine", true},
      {"clientWaitLimit", 100}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto connection = server.addLocalClient();
  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.pendingHandshakeTimedOut == 1 && status.pendingHandshakes == 0;
  });

  EXPECT_EQ(status.pendingHandshakeAccepted, 1u);
  EXPECT_EQ(status.pendingHandshakeRejected, 0u);
  EXPECT_EQ(status.pendingHandshakeFinalized, 0u);

  server.stop();
  server.join();
}

TEST(ServerTest, PendingHandshakeStateMachineTimesOutClientConnect) {
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{
      {"usePendingConnectionStateMachine", true},
      {"clientWaitLimit", 100}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

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

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.pendingHandshakeTimedOut == 1 && status.pendingHandshakeRejected == 1 && status.pendingHandshakes == 0;
  });

  EXPECT_EQ(status.pendingHandshakeAccepted, 1u);
  EXPECT_EQ(status.pendingHandshakeFinalized, 0u);

  server.stop();
  server.join();
}

TEST(ServerTest, PendingHandshakeStateMachineTimesOutPasswordResponse) {
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{
      {"usePendingConnectionStateMachine", true},
      {"clientWaitLimit", 100}});
  ConfigurationValueGuard usersGuard("serverUsers", JsonObject{{"phaseuser", JsonObject{{"password", "secret"}, {"admin", false}}}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  auto player = makeTestPlayer("password-timeout");
  auto result = connectRemoteClient(server, player, "phaseuser", {}, true, true, {}, false);
  ASSERT_TRUE(as<HandshakeChallengePacket>(result.packet));

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.pendingHandshakeTimedOut == 1 && status.pendingHandshakeRejected == 1 && status.pendingHandshakes == 0;
  });
  EXPECT_EQ(status.pendingHandshakeAccepted, 1u);
  EXPECT_EQ(status.pendingHandshakeFinalized, 0u);

  server.stop();
  server.join();
}

TEST(ServerTest, PendingHandshakeStateMachineTimesOutLoginBursts) {
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{
      {"usePendingConnectionStateMachine", true},
      {"clientWaitLimit", 100}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  List<UniverseConnection> connections;
  for (size_t i = 0; i < 4; ++i)
    connections.append(server.addRemoteLocalClient(HostAddress::localhost()));

  auto status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.pendingHandshakeTimedOut == 4 && status.pendingHandshakes == 0;
  });
  EXPECT_EQ(status.pendingHandshakeAccepted, 4u);
  EXPECT_EQ(status.pendingHandshakeRejected, 0u);
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

TEST(ServerTest, AsyncPersistenceReloadsWrittenSettings) {
  ConfigurationValueGuard clearGuard("clearUniverseFiles", false);
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{
      {"useAsyncPersistence", true},
      {"persistenceWorkerThreads", 1},
      {"maxQueuedPersistenceSnapshots", 128},
      {"maxPersistenceWriteRetries", 0}});
  TemporaryUniverseStorage storage;
  Uuid writtenUuid;
  {
    UniverseServer server(storage.universe);
    writtenUuid = server.universeSettings()->uuid();
    server.start();

    waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
      return status.persistenceBatchesCompleted > 0 && status.persistenceSnapshotsWritten >= 2;
    });

    server.stop();
    server.join();
  }

  UniverseServer reloadedServer(storage.universe);
  EXPECT_EQ(reloadedServer.universeSettings()->uuid(), writtenUuid);
  EXPECT_TRUE(loadVersionedJsonFile(File::relativeTo(storage.universe, "universe.dat"), "UniverseSettings").isType(Json::Type::Object));
  EXPECT_TRUE(loadVersionedJsonFile(File::relativeTo(storage.universe, "tempworlds.index"), "TempWorldIndex").isType(Json::Type::Object));
}

TEST(ServerTest, AsyncPersistenceDrainsQueuedWritesOnShutdown) {
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{
      {"useAsyncPersistence", true},
      {"persistenceWorkerThreads", 1},
      {"maxQueuedPersistenceSnapshots", 128},
      {"maxPersistenceWriteRetries", 0}});
  TemporaryUniverseStorage storage;
  UniverseServer server(storage.universe);
  server.start();

  waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
    return status.persistenceSnapshotsPending > 0 || status.persistenceSnapshotsWritten >= 2;
  });

  server.stop();
  server.join();

  auto status = server.serverStatus();
  EXPECT_EQ(status.persistenceFailures, 0u);
  EXPECT_EQ(status.persistenceBatchesPending, 0u);
  EXPECT_EQ(status.persistenceSnapshotsPending, 0u);
  EXPECT_GE(status.persistenceSnapshotsWritten, 2u);

  auto universeSettingsFile = File::relativeTo(storage.universe, "universe.dat");
  auto tempWorldIndexFile = File::relativeTo(storage.universe, "tempworlds.index");
  ASSERT_TRUE(File::isFile(universeSettingsFile));
  ASSERT_TRUE(File::isFile(tempWorldIndexFile));
  EXPECT_TRUE(loadVersionedJsonFile(universeSettingsFile, "UniverseSettings").isType(Json::Type::Object));
  EXPECT_TRUE(loadVersionedJsonFile(tempWorldIndexFile, "TempWorldIndex").isType(Json::Type::Object));
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

TEST(ServerTest, AsyncPersistenceRecordsWriteFailuresAndRetries) {
  ConfigurationValueGuard clearGuard("clearUniverseFiles", false);
  ConfigurationValueGuard configGuard("universeServerConfigOverrides", JsonObject{
      {"useAsyncPersistence", true},
      {"persistenceWorkerThreads", 1},
      {"maxQueuedPersistenceSnapshots", 128},
      {"maxPersistenceWriteRetries", 1}});
  TemporaryUniverseStorage storage;
  File::makeDirectory(storage.universe);
  File::makeDirectory(File::relativeTo(storage.universe, "universe.dat"));
  File::makeDirectory(File::relativeTo(storage.universe, "universe.dat.new"));

  UniverseServer server(storage.universe);
  server.start();

  UniverseServer::ServerStatus status;
  EXPECT_NONFATAL_FAILURE_ON_ALL_THREADS({
    status = waitForServerStatus(server, [](UniverseServer::ServerStatus const& status) {
      return status.persistenceFailures > 0;
    }, 10000);
  }, "Error was logged");

  EXPECT_TRUE(status.persistenceAsyncEnabled);
  EXPECT_GT(status.persistenceWriteRetries, 0u);
  EXPECT_GT(status.persistenceSnapshotsWritten, 0u);
  EXPECT_GT(status.persistenceBatchesCompleted, 0u);

  File::removeDirectoryRecursive(File::relativeTo(storage.universe, "universe.dat"));
  File::removeDirectoryRecursive(File::relativeTo(storage.universe, "universe.dat.new"));

  server.stop();
  server.join();
}
