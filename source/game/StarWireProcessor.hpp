#pragma once

#include "StarWiring.hpp"

namespace Star {

STAR_CLASS(WireEntity);
STAR_CLASS(WorldStorage);

STAR_CLASS(WireProcessor);

// Propogates WireEntity signals, and keeps networks of WireEntities alive
// together.
class WireProcessor : public WireCoordinator {
public:
  struct ProcessStats {
    size_t initialEntities = 0;
    size_t loadedEntities = 0;
    uint64_t networkLoads = 0;
    uint64_t evaluatedEntities = 0;
    uint64_t networkSignatureChecks = 0;
    uint64_t cleanNetworkSignatures = 0;
    uint64_t dirtyNetworkSignatures = 0;
    uint64_t topologyDirtyNetworkSignatures = 0;
    uint64_t outputDirtyNetworkSignatures = 0;
    uint64_t cleanNetworkEntities = 0;
    uint64_t dirtyNetworkEntities = 0;
  };

  WireProcessor(WorldStoragePtr worldStorage);

  ProcessStats process();

  bool readInputConnection(WireConnection const& connection) override;

private:
  struct WireEntityState {
    WireEntity* wireEntity;
    List<bool> outputStates;
    bool networkLoaded;
  };

  struct WireNetworkSignature {
    size_t topologyHash;
    size_t outputHash;
    size_t entityCount;
  };

  // Add the given WireEntity to the working entities set, populating inbound /
  // outbound nodes and states.
  void populateWorking(WireEntity* wireEntity);
  // Scans a wire network, starting at an entity at the given position, while
  // also loading any unloaded entries in the network and marking each entry as
  // now having been 'networkLoaded'.
  List<Vec2I> loadNetwork(Vec2I tilePosition);
  void recordNetworkSignature(ProcessStats& stats, List<Vec2I> networkPositions, StableHashMap<Vec2I, WireNetworkSignature>& nextNetworkSignatures) const;
  WireNetworkSignature buildNetworkSignature(List<Vec2I> const& networkPositions) const;

  WorldStoragePtr m_worldStorage;
  StableHashMap<Vec2I, WireEntityState> m_workingWireEntities;
  StableHashMap<Vec2I, WireNetworkSignature> m_previousNetworkSignatures;
};

}
