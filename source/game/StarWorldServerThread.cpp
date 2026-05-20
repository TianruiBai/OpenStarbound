#include "StarWorldServerThread.hpp"
#include "StarTickRateMonitor.hpp"
#include "StarNpc.hpp"
#include "StarRoot.hpp"
#include "StarLogging.hpp"
#include "StarAssets.hpp"
#include "StarPlayer.hpp"
#include "StarTime.hpp"
#include "StarUniverseSettings.hpp"

namespace Star {

WorldServerThread::WorldServerThread(WorldServerPtr server, WorldId worldId)
  : Thread("WorldServerThread: " + printWorldId(worldId)),
    m_worldServer(std::move(server)),
    m_worldId(std::move(worldId)),
    m_stop(false),
    m_errorOccurred(false),
    m_shouldExpire(true) {
  m_threadTimings.resize(static_cast<size_t>(ThreadTimingPhase::Count));
  if (m_worldServer)
    m_worldServer->setWorldId(printWorldId(m_worldId));
}

WorldServerThread::~WorldServerThread() {
  m_stop = true;
  join();

  RecursiveMutexLocker locker(m_mutex);
  for (auto clientId : m_worldServer->clientIds())
    removeClient(clientId);
}

WorldId WorldServerThread::worldId() const {
  return m_worldId;
}

void WorldServerThread::start() {
  m_stop = false;
  m_errorOccurred = false;
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS WorldServerThread: using threadless update for {}", printWorldId(m_worldId));
#else
  Thread::start();
#endif
}

void WorldServerThread::stop() {
  m_stop = true;
#ifndef STAR_PLATFORM_N3DS
  Thread::join();
#endif
}

#ifdef STAR_PLATFORM_N3DS
void WorldServerThread::n3dsUpdate() {
  if (m_stop || m_errorOccurred)
    return;

  try {
    update(WorldServerFidelity::Minimum);
  } catch (std::exception const& exception) {
    Logger::error("N3DS WorldServerThread exception caught: {}", outputException(exception, true));
    m_errorOccurred = true;
    failPendingCommands("N3DS world server update failed");
  }
}
#endif

void WorldServerThread::setPause(shared_ptr<const atomic<bool>> pause) {
  m_pause = pause;
}

bool WorldServerThread::serverErrorOccurred() {
  return m_errorOccurred;
}

bool WorldServerThread::shouldExpire() {
  return m_shouldExpire;
}

WorldServerThread::CommandStats WorldServerThread::commandStats() const {
  CommandStats stats{};
  {
    MutexLocker locker(m_commandMutex);
    stats.pending = m_commandQueue.size();
    auto now = Time::monotonicMicroseconds();
    for (auto const& command : m_commandQueue)
      stats.oldestPendingAgeMicroseconds = max<int64_t>(stats.oldestPendingAgeMicroseconds, now - command.queuedAt);
  }
  stats.processed = m_commandsProcessed;
  stats.direct = m_commandsProcessedDirect;
  stats.failed = m_commandsFailed;
  stats.waitMicroseconds = m_commandWaitMicroseconds;
  return stats;
}

List<ServerTimingRecord> WorldServerThread::threadTimingRecords() const {
  MutexLocker locker(m_threadTimingsMutex);
  List<ServerTimingRecord> records;
  for (size_t i = 0; i < m_threadTimings.size(); ++i)
    records.append({threadTimingPhaseName(static_cast<ThreadTimingPhase>(i)), m_threadTimings[i]});
  return records;
}

List<ServerTimingStatus> WorldServerThread::threadTimingStatus() const {
  return serverTimingStatusList(threadTimingRecords());
}

List<ServerTimingRecord> WorldServerThread::worldTimingRecords() const {
  RecursiveMutexLocker locker(m_mutex);
  return m_worldServer->updateTimingRecords();
}

List<ServerTimingStatus> WorldServerThread::worldTimingStatus() const {
  return serverTimingStatusList(worldTimingRecords());
}

WorldServer::PacketPreparationStats WorldServerThread::packetPreparationStats() const {
  RecursiveMutexLocker locker(m_mutex);
  return m_worldServer->packetPreparationStats();
}

WorldStorageTimingStats WorldServerThread::storageTimingStats() const {
  RecursiveMutexLocker locker(m_mutex);
  return m_worldServer->storageTimingStats();
}

WorldServer::Phase6WorldParallelismStats WorldServerThread::phase6WorldParallelismStats() const {
  RecursiveMutexLocker locker(m_mutex);
  return m_worldServer->phase6WorldParallelismStats();
}

char const* WorldServerThread::threadTimingPhaseName(ThreadTimingPhase phase) {
  switch (phase) {
    case ThreadTimingPhase::Loop:
      return "loop";
    case ThreadTimingPhase::ProcessCommands:
      return "processCommands";
    case ThreadTimingPhase::IncomingPackets:
      return "incomingPackets";
    case ThreadTimingPhase::WorldUpdate:
      return "worldUpdate";
    case ThreadTimingPhase::Messages:
      return "messages";
    case ThreadTimingPhase::OutgoingPackets:
      return "outgoingPackets";
    case ThreadTimingPhase::UpdateAction:
      return "updateAction";
    case ThreadTimingPhase::Sync:
      return "sync";
    case ThreadTimingPhase::Count:
      break;
  }

  return "unknown";
}

void WorldServerThread::recordThreadTiming(ThreadTimingPhase phase, int64_t durationMicroseconds) {
  auto index = static_cast<size_t>(phase);
  if (index >= m_threadTimings.size())
    return;

  MutexLocker locker(m_threadTimingsMutex);
  recordServerTiming(m_threadTimings[index], durationMicroseconds);
}

void WorldServerThread::setWorldPause(bool pause) {
  try {
    executeCommand("setWorldPause", [pause](WorldServerThread*, WorldServer* worldServer) {
        worldServer->setPause(pause);
      });
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }
}

void WorldServerThread::executeCommand(String const& name, WorldServerAction action) {
  if (!isRunning() || m_stop || m_errorOccurred) {
    RecursiveMutexLocker locker(m_mutex);
    action(this, m_worldServer.get());
    ++m_commandsProcessedDirect;
    return;
  }

  auto state = make_shared<CommandState>();
  {
    MutexLocker locker(m_commandMutex);
    m_commandQueue.append(Command{name, std::move(action), state, Time::monotonicMicroseconds()});
  }

  MutexLocker stateLocker(state->mutex);
  while (!state->finished)
    state->condition.wait(state->mutex);

  if (state->failed)
    throw StarException::format("World server queued command failed: {}", state->error);
}

void WorldServerThread::processCommands() {
  List<Command> commands;
  {
    MutexLocker locker(m_commandMutex);
    commands = take(m_commandQueue);
  }

  for (auto& command : commands) {
    bool failed = false;
    String error;
    try {
      command.action(this, m_worldServer.get());
    } catch (std::exception const& e) {
      failed = true;
      error = printException(e, true);
      Logger::error("WorldServerThread exception caught running queued command '{}': {}", command.name, error);
      m_errorOccurred = true;
    }

    ++m_commandsProcessed;
    m_commandWaitMicroseconds += Time::monotonicMicroseconds() - command.queuedAt;
    if (failed)
      ++m_commandsFailed;

    {
      MutexLocker stateLocker(command.state->mutex);
      command.state->failed = failed;
      command.state->error = std::move(error);
      command.state->finished = true;
    }
    command.state->condition.broadcast();
  }
}

void WorldServerThread::failPendingCommands(String const& error) {
  List<Command> commands;
  {
    MutexLocker locker(m_commandMutex);
    commands = take(m_commandQueue);
  }

  auto now = Time::monotonicMicroseconds();
  for (auto& command : commands) {
    ++m_commandsFailed;
    m_commandWaitMicroseconds += now - command.queuedAt;
    {
      MutexLocker stateLocker(command.state->mutex);
      command.state->failed = true;
      command.state->error = error;
      command.state->finished = true;
    }
    command.state->condition.broadcast();
  }
}

bool WorldServerThread::spawnTargetValid(SpawnTarget const& spawnTarget) {
  try {
    bool result = false;
    executeCommand("spawnTargetValid", [&spawnTarget, &result](WorldServerThread*, WorldServer* worldServer) {
        result = worldServer->spawnTargetValid(spawnTarget);
      });
    return result;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return false;
  }
}

bool WorldServerThread::addClient(ConnectionId clientId, SpawnTarget const& spawnTarget, bool isLocal, bool isAdmin, NetCompatibilityRules netRules) {
  try {
    bool added = false;
    executeCommand("addClient", [this, clientId, spawnTarget, isLocal, isAdmin, netRules, &added](WorldServerThread*, WorldServer* worldServer) {
        if (worldServer->addClient(clientId, spawnTarget, isLocal, isAdmin, netRules)) {
          m_clients.add(clientId);
          added = true;
        }
      });
    return added;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return false;
  }
}

List<PacketPtr> WorldServerThread::removeClient(ConnectionId clientId) {
  List<PacketPtr> outgoingPackets;
  try {
    executeCommand("removeClient", [this, clientId, &outgoingPackets](WorldServerThread*, WorldServer* worldServer) {
        if (!m_clients.contains(clientId))
          return;

        RecursiveMutexLocker queueLocker(m_queueMutex);
        try {
          auto incomingPackets = take(m_incomingPacketQueue[clientId]);
          if (worldServer->hasClient(clientId))
            worldServer->handleIncomingPackets(clientId, std::move(incomingPackets));

          outgoingPackets = take(m_outgoingPacketQueue[clientId]);
          if (worldServer->hasClient(clientId))
            outgoingPackets.appendAll(worldServer->removeClient(clientId));

        } catch (std::exception const& e) {
          Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
          m_errorOccurred = true;
        }

        m_clients.remove(clientId);
        m_incomingPacketQueue.remove(clientId);
        m_outgoingPacketQueue.remove(clientId);
      });

  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }
  return outgoingPackets;
}

bool WorldServerThread::executeForClient(ConnectionId clientId, function<void(WorldServer*, PlayerPtr)> action) {
  bool success = false;
  std::exception_ptr actionException;
  executeCommand("executeForClient", [clientId, action = std::move(action), &success, &actionException](WorldServerThread*, WorldServer* worldServer) {
        if (auto player = worldServer->clientPlayer(clientId)) {
          try {
            action(worldServer, player);
            success = true;
          } catch (...) {
            actionException = std::current_exception();
          }
        }
      });
  if (actionException)
    std::rethrow_exception(actionException);
  return success;
}

List<ConnectionId> WorldServerThread::clients() const {
  RecursiveMutexLocker locker(m_mutex);
  return m_clients.values();
}

bool WorldServerThread::hasClient(ConnectionId clientId) const {
  RecursiveMutexLocker locker(m_mutex);
  return m_clients.contains(clientId);
}

bool WorldServerThread::noClients() const {
  RecursiveMutexLocker locker(m_mutex);
  return m_clients.empty();
}


List<ConnectionId> WorldServerThread::erroredClients() const {
  RecursiveMutexLocker locker(m_mutex);
  auto unerroredClients = HashSet<ConnectionId>::from(m_worldServer->clientIds());
  return m_clients.difference(unerroredClients).values();
}

void WorldServerThread::pushIncomingPackets(ConnectionId clientId, List<PacketPtr> packets) {
  RecursiveMutexLocker queueLocker(m_queueMutex);
  m_incomingPacketQueue[clientId].appendAll(std::move(packets));
}

List<PacketPtr> WorldServerThread::pullOutgoingPackets(ConnectionId clientId) {
  RecursiveMutexLocker queueLocker(m_queueMutex);
  return take(m_outgoingPacketQueue[clientId]);
}

Maybe<Vec2F> WorldServerThread::playerRevivePosition(ConnectionId clientId) const {
  try {
    Maybe<Vec2F> result;
    const_cast<WorldServerThread*>(this)->executeCommand("playerRevivePosition", [clientId, &result](WorldServerThread*, WorldServer* worldServer) {
        if (auto player = worldServer->clientPlayer(clientId))
          result = player->position() + player->feetOffset();
      });
    return result;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

Maybe<pair<String, String>> WorldServerThread::pullNewPlanetType() {
  try {
    Maybe<pair<String, String>> result;
    executeCommand("pullNewPlanetType", [&result](WorldServerThread*, WorldServer* worldServer) {
        result = worldServer->pullNewPlanetType();
      });
    return result;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

void WorldServerThread::setWeather(String const& weatherName, bool force) {
  try {
    executeCommand("setWeather", [weatherName, force](WorldServerThread*, WorldServer* worldServer) {
        worldServer->setWeather(weatherName, force);
      });
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }
}

StringList WorldServerThread::weatherList() {
  try {
    StringList result;
    executeCommand("weatherList", [&result](WorldServerThread*, WorldServer* worldServer) {
        result = worldServer->weatherList();
      });
    return result;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

List<ItemDescriptor> WorldServerThread::containerPutItems(EntityId entityId, List<ItemDescriptor> items) {
  try {
    List<ItemDescriptor> result;
    executeCommand("containerPutItems", [entityId, items, &result](WorldServerThread*, WorldServer* worldServer) {
        result = worldServer->containerPutItems(entityId, items);
      });
    return result;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return items;
  }
}

void WorldServerThread::setUniverseFlag(String const& flagName) {
  try {
    executeCommand("setUniverseFlag", [flagName](WorldServerThread*, WorldServer* worldServer) {
        worldServer->universeSettings()->setFlag(flagName);
      });
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }
}

bool WorldServerThread::placeDungeon(String const& dungeonName, Vec2I const& position, Maybe<DungeonId> dungeonId, bool forcePlacement) {
  try {
    bool result = false;
    executeCommand("placeDungeon", [dungeonName, position, dungeonId, forcePlacement, &result](WorldServerThread*, WorldServer* worldServer) {
        result = worldServer->placeDungeon(dungeonName, position, dungeonId, forcePlacement);
      });
    return result;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return false;
  }
}

void WorldServerThread::startFlyingSky(bool enterHyperspace, bool startInWarp, Json settings) {
  try {
    executeCommand("startFlyingSky", [enterHyperspace, startInWarp, settings = std::move(settings)](WorldServerThread*, WorldServer* worldServer) mutable {
        worldServer->startFlyingSky(enterHyperspace, startInWarp, std::move(settings));
      });
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }
}

void WorldServerThread::stopFlyingSkyAt(SkyParameters const& destination) {
  try {
    executeCommand("stopFlyingSkyAt", [destination](WorldServerThread*, WorldServer* worldServer) {
        worldServer->stopFlyingSkyAt(destination);
      });
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }
}

WorldServerThread::ShipUpgradeApplicationResult WorldServerThread::applyShipUpgrades(String fallbackSpecies, ShipUpgrades shipUpgrades, StringMap<StringList> const& speciesShips, WorldChunks oldShipChunks) {
  ShipUpgradeApplicationResult result;
  result.species = fallbackSpecies;
  result.shipUpgrades = shipUpgrades;

  try {
    executeCommand("applyShipUpgrades", [fallbackSpecies = std::move(fallbackSpecies), shipUpgrades = std::move(shipUpgrades), oldShipChunks = std::move(oldShipChunks), &speciesShips, &result](WorldServerThread*, WorldServer* shipWorld) mutable {
        String species;
        Json jSpecies = shipWorld->getProperty("ship.species");
        if (jSpecies.isType(Json::Type::String))
          species = jSpecies.toString();
        else
          shipWorld->setProperty("ship.species", species = fallbackSpecies);

        result.species = species;
        result.shipUpgrades = shipUpgrades;
        auto const& speciesShipConfigs = speciesShips.get(species);
        Json jOldShipLevel = shipWorld->getProperty("ship.level");
        unsigned newShipLevel = min<unsigned>(speciesShipConfigs.size() - 1, result.shipUpgrades.shipLevel);

        if (jOldShipLevel.isType(Json::Type::Int)) {
          auto oldShipLevel = jOldShipLevel.toUInt();
          if (oldShipLevel < newShipLevel) {
            for (unsigned i = oldShipLevel + 1; i <= newShipLevel; ++i) {
              auto shipStructure = WorldStructure(speciesShipConfigs[i]);
              shipWorld->setCentralStructure(shipStructure);
              result.shipUpgrades.apply(shipStructure.configValue("shipUpgrades"));
            }

            result.shipChunkUpdate = shipWorld->readChunkUpdate(oldShipChunks);
          }
        }

        shipWorld->setProperty("ship.level", result.shipUpgrades.shipLevel);
        shipWorld->setProperty("ship.maxFuel", result.shipUpgrades.maxFuel);
        shipWorld->setProperty("ship.crewSize", result.shipUpgrades.crewSize);
        shipWorld->setProperty("ship.fuelEfficiency", result.shipUpgrades.fuelEfficiency);
      });
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }

  return result;
}

void WorldServerThread::executeAction(WorldServerAction action) {
  RecursiveMutexLocker locker(m_mutex);
  action(this, m_worldServer.get());
}

void WorldServerThread::setUpdateAction(WorldServerAction updateAction) {
  RecursiveMutexLocker locker(m_mutex);
  m_updateAction = updateAction;
}

void WorldServerThread::passMessages(List<Message>&& messages) {
  RecursiveMutexLocker locker(m_messageMutex);
  m_messages.appendAll(std::move(messages));
}

void WorldServerThread::unloadAll(bool force) {
  try {
    executeCommand("unloadAll", [force](WorldServerThread*, WorldServer* worldServer) {
        worldServer->unloadAll(force);
      });
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }
}

WorldChunks WorldServerThread::readChunks() {
  try {
    WorldChunks chunks;
    executeCommand("readChunks", [&chunks](WorldServerThread*, WorldServer* worldServer) {
        chunks = worldServer->readChunks();
      });
    return chunks;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

WorldChunks WorldServerThread::readChunkUpdate(WorldChunks oldChunks) {
  try {
    WorldChunks update;
    executeCommand("readChunkUpdate", [&oldChunks, &update](WorldServerThread*, WorldServer* worldServer) {
        update = worldServer->readChunkUpdate(oldChunks);
      });
    return update;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

void WorldServerThread::run() {
  try {
    auto& root = Root::singleton();
    double updateMeasureWindow = root.assets()->json("/universe_server.config:updateMeasureWindow").toDouble();
    double fidelityDecrementScore = root.assets()->json("/universe_server.config:fidelityDecrementScore").toDouble();
    double fidelityIncrementScore = root.assets()->json("/universe_server.config:fidelityIncrementScore").toDouble();

    String serverFidelityMode = root.configuration()->get("serverFidelity").toString();
    Maybe<WorldServerFidelity> lockedFidelity;
    if (!serverFidelityMode.equalsIgnoreCase("automatic"))
      lockedFidelity = WorldServerFidelityNames.getLeft(serverFidelityMode);

    double storageInterval = root.assets()->json("/universe_server.config:worldStorageInterval").toDouble() / 1000.0;
    Timer storageTimer = Timer::withTime(storageInterval);

    TickRateApproacher tickApproacher(1.0f / ServerGlobalTimestep, updateMeasureWindow);
    double fidelityScore = 0.0;
    WorldServerFidelity automaticFidelity = WorldServerFidelity::Medium;

    while (!m_stop && !m_errorOccurred) {
      auto loopStart = Time::monotonicMicroseconds();
      auto fidelity = lockedFidelity.value(automaticFidelity);
      LogMap::set(strf("server_{}_fidelity", m_worldId), WorldServerFidelityNames.getRight(fidelity));
      LogMap::set(strf("server_{}_update", m_worldId), strf("{:4.2f}Hz", tickApproacher.rate()));

      update(fidelity);
      tickApproacher.setTargetTickRate(1.0f / ServerGlobalTimestep);
      tickApproacher.tick();

      if (storageTimer.timeUp()) {
        auto syncStart = Time::monotonicMicroseconds();
        sync();
        recordThreadTiming(ThreadTimingPhase::Sync, Time::monotonicMicroseconds() - syncStart);
        storageTimer.restart(storageInterval);
      }

      recordThreadTiming(ThreadTimingPhase::Loop, Time::monotonicMicroseconds() - loopStart);

      double spareTime = tickApproacher.spareTime();
      fidelityScore += spareTime;

      if (fidelityScore <= fidelityDecrementScore) {
        if (automaticFidelity > WorldServerFidelity::Minimum)
          automaticFidelity = (WorldServerFidelity)((int)automaticFidelity - 1);
        fidelityScore = 0.0;
      }

      if (fidelityScore >= fidelityIncrementScore) {
        if (automaticFidelity < WorldServerFidelity::High)
          automaticFidelity = (WorldServerFidelity)((int)automaticFidelity + 1);
        fidelityScore = 0.0;
      }

      int64_t spareMilliseconds = floor(spareTime * 1000);
      if (spareMilliseconds > 0)
        Thread::sleepPrecise(spareMilliseconds);
    }
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }

  failPendingCommands("World server thread stopped before queued command could run");
}

void WorldServerThread::update(WorldServerFidelity fidelity) {
  RecursiveMutexLocker locker(m_mutex);

  auto timePhase = [this](ThreadTimingPhase phase, auto&& action) {
    auto start = Time::monotonicMicroseconds();
    try {
      action();
      recordThreadTiming(phase, Time::monotonicMicroseconds() - start);
    } catch (std::exception const& e) {
#ifdef STAR_PLATFORM_N3DS
      Logger::error("N3DS WorldServerThread: exception in phase {}: {}", threadTimingPhaseName(phase), outputException(e, true));
#endif
      throw;
    }
  };

  timePhase(ThreadTimingPhase::ProcessCommands, [&]() { processCommands(); });

  auto unerroredClientIds = m_worldServer->clientIds();
  timePhase(ThreadTimingPhase::IncomingPackets, [&]() {
    for (auto clientId : unerroredClientIds) {
      RecursiveMutexLocker queueLocker(m_queueMutex);
      auto incomingPackets = take(m_incomingPacketQueue[clientId]);
      queueLocker.unlock();
      try {
        m_worldServer->handleIncomingPackets(clientId, std::move(incomingPackets));
      } catch (std::exception const& e) {
        Logger::error("WorldServerThread exception caught handling incoming packets for client {}: {}",
            clientId, outputException(e, true));
        RecursiveMutexLocker queueLocker(m_queueMutex);
        m_outgoingPacketQueue[clientId].appendAll(m_worldServer->removeClient(clientId));
        unerroredClientIds.remove(clientId);
      }
    }
  });

  timePhase(ThreadTimingPhase::WorldUpdate, [&]() {
    float dt = ServerGlobalTimestep * GlobalTimescale;
    m_worldServer->setFidelity(fidelity);
    if (dt > 0.0f && (!m_pause || *m_pause == false))
      m_worldServer->update(dt);
  });

  List<Message> messages;
  timePhase(ThreadTimingPhase::Messages, [&]() {
    {
      RecursiveMutexLocker locker(m_messageMutex);
      messages = std::move(m_messages);
    }
    for (auto& message : messages) {
      if (auto resp = m_worldServer->receiveMessage(ServerConnectionId, message.message, message.args))
        message.promise.fulfill(*resp);
      else
        message.promise.fail("Message not handled by world");
    }
  });

  timePhase(ThreadTimingPhase::OutgoingPackets, [&]() {
    for (auto& clientId : unerroredClientIds) {
      auto outgoingPackets = m_worldServer->getOutgoingPackets(clientId);
      RecursiveMutexLocker queueLocker(m_queueMutex);
      m_outgoingPacketQueue[clientId].appendAll(std::move(outgoingPackets));
    }
  });

  m_shouldExpire = m_worldServer->shouldExpire();

  if (m_updateAction)
    timePhase(ThreadTimingPhase::UpdateAction, [&]() { m_updateAction(this, m_worldServer.get()); });
}

void WorldServerThread::sync() {
  RecursiveMutexLocker locker(m_mutex);
  Logger::debug("WorldServer: periodic sync to disk of world {}", m_worldId);
  m_worldServer->sync();
}

}
