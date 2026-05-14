#include "StarFallingBlocksAgent.hpp"
#include "StarRoot.hpp"
#include "StarAssets.hpp"

namespace Star {

namespace {

uint64_t positionListSignature(List<Vec2I> positions) {
  positions.sort([](Vec2I const& lhs, Vec2I const& rhs) {
      if (lhs[0] != rhs[0])
        return lhs[0] < rhs[0];
      return lhs[1] < rhs[1];
    });

  size_t signature = 0;
  for (auto const& position : positions)
    hashCombine(signature, hashOf(position));
  return signature;
}

uint64_t moveListSignature(List<pair<Vec2I, Vec2I>> moves) {
  moves.sort([](auto const& lhs, auto const& rhs) {
      if (lhs.first[0] != rhs.first[0])
        return lhs.first[0] < rhs.first[0];
      if (lhs.first[1] != rhs.first[1])
        return lhs.first[1] < rhs.first[1];
      if (lhs.second[0] != rhs.second[0])
        return lhs.second[0] < rhs.second[0];
      return lhs.second[1] < rhs.second[1];
    });

  size_t signature = 0;
  for (auto const& move : moves)
    hashCombine(signature, hashOf(move.first, move.second));
  return signature;
}

}

FallingBlocksAgent::FallingBlocksAgent(FallingBlocksFacadePtr worldFacade)
  : m_facade(std::move(worldFacade)) {
  m_immediateUpwardPropagateProbability = Root::singleton().assets()->json("/worldserver.config:fallingBlocksImmediateUpwardPropogateProbability").toFloat();
}

void FallingBlocksAgent::setRandomSeed(uint64_t seed) {
  m_random.init(seed);
}

FallingBlocksAgent::UpdateStats FallingBlocksAgent::update(bool collectSignatures) {
  UpdateStats stats;
  HashSet<Vec2I> processing = take(m_pending);
  stats.pendingPositions = processing.size();
  if (collectSignatures)
    stats.pendingPositionSignature = positionListSignature(processing.values());

  List<Vec2I> processedPositions;
  List<pair<Vec2I, Vec2I>> movedBlocks;

  while (!processing.empty()) {
    List<Vec2I> positions;
    for (auto const& pos : take(processing))
      positions.append(pos);

    m_random.shuffle(positions);

    positions.sort([](auto const& a, auto const& b) {
        return a[1] < b[1];
      });

    for (auto const& pos : positions) {
      stats.processedPositions += 1;
      if (collectSignatures)
        processedPositions.append(pos);
      Vec2I belowPos = pos + Vec2I(0, -1);
      Vec2I belowLeftPos = pos + Vec2I(-1, -1);
      Vec2I belowRightPos = pos + Vec2I(1, -1);

      FallingBlockType thisBlock = m_facade->blockType(pos);
      FallingBlockType belowBlock = m_facade->blockType(belowPos);

      Maybe<Vec2I> moveTo;

      if (thisBlock == FallingBlockType::Falling) {
        if (belowBlock == FallingBlockType::Open)
          moveTo = belowPos;
      } else if (thisBlock == FallingBlockType::Cascading) {
        if (belowBlock == FallingBlockType::Open) {
          moveTo = belowPos;
        } else {
          FallingBlockType belowLeftBlock = m_facade->blockType(belowLeftPos);
          FallingBlockType belowRightBlock = m_facade->blockType(belowRightPos);

          if (belowLeftBlock == FallingBlockType::Open && belowRightBlock == FallingBlockType::Open)
            moveTo = m_random.randb() ? belowLeftPos : belowRightPos;
          else if (belowLeftBlock == FallingBlockType::Open)
            moveTo = belowLeftPos;
          else if (belowRightBlock == FallingBlockType::Open)
            moveTo = belowRightPos;
        }
      }

      if (moveTo) {
        stats.movedBlocks += 1;
        if (collectSignatures)
          movedBlocks.append({pos, *moveTo});
        m_facade->moveBlock(pos, *moveTo);
        if (m_random.randf() < m_immediateUpwardPropagateProbability) {
          processing.add(pos + Vec2I(0, 1));
          processing.add(pos + Vec2I(-1, 1));
          processing.add(pos + Vec2I(1, 1));
        }

        visitLocation(pos);
        visitLocation(*moveTo);
      }
    }
  }

  stats.nextPendingPositions = m_pending.size();
  if (collectSignatures) {
    stats.processedPositionSignature = positionListSignature(std::move(processedPositions));
    stats.movedBlockSignature = moveListSignature(std::move(movedBlocks));
    stats.nextPendingPositionSignature = positionListSignature(m_pending.values());
  }

  return stats;
}

void FallingBlocksAgent::visitLocation(Vec2I const& location) {
  visitRegion(RectI::withSize(location, Vec2I(1, 1)));
}

void FallingBlocksAgent::visitRegion(RectI const& region) {
  for (int x = region.xMin() - 1; x <= region.xMax(); ++x) {
    for (int y = region.yMin(); y <= region.yMax(); ++y) 
      m_pending.add({x, y});
  }
}

}
