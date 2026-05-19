#include "StarUniverseConnection.hpp"
#include "StarFormat.hpp"
#include "StarLogging.hpp"
#include <thread>

namespace Star {

static const int PacketSocketPollSleep = 1;

UniverseConnection::UniverseConnection(PacketSocketUPtr packetSocket)
    : m_packetSocket(std::move(packetSocket)) {}

UniverseConnection::UniverseConnection(UniverseConnection&& rhs) {
  operator=(std::move(rhs));
}

UniverseConnection::~UniverseConnection() {
  if (m_packetSocket)
    m_packetSocket->close();
}

UniverseConnection& UniverseConnection::operator=(UniverseConnection&& rhs) {
  MutexLocker locker(m_mutex);
  m_sendQueue = take(rhs.m_sendQueue);
  m_receiveQueue = take(rhs.m_receiveQueue);
  m_packetSocket = take(rhs.m_packetSocket);
  return *this;
}

bool UniverseConnection::isOpen() const {
  MutexLocker locker(m_mutex);
  return m_packetSocket->isOpen();
}

void UniverseConnection::close() {
  MutexLocker locker(m_mutex);
  m_packetSocket->close();
}

void UniverseConnection::push(List<PacketPtr> packets) {
  MutexLocker locker(m_mutex);
  m_sendQueue.appendAll(std::move(packets));
}

void UniverseConnection::pushSingle(PacketPtr packet) {
  MutexLocker locker(m_mutex);
  m_sendQueue.append(std::move(packet));
}

List<PacketPtr> UniverseConnection::pull() {
  MutexLocker locker(m_mutex);
  return List<PacketPtr>::from(take(m_receiveQueue));
}

PacketPtr UniverseConnection::pullSingle() {
  MutexLocker locker(m_mutex);
  if (m_receiveQueue.empty())
    return {};
  return m_receiveQueue.takeFirst();
}

bool UniverseConnection::send() {
  MutexLocker locker(m_mutex);
  m_packetSocket->sendPackets(take(m_sendQueue));
  return m_packetSocket->writeData();
}

bool UniverseConnection::sendAll(unsigned timeout) {
  MutexLocker locker(m_mutex);

  m_packetSocket->sendPackets(take(m_sendQueue));

  auto timer = Timer::withMilliseconds(timeout);
  while (true) {
    m_packetSocket->writeData();
    if (!m_packetSocket->sentPacketsPending())
      return true;

    if (timer.timeUp() || !m_packetSocket->isOpen())
      return false;

    Thread::sleep(PacketSocketPollSleep);
  }
}

bool UniverseConnection::receive() {
  MutexLocker locker(m_mutex);
  bool received = m_packetSocket->readData();
  m_receiveQueue.appendAll(m_packetSocket->receivePackets());
  return received;
}

bool UniverseConnection::receiveAny(unsigned timeout) {
  MutexLocker locker(m_mutex);
  if (!m_receiveQueue.empty())
    return true;

  auto timer = Timer::withMilliseconds(timeout);
  while (true) {
    m_packetSocket->readData();
    m_receiveQueue.appendAll(m_packetSocket->receivePackets());
    if (!m_receiveQueue.empty())
      return true;

    if (timer.timeUp() || !m_packetSocket->isOpen())
      return false;

    Thread::sleep(PacketSocketPollSleep);
  }
}

PacketSocket& UniverseConnection::packetSocket() {
  return *m_packetSocket;
}

Maybe<PacketStats> UniverseConnection::incomingStats() const {
  MutexLocker locker(m_mutex);
  return m_packetSocket->incomingStats();
}

Maybe<PacketStats> UniverseConnection::outgoingStats() const {
  MutexLocker locker(m_mutex);
  return m_packetSocket->outgoingStats();
}

UniverseConnectionServer::UniverseConnectionServer(PacketReceiveCallback packetReceiver, size_t numWorkerThreads, bool queueOnlySends)
    : m_packetReceiver(std::move(packetReceiver)), m_shutdown(false), m_queueOnlySends(queueOnlySends) {
#ifdef STAR_PLATFORM_N3DS
  if (numWorkerThreads == 0) {
    m_numWorkerThreads = 0;
    Logger::info("N3DS UniverseConnectionServer: using threadless polling");
    return;
  }
#endif

  if (numWorkerThreads == 0)
    m_numWorkerThreads = max<size_t>(2, std::thread::hardware_concurrency() / 4);
  else
    m_numWorkerThreads = numWorkerThreads;

  Logger::info("UniverseConnectionServer: Starting {} network worker threads", m_numWorkerThreads);

  m_workerStats.resize(m_numWorkerThreads);
  for (size_t i = 0; i < m_numWorkerThreads; ++i)
    m_workerStates.append(make_shared<WorkerState>());

  for (size_t i = 0; i < m_numWorkerThreads; ++i) {
    m_processingThreads.append(Thread::invoke(strf("UniverseConnectionServer::worker_{}", i), [this, i]() {
      auto workerState = m_workerStates[i];
      try {
        while (!m_shutdown) {
          MutexLocker workerLocker(workerState->mutex);
          auto connectionIds = workerState->connections;
          workerState->wakeup = false;
          workerLocker.unlock();

          bool dataTransmitted = false;
          size_t handledCount = 0;
          m_workerStats[i].connectionScans += connectionIds.size();
          for (auto clientId : connectionIds) {
            RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
            auto connection = m_connections.value(clientId);
            connectionsLocker.unlock();

            if (!connection || connection->workerIndex != i) {
              m_workerStats[i].staleConnectionScans++;
              continue;
            }

            handledCount++;
            MutexLocker connectionLocker(connection->mutex);
            if (!connection->packetSocket || !connection->packetSocket->isOpen())
              continue;

            auto queuedSendPackets = connection->sendQueue.size();
            if (queuedSendPackets != 0) {
              m_workerStats[i].workerSendBatches++;
              m_workerStats[i].workerSendPackets += queuedSendPackets;
              connection->packetSocket->sendPackets(take(connection->sendQueue));
            }

            bool hadPendingWrites = queuedSendPackets != 0 || connection->packetSocket->sentPacketsPending();
            int64_t writeStart = hadPendingWrites ? Time::monotonicMicroseconds() : 0;
            dataTransmitted |= connection->packetSocket->writeData();
            if (hadPendingWrites)
              m_workerStats[i].workerWriteTimeMicroseconds += Time::monotonicMicroseconds() - writeStart;

            dataTransmitted |= connection->packetSocket->readData();
            List<PacketPtr> receivePackets = connection->packetSocket->receivePackets();
            if (!receivePackets.empty()) {
              connection->lastActivityTime = Time::monotonicMilliseconds();
              m_workerStats[i].packetsProcessed += receivePackets.size();
              connection->receiveQueue.appendAll(take(receivePackets));
            }

            if (!connection->receiveQueue.empty()) {
              List<PacketPtr> toReceive = List<PacketPtr>::from(take(connection->receiveQueue));
              connectionLocker.unlock();

              auto callbackStart = Time::monotonicMicroseconds();
              try {
                m_packetReceiver(this, clientId, std::move(toReceive));
              } catch (std::exception const& e) {
                Logger::error("Exception caught handling incoming server packets, disconnecting client '{}' {}", clientId, outputException(e, true));

                connectionLocker.lock();
                connection->packetSocket->close();
              }
              m_workerStats[i].callbackGroupsProcessed++;
              m_workerStats[i].callbackTimeMicroseconds += Time::monotonicMicroseconds() - callbackStart;
            }
          }
          m_workerStats[i].connectionsHandled = handledCount;

          if (!dataTransmitted) {
            workerLocker.lock();
            if (!workerState->wakeup && !m_shutdown) {
              m_workerStats[i].timedWaits++;
              workerState->condition.wait(workerState->mutex, PacketSocketPollSleep);
              if (!workerState->wakeup)
                m_workerStats[i].idleTimedWaits++;
            }
          }
        }
      } catch (std::exception const& e) {
        Logger::error("Exception caught in UniverseConnectionServer::worker_{}, closing assigned connections: {}", i, e.what());
        MutexLocker workerLocker(workerState->mutex);
        auto connectionIds = workerState->connections;
        workerLocker.unlock();

        for (auto clientId : connectionIds) {
          RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
          auto connection = m_connections.value(clientId);
          connectionsLocker.unlock();

          if (connection && connection->workerIndex == i) {
            MutexLocker connectionLocker(connection->mutex);
            if (connection->packetSocket)
              connection->packetSocket->close();
          }
        }
      }
    }));
  }
}

UniverseConnectionServer::~UniverseConnectionServer() {
  m_shutdown = true;
  for (auto& workerState : m_workerStates) {
    MutexLocker workerLocker(workerState->mutex);
    workerState->wakeup = true;
    workerState->condition.broadcast();
  }
  for (auto& thread : m_processingThreads)
    thread.finish();
  removeAllConnections();
}

void UniverseConnectionServer::wakeWorker(size_t workerIndex) {
  if (workerIndex >= m_workerStates.size())
    return;

  auto workerState = m_workerStates[workerIndex];
  MutexLocker workerLocker(workerState->mutex);
  m_workerStats[workerIndex].wakeups++;
  workerState->wakeup = true;
  workerState->condition.signal();
}

bool UniverseConnectionServer::hasConnection(ConnectionId clientId) const {
  RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
  return m_connections.contains(clientId);
}

List<ConnectionId> UniverseConnectionServer::allConnections() const {
  RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
  return m_connections.keys();
}

bool UniverseConnectionServer::connectionIsOpen(ConnectionId clientId) const {
  RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
  if (auto conn = m_connections.value(clientId)) {
    connectionsLocker.unlock();
    MutexLocker connectionLocker(conn->mutex);
    return conn->packetSocket->isOpen();
  }

  throw UniverseConnectionException::format("No such client '{}' in UniverseConnectionServer::connectionIsOpen", clientId);
}

int64_t UniverseConnectionServer::lastActivityTime(ConnectionId clientId) const {
  RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
  if (auto conn = m_connections.value(clientId)) {
    connectionsLocker.unlock();
    MutexLocker connectionLocker(conn->mutex);
    return conn->lastActivityTime;
  }
  throw UniverseConnectionException::format("No such client '{}' in UniverseConnectionServer::lastRemoteActivityTime", clientId);
}

void UniverseConnectionServer::addConnection(ConnectionId clientId, UniverseConnection uc) {
  RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
  if (m_connections.contains(clientId))
    throw UniverseConnectionException::format("Client '{}' already exists in UniverseConnectionServer::addConnection", clientId);

  auto connection = make_shared<Connection>();
  connection->packetSocket = std::move(uc.m_packetSocket);
  connection->sendQueue = std::move(uc.m_sendQueue);
  connection->receiveQueue = std::move(uc.m_receiveQueue);
  connection->lastActivityTime = Time::monotonicMilliseconds();
#ifdef STAR_PLATFORM_N3DS
  if (m_numWorkerThreads == 0) {
    connection->workerIndex = 0;
    m_connections.add(clientId, std::move(connection));
    return;
  }
#endif
  connection->workerIndex = clientId % m_numWorkerThreads;
  auto workerIndex = connection->workerIndex;
  m_connections.add(clientId, std::move(connection));
  connectionsLocker.unlock();

  auto workerState = m_workerStates[workerIndex];
  MutexLocker workerLocker(workerState->mutex);
  workerState->connections.append(clientId);
  m_workerStats[workerIndex].wakeups++;
  workerState->wakeup = true;
  workerState->condition.signal();
}

UniverseConnection UniverseConnectionServer::removeConnection(ConnectionId clientId) {
  RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
  if (!m_connections.contains(clientId))
    throw UniverseConnectionException::format("Client '{}' does not exist in UniverseConnectionServer::removeConnection", clientId);

  auto conn = m_connections.take(clientId);
  connectionsLocker.unlock();
#ifdef STAR_PLATFORM_N3DS
  if (m_numWorkerThreads == 0) {
    MutexLocker connectionLocker(conn->mutex);

    UniverseConnection uc;
    uc.m_packetSocket = take(conn->packetSocket);
    uc.m_sendQueue = std::move(conn->sendQueue);
    uc.m_receiveQueue = std::move(conn->receiveQueue);
    return uc;
  }
#endif
  auto workerState = m_workerStates[conn->workerIndex];
  MutexLocker workerLocker(workerState->mutex);
  workerState->connections.remove(clientId);
  m_workerStats[conn->workerIndex].wakeups++;
  workerState->wakeup = true;
  workerState->condition.signal();
  workerLocker.unlock();

  MutexLocker connectionLocker(conn->mutex);

  UniverseConnection uc;
  uc.m_packetSocket = take(conn->packetSocket);
  uc.m_sendQueue = std::move(conn->sendQueue);
  uc.m_receiveQueue = std::move(conn->receiveQueue);
  return uc;
}

void UniverseConnectionServer::update() {
#ifdef STAR_PLATFORM_N3DS
  if (m_numWorkerThreads != 0)
    return;

  List<ConnectionId> connectionIds;
  {
    RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
    connectionIds = m_connections.keys();
  }

  for (auto clientId : connectionIds) {
    shared_ptr<Connection> connection;
    {
      RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
      connection = m_connections.value(clientId);
    }

    if (!connection)
      continue;

    MutexLocker connectionLocker(connection->mutex);
    if (!connection->packetSocket || !connection->packetSocket->isOpen())
      continue;

    if (!connection->sendQueue.empty())
      connection->packetSocket->sendPackets(take(connection->sendQueue));

    connection->packetSocket->writeData();
    connection->packetSocket->readData();
    auto receivePackets = connection->packetSocket->receivePackets();
    if (!receivePackets.empty()) {
      connection->lastActivityTime = Time::monotonicMilliseconds();
      connection->receiveQueue.appendAll(take(receivePackets));
    }

    if (!connection->receiveQueue.empty()) {
      List<PacketPtr> toReceive = List<PacketPtr>::from(take(connection->receiveQueue));
      connectionLocker.unlock();
      m_packetReceiver(this, clientId, std::move(toReceive));
    }
  }
#endif
}

List<UniverseConnection> UniverseConnectionServer::removeAllConnections() {
  List<UniverseConnection> removedConnections;
  RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
  for (auto connectionId : m_connections.keys())
    removedConnections.append(removeConnection(connectionId));
  return removedConnections;
}

void UniverseConnectionServer::sendPackets(ConnectionId clientId, List<PacketPtr> packets) {
  RecursiveMutexLocker connectionsLocker(m_connectionsMutex);
  if (auto conn = m_connections.value(clientId)) {
    connectionsLocker.unlock();
    MutexLocker connectionLocker(conn->mutex);
#ifdef STAR_PLATFORM_N3DS
    if (m_numWorkerThreads == 0) {
      conn->sendQueue.appendAll(std::move(packets));
      if (conn->packetSocket && conn->packetSocket->isOpen()) {
        conn->packetSocket->sendPackets(take(conn->sendQueue));
        conn->packetSocket->writeData();
      }
      return;
    }
#endif
    auto workerIndex = conn->workerIndex;
    auto packetCount = packets.size();
    conn->sendQueue.appendAll(std::move(packets));
    if (packetCount != 0) {
      m_workerStats[workerIndex].queuedSendBatches++;
      m_workerStats[workerIndex].queuedSendPackets += packetCount;
    }

    if (!m_queueOnlySends && conn->packetSocket->isOpen()) {
      auto eagerSendPackets = conn->sendQueue.size();
      if (eagerSendPackets != 0) {
        m_workerStats[workerIndex].eagerSendBatches++;
        m_workerStats[workerIndex].eagerSendPackets += eagerSendPackets;
      }
      bool hadPendingWrites = eagerSendPackets != 0 || conn->packetSocket->sentPacketsPending();
      conn->packetSocket->sendPackets(take(conn->sendQueue));
      int64_t writeStart = hadPendingWrites ? Time::monotonicMicroseconds() : 0;
      conn->packetSocket->writeData();
      if (hadPendingWrites)
        m_workerStats[workerIndex].eagerWriteTimeMicroseconds += Time::monotonicMicroseconds() - writeStart;
    }
    connectionLocker.unlock();
    wakeWorker(workerIndex);
  } else {
    throw UniverseConnectionException::format("No such client '{}' in UniverseConnectionServer::sendPackets", clientId);
  }
}

uint64_t UniverseConnectionServer::totalPacketsProcessed() const {
  uint64_t total = 0;
  for (auto const& stats : m_workerStats)
    total += stats.packetsProcessed.load();
  return total;
}

size_t UniverseConnectionServer::numWorkerThreads() const {
  return m_numWorkerThreads;
}

bool UniverseConnectionServer::queueOnlySends() const {
  return m_queueOnlySends;
}

List<UniverseConnectionServer::NetworkWorkerStats> UniverseConnectionServer::workerStats() const {
  List<NetworkWorkerStats> stats;
  for (size_t i = 0; i < m_workerStats.size(); ++i) {
    NetworkWorkerStats workerStats;
    workerStats.lastHandledConnections = m_workerStats[i].connectionsHandled.load();
    workerStats.connectionScans = m_workerStats[i].connectionScans.load();
    workerStats.staleConnectionScans = m_workerStats[i].staleConnectionScans.load();
    workerStats.packetsProcessed = m_workerStats[i].packetsProcessed.load();
    workerStats.callbackGroupsProcessed = m_workerStats[i].callbackGroupsProcessed.load();
    workerStats.callbackTimeMicroseconds = m_workerStats[i].callbackTimeMicroseconds.load();
    workerStats.queuedSendBatches = m_workerStats[i].queuedSendBatches.load();
    workerStats.queuedSendPackets = m_workerStats[i].queuedSendPackets.load();
    workerStats.eagerSendBatches = m_workerStats[i].eagerSendBatches.load();
    workerStats.eagerSendPackets = m_workerStats[i].eagerSendPackets.load();
    workerStats.eagerWriteTimeMicroseconds = m_workerStats[i].eagerWriteTimeMicroseconds.load();
    workerStats.workerSendBatches = m_workerStats[i].workerSendBatches.load();
    workerStats.workerSendPackets = m_workerStats[i].workerSendPackets.load();
    workerStats.workerWriteTimeMicroseconds = m_workerStats[i].workerWriteTimeMicroseconds.load();
    workerStats.wakeups = m_workerStats[i].wakeups.load();
    workerStats.timedWaits = m_workerStats[i].timedWaits.load();
    workerStats.idleTimedWaits = m_workerStats[i].idleTimedWaits.load();

    MutexLocker workerLocker(m_workerStates[i]->mutex);
    workerStats.ownedConnections = m_workerStates[i]->connections.size();
    stats.append(workerStats);
  }

  return stats;
}

}// namespace Star
