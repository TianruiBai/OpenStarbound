#include "StarUniverseConnection.hpp"
#include "StarTcp.hpp"
#include "StarTime.hpp"

#include "gtest/gtest.h"

using namespace Star;

unsigned const PacketCount = 20;
uint16_t const ServerPort = 55555;

unsigned const NumLocalASyncConnections = 5;
unsigned const NumRemoteASyncConnections = 5;
unsigned const ASyncSleepMillis = 5;

unsigned const NumLocalSyncConnections = 5;
unsigned const NumRemoteSyncConnections = 5;
unsigned const SyncWaitMillis = 10000;

template <typename Predicate>
bool waitUntil(Predicate predicate, unsigned timeoutMillis = 2000) {
  auto timer = Timer::withMilliseconds(timeoutMillis);
  while (!timer.timeUp()) {
    if (predicate())
      return true;
    Thread::sleep(1);
  }
  return predicate();
}

size_t totalOwnedConnections(List<UniverseConnectionServer::NetworkWorkerStats> const& stats) {
  size_t total = 0;
  for (auto const& workerStats : stats)
    total += workerStats.ownedConnections;
  return total;
}

class ASyncClientThread : public Thread {
public:
  ASyncClientThread(UniverseConnection conn)
    : Thread("UniverseConnectionTestClientThread"), m_connection(std::move(conn)) {
    start();
  }

  virtual void run() {
    try {
      unsigned read = 0;
      unsigned written = 0;
      while (read < PacketCount || written < PacketCount) {
        m_connection.receive();
        if (read < PacketCount) {
          if (auto packet = m_connection.pullSingle()) {
            EXPECT_TRUE(convert<ProtocolRequestPacket>(packet)->requestProtocolVersion == read);
            ++read;
          }
        }

        if (written < PacketCount) {
          m_connection.push({make_shared<ProtocolRequestPacket>(written)});
          ++written;
        }
        m_connection.send();

        Thread::sleep(ASyncSleepMillis);

        if (!m_connection.isOpen())
          break;
      }

      EXPECT_EQ(PacketCount, read);
      EXPECT_EQ(PacketCount, written);
      m_connection.close();
      EXPECT_TRUE(m_connection.pull().empty());
    } catch (std::exception const& e) {
      ADD_FAILURE() << "Exception: " << outputException(e, true);
    } catch (...) {
      ADD_FAILURE();
    }
  }

private:
  UniverseConnection m_connection;
};

class SyncClientThread : public Thread {
public:
  SyncClientThread(UniverseConnection conn)
    : Thread("UniverseConnectionTestClientThread"), m_connection(std::move(conn)) {
    start();
  }

  virtual void run() {
    try {
      for (unsigned i = 0; i < PacketCount; ++i) {
        m_connection.pushSingle(make_shared<ProtocolRequestPacket>(i));
        EXPECT_TRUE(m_connection.sendAll(SyncWaitMillis));
        EXPECT_TRUE(m_connection.receiveAny(SyncWaitMillis));
        EXPECT_EQ(convert<ProtocolRequestPacket>(m_connection.pullSingle())->requestProtocolVersion, i);

        if (!m_connection.isOpen())
          break;
      }

      m_connection.close();
      EXPECT_TRUE(m_connection.pull().empty());
    } catch (std::exception const& e) {
      ADD_FAILURE() << "Exception: " << outputException(e, true);
    } catch (...) {
      ADD_FAILURE();
    }
  }

private:
  UniverseConnection m_connection;
};

