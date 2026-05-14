#include "StarSocketPoller.hpp"
#include "StarException.hpp"
#include "StarTcp.hpp"
#include "StarTime.hpp"

#include "gtest/gtest.h"

using namespace Star;

namespace {

uint16_t findAvailableSocketPollerPort() {
  uint16_t const firstPort = 57000 + static_cast<uint16_t>(Time::monotonicMilliseconds() % 3000);
  for (uint16_t i = 0; i < 3000; ++i) {
    uint16_t port = 57000 + static_cast<uint16_t>((firstPort + i - 57000) % 3000);
    try {
      TcpServer server({HostAddress::localhost(), port});
      server.stop();
      return port;
    } catch (std::exception const&) {}
  }

  throw StarException("Could not find an available TCP port for socket poller test");
}

pair<TcpSocketPtr, TcpSocketPtr> openTcpSocketPair() {
  auto port = findAvailableSocketPollerPort();
  TcpServer server({HostAddress::localhost(), port});
  auto client = TcpSocket::connectTo({HostAddress::localhost(), port});
  auto serverSocket = server.accept(1000);

  if (client)
    client->setNonBlocking(true);
  if (serverSocket)
    serverSocket->setNonBlocking(true);

  return {client, serverSocket};
}

}

TEST(SocketPollerTest, WakeInterruptsEmptyPoll) {
  SocketPoller poller;

  auto wakeThread = Thread::invoke("SocketPollerTest::wake", [&poller]() {
      Thread::sleep(20);
      poller.wake();
    });

  auto start = Time::monotonicMilliseconds();
  auto ready = poller.poll(10000);
  auto elapsed = Time::monotonicMilliseconds() - start;
  wakeThread.finish();

  EXPECT_TRUE(ready.empty());
  EXPECT_LT(elapsed, 1000);
}

TEST(SocketPollerTest, ReportsReadableAndWritableSockets) {
  auto socketPair = openTcpSocketPair();
  auto client = socketPair.first;
  auto server = socketPair.second;
  ASSERT_TRUE(client);
  ASSERT_TRUE(server);

  SocketPoller poller;
  poller.registerSocket(server, {true, false});
  EXPECT_EQ(poller.registeredSockets(), 1u);

  char byte = 'x';
  ASSERT_EQ(client->send(&byte, 1), 1u);

  auto ready = poller.poll(1000);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].socket, server);
  EXPECT_TRUE(ready[0].readable);
  EXPECT_FALSE(ready[0].writable);
  EXPECT_FALSE(ready[0].exception);

  char receivedByte;
  ASSERT_EQ(server->receive(&receivedByte, 1), 1u);

  poller.updateSocket(server, {false, true});
  ready = poller.poll(1000);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].socket, server);
  EXPECT_FALSE(ready[0].readable);
  EXPECT_TRUE(ready[0].writable);
  EXPECT_FALSE(ready[0].exception);

  poller.unregisterSocket(server);
  EXPECT_EQ(poller.registeredSockets(), 0u);

  poller.registerSocket(client, {false, true});
  ready = poller.poll(1000);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].socket, client);
  EXPECT_FALSE(ready[0].readable);
  EXPECT_TRUE(ready[0].writable);
  EXPECT_FALSE(ready[0].exception);
}
