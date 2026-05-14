#include "StarWireProcessor.hpp"
#include "StarWorldStorage.hpp"
#include "StarEntityMap.hpp"
#include "StarWireEntity.hpp"
#include "StarHash.hpp"
#include "StarLogging.hpp"

namespace Star {

WireProcessor::WireProcessor(WorldStoragePtr worldStorage) {
  m_worldStorage = worldStorage;
}

WireProcessor::ProcessStats WireProcessor::process() {
  ProcessStats stats;
  StableHashMap<Vec2I, WireNetworkSignature> nextNetworkSignatures;

  // First, populate all the working entities that are already live
  m_worldStorage->entityMap()->forAllEntities([&](EntityPtr const& entity) {
    if (auto wireEntity = as<WireEntity>(entity.get()))
      populateWorking(wireEntity);
  });
  stats.initialEntities = m_workingWireEntities.size();

  // Then, scan the network of each entity in the working set.  This may, as a
  // side effect, load further unconnected wire entities. Because our policy is
  // to try as hard as possible to make sure that the entire wire entity
  // network to be loaded at once or not at all, we need to make sure that each
  // new disconnected entity also has its network loaded and so on.  Thus, if
  // the working entities size changes during scanning, simply scan the whole
  // thing again until the size stops changing.
  while (true) {
    size_t oldWorkingSize = m_workingWireEntities.size();
    for (auto const& p : m_workingWireEntities.keys()) {
      if (!m_workingWireEntities.get(p).networkLoaded) {
        stats.networkLoads += 1;
        recordNetworkSignature(stats, loadNetwork(p), nextNetworkSignatures);
      }
    }
    if (m_workingWireEntities.size() == oldWorkingSize)
      break;
  }

  stats.loadedEntities = m_workingWireEntities.size();
  stats.evaluatedEntities = m_workingWireEntities.size();
  for (auto const& p : m_workingWireEntities)
    p.second.wireEntity->evaluate(this);

  m_workingWireEntities.clear();
  m_previousNetworkSignatures = std::move(nextNetworkSignatures);
  return stats;
}

bool WireProcessor::readInputConnection(WireConnection const& connection) {
  if (auto wes = m_workingWireEntities.ptr(connection.entityLocation))
    return wes->outputStates.get(connection.nodeIndex);
  return false;
}

void WireProcessor::populateWorking(WireEntity* wireEntity) {
  auto p = m_workingWireEntities.insert(wireEntity->tilePosition(), WireEntityState{nullptr, {}, false});
  if (!p.second) {
    if (p.first->second.wireEntity != wireEntity)
      Logger::debug("Multiple wire entities share tile position: {}", wireEntity->position());
    return;
  }
  auto& wes = p.first->second;
  wes.wireEntity = wireEntity;
  size_t outputNodeCount = wes.wireEntity->nodeCount(WireDirection::Output);
  wes.outputStates.resize(outputNodeCount);
  for (size_t i = 0; i < outputNodeCount; ++i)
    wes.outputStates[i] = wes.wireEntity->nodeState({WireDirection::Output, i});
}

List<Vec2I> WireProcessor::loadNetwork(Vec2I tilePosition) {
  HashSet<WorldStorage::Sector> networkSectors;
  Maybe<float> highestTtl;
  List<Vec2I> networkPositions;

  // Recursively load a given WireEntity at the given position.  Returns true
  // if that wire entity was found.
  // TODO: This is depth first recursive, because that is the simplest thing,
  // but if this causes issues with recursion depth it can be changed.
  function<bool(Vec2I)> doLoad;
  doLoad = [&](Vec2I const& pos) {
    auto sector = m_worldStorage->sectorForPosition(pos);
    if (!sector)
      return false;

    if (m_worldStorage->sectorLoadLevel(*sector) == SectorLoadLevel::Loaded) {
      auto ttl = *m_worldStorage->sectorTimeToLive(*sector);
      if (highestTtl)
        highestTtl = max(*highestTtl, ttl);
      else
        highestTtl = ttl;
    } else {
      m_worldStorage->loadSector(*sector);
      m_worldStorage->entityMap()->forEachEntity(RectF(*m_worldStorage->regionForSector(*sector)), [&](EntityPtr const& entity) {
          if (auto wireEntity = as<WireEntity>(entity.get()))
            populateWorking(wireEntity);
        });
    }

    auto wes = m_workingWireEntities.ptr(pos);
    if (!wes)
      return false;
    if (wes->networkLoaded)
      return true;

    wes->networkLoaded = true;
    networkPositions.append(pos);
    networkSectors.add(*sector);

    // Recursively descend into all the inbound and outbound nodes, and if we
    // ever cannot load the wire entity for a connection, go ahead and remove
    // the connection.
    size_t inboundNodeCount = wes->wireEntity->nodeCount(WireDirection::Input);
    for (size_t i = 0; i < inboundNodeCount; ++i) {
      for (auto const& connection : wes->wireEntity->connectionsForNode({WireDirection::Input, i})) {
        if (!doLoad(connection.entityLocation))
          wes->wireEntity->removeNodeConnection({WireDirection::Input, i}, connection);
      }
    }
    size_t outboundNodeCount = wes->wireEntity->nodeCount(WireDirection::Output);
    for (size_t i = 0; i < outboundNodeCount; ++i) {
      for (auto const& connection : wes->wireEntity->connectionsForNode({WireDirection::Output, i})) {
        if (!doLoad(connection.entityLocation))
          wes->wireEntity->removeNodeConnection({WireDirection::Output, i}, connection);
      }
    }
    return true;
  };

  doLoad(tilePosition);

  // Set the sector ttl for the entire network to be equal to the highest
  // entry, so that the entire network either lives or dies together, but
  // without artificially extending the lifetime of the network.
  if (highestTtl) {
    for (auto const& sector : networkSectors)
      m_worldStorage->setSectorTimeToLive(sector, *highestTtl);
  }

  return networkPositions;
}

void WireProcessor::recordNetworkSignature(ProcessStats& stats, List<Vec2I> networkPositions, StableHashMap<Vec2I, WireNetworkSignature>& nextNetworkSignatures) const {
  if (networkPositions.empty())
    return;

  networkPositions.sort([](Vec2I const& lhs, Vec2I const& rhs) {
      return tie(lhs[0], lhs[1]) < tie(rhs[0], rhs[1]);
    });

  auto rootPosition = networkPositions.first();
  auto signature = buildNetworkSignature(networkPositions);
  nextNetworkSignatures.set(rootPosition, signature);
  stats.networkSignatureChecks += 1;

  bool topologyDirty = true;
  bool outputDirty = true;
  if (auto previousSignature = m_previousNetworkSignatures.ptr(rootPosition)) {
    topologyDirty = previousSignature->topologyHash != signature.topologyHash || previousSignature->entityCount != signature.entityCount;
    outputDirty = previousSignature->outputHash != signature.outputHash;
  }

  if (topologyDirty || outputDirty) {
    stats.dirtyNetworkSignatures += 1;
    stats.dirtyNetworkEntities += signature.entityCount;
    if (topologyDirty)
      stats.topologyDirtyNetworkSignatures += 1;
    if (outputDirty)
      stats.outputDirtyNetworkSignatures += 1;
  } else {
    stats.cleanNetworkSignatures += 1;
    stats.cleanNetworkEntities += signature.entityCount;
  }
}

WireProcessor::WireNetworkSignature WireProcessor::buildNetworkSignature(List<Vec2I> const& networkPositions) const {
  WireNetworkSignature signature{0, 0, networkPositions.size()};

  for (auto const& position : networkPositions) {
    auto const& wireEntityState = m_workingWireEntities.get(position);
    hashCombine(signature.topologyHash, hashOf(position));
    hashCombine(signature.outputHash, hashOf(position));

    for (auto direction : {WireDirection::Input, WireDirection::Output}) {
      auto nodeCount = wireEntityState.wireEntity->nodeCount(direction);
      hashCombine(signature.topologyHash, hashOf(direction, nodeCount));
      for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
        auto connections = wireEntityState.wireEntity->connectionsForNode({direction, nodeIndex});
        connections.sort([](WireConnection const& lhs, WireConnection const& rhs) {
            return tie(lhs.entityLocation[0], lhs.entityLocation[1], lhs.nodeIndex) < tie(rhs.entityLocation[0], rhs.entityLocation[1], rhs.nodeIndex);
          });
        hashCombine(signature.topologyHash, hashOf(direction, nodeIndex, connections.size()));
        for (auto const& connection : connections)
          hashCombine(signature.topologyHash, hashOf(direction, nodeIndex, connection.entityLocation, connection.nodeIndex));
      }
    }

    for (size_t nodeIndex = 0; nodeIndex < wireEntityState.outputStates.size(); ++nodeIndex)
      hashCombine(signature.outputHash, hashOf(nodeIndex, wireEntityState.outputStates[nodeIndex]));
  }

  return signature;
}

}