TEST(UniverseConnections, All) {
  UniverseConnectionServer server([](UniverseConnectionServer* server, ConnectionId clientId, List<PacketPtr> packets) {
      server->sendPackets(clientId, packets);
    });

  ConnectionId clientId = ServerConnectionId;
  TcpServer tcpServer(HostAddressWithPort(HostAddress::localhost(), ServerPort));
  tcpServer.setAcceptCallback([&server, &clientId](TcpSocketPtr socket) {
      socket->setNonBlocking(true);
      auto conn = UniverseConnection(TcpPacketSocket::open(std::move(socket)));
      server.addConnection(++clientId, std::move(conn));
    });

  LinkedList<ASyncClientThread> localASyncClients;
  for (unsigned i = 0; i < NumLocalASyncConnections; ++i) {
    auto pair = LocalPacketSocket::openPair();
    server.addConnection(++clientId, UniverseConnection(std::move(pair.first)));
    localASyncClients.emplaceAppend(UniverseConnection(std::move(pair.second)));
  }

  LinkedList<SyncClientThread> localSyncClients;
  for (unsigned i = 0; i < NumLocalSyncConnections; ++i) {
    auto pair = LocalPacketSocket::openPair();
    server.addConnection(++clientId, UniverseConnection(std::move(pair.first)));
    localSyncClients.emplaceAppend(UniverseConnection(std::move(pair.second)));
  }

  LinkedList<ASyncClientThread> remoteASyncClients;
  for (unsigned i = 0; i < NumRemoteASyncConnections; ++i) {
    auto socket = TcpSocket::connectTo({HostAddress::localhost(), ServerPort});
    socket->setNonBlocking(true);
    remoteASyncClients.emplaceAppend(UniverseConnection(TcpPacketSocket::open(std::move(socket))));
  }

  LinkedList<SyncClientThread> remoteSyncClients;
  for (unsigned i = 0; i < NumRemoteSyncConnections; ++i) {
    auto socket = TcpSocket::connectTo({HostAddress::localhost(), ServerPort});
    socket->setNonBlocking(true);
    remoteSyncClients.emplaceAppend(UniverseConnection(TcpPacketSocket::open(std::move(socket))));
  }

  for (auto& c : localASyncClients)
    c.join();

  for (auto& c : remoteASyncClients)
    c.join();

  for (auto& c : localSyncClients)
    c.join();

  for (auto& c : remoteSyncClients)
    c.join();

  server.removeAllConnections();
}

TEST(UniverseConnectionServer, WorkerOwnershipStats) {
  UniverseConnectionServer server([](UniverseConnectionServer*, ConnectionId, List<PacketPtr>) {}, 2);

  List<UniverseConnection> clients;
  for (ConnectionId clientId = 1; clientId <= 4; ++clientId) {
    auto pair = LocalPacketSocket::openPair();
    server.addConnection(clientId, UniverseConnection(std::move(pair.first)));
    clients.append(UniverseConnection(std::move(pair.second)));
  }

  auto stats = server.workerStats();
  ASSERT_EQ(2, stats.size());
  EXPECT_EQ(4, totalOwnedConnections(stats));
  EXPECT_EQ(2, stats[0].ownedConnections);
  EXPECT_EQ(2, stats[1].ownedConnections);

  auto removed = server.removeConnection(2);
  EXPECT_FALSE(server.hasConnection(2));

  stats = server.workerStats();
  EXPECT_EQ(3, totalOwnedConnections(stats));
  EXPECT_EQ(1, stats[0].ownedConnections);
  EXPECT_EQ(2, stats[1].ownedConnections);
}

TEST(UniverseConnectionServer, SendPacketsWakesIdleWorker) {
  UniverseConnectionServer server([](UniverseConnectionServer*, ConnectionId, List<PacketPtr>) {}, 1);

  auto pair = LocalPacketSocket::openPair();
  server.addConnection(1, UniverseConnection(std::move(pair.first)));
  UniverseConnection client(std::move(pair.second));

  auto beforeWakeups = server.workerStats()[0].wakeups;
  server.sendPackets(1, {make_shared<ProtocolRequestPacket>(42)});

  shared_ptr<ProtocolRequestPacket> received;
  ASSERT_TRUE(waitUntil([&]() {
    client.receive();
    if (auto packet = client.pullSingle())
      received = as<ProtocolRequestPacket>(packet);
    return (bool)received;
  }));

  EXPECT_EQ(42, received->requestProtocolVersion);
  EXPECT_GT(server.workerStats()[0].wakeups, beforeWakeups);
}

TEST(UniverseConnectionServer, RemoveConnectionDuringCallback) {
  atomic<bool> callbackRan(false);
  UniverseConnectionServer server([&callbackRan](UniverseConnectionServer* server, ConnectionId clientId, List<PacketPtr>) {
      auto removed = server->removeConnection(clientId);
      callbackRan = true;
    }, 1);

  auto pair = LocalPacketSocket::openPair();
  server.addConnection(1, UniverseConnection(std::move(pair.first)));
  UniverseConnection client(std::move(pair.second));

  client.pushSingle(make_shared<ProtocolRequestPacket>(7));
  client.send();

  ASSERT_TRUE(waitUntil([&callbackRan]() { return callbackRan.load(); }));
  EXPECT_FALSE(server.hasConnection(1));
  EXPECT_EQ(0, totalOwnedConnections(server.workerStats()));
}
