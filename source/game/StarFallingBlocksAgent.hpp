#pragma once

#include "StarVector.hpp"
#include "StarSet.hpp"
#include "StarMap.hpp"
#include "StarRandom.hpp"
#include "StarGameTypes.hpp"
#include "StarWorldTiles.hpp"
#include "StarHash.hpp"

namespace Star {

STAR_CLASS(MaterialDatabase);
STAR_CLASS(FallingBlocksFacade);
STAR_CLASS(FallingBlocksAgent);

enum class FallingBlockType {
  Immovable,
  Falling,
  Cascading,
  Open
};

class FallingBlocksFacade {
public:
  virtual ~FallingBlocksFacade() = default;

  virtual FallingBlockType blockType(Vec2I const& pos) = 0;
  virtual void moveBlock(Vec2I const& from, Vec2I const& to) = 0;
};

class FallingBlocksAgent {
public:
  struct UpdateStats {
    size_t pendingPositions = 0;
    size_t nextPendingPositions = 0;
    uint64_t processedPositions = 0;
    uint64_t movedBlocks = 0;
    uint64_t pendingPositionSignature = 0;
    uint64_t processedPositionSignature = 0;
    uint64_t movedBlockSignature = 0;
    uint64_t nextPendingPositionSignature = 0;
  };

  FallingBlocksAgent(FallingBlocksFacadePtr worldFacade);

  void setRandomSeed(uint64_t seed);

  UpdateStats update(bool collectSignatures = false);

  void visitLocation(Vec2I const& location);
  void visitRegion(RectI const& region);

private:
  FallingBlocksFacadePtr m_facade;
  float m_immediateUpwardPropagateProbability;
  HashSet<Vec2I> m_pending;
  RandomSource m_random;
};

}
