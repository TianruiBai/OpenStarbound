#include "StarPlayerFactory.hpp"
#include "StarJsonExtra.hpp"
#include "StarPlayer.hpp"
#include "StarAssets.hpp"
#include "StarRoot.hpp"
#include "StarRootLuaBindings.hpp"
#include "StarUtilityLuaBindings.hpp"
#include "StarRebuilder.hpp"

namespace Star {

PlayerConfig::PlayerConfig(JsonObject const& cfg) {
  defaultIdentity = HumanoidIdentity(cfg.value("defaultHumanoidIdentity"));
  humanoidTiming = Humanoid::HumanoidTiming(cfg.value("humanoidTiming"));

  for (Json v : cfg.value("defaultItems", JsonArray()).toArray())
    defaultItems.append(ItemDescriptor(v));

  for (Json v : cfg.value("defaultBlueprints", JsonObject()).getArray("tier1", JsonArray()))
    defaultBlueprints.append(ItemDescriptor(v));

  metaBoundBox = jsonToRectF(cfg.get("metaBoundBox"));

  movementParameters = cfg.get("movementParameters");
  zeroGMovementParameters = cfg.get("zeroGMovementParameters");
  statusControllerSettings = cfg.get("statusControllerSettings");

  footstepTiming = cfg.get("footstepTiming").toFloat();
  footstepSensor = jsonToVec2F(cfg.get("footstepSensor"));

  underwaterSensor = jsonToVec2F(cfg.get("underwaterSensor"));
  underwaterMinWaterLevel = cfg.get("underwaterMinWaterLevel").toFloat();

  splashConfig = EntitySplashConfig(cfg.get("splashConfig"));

  companionsConfig = cfg.get("companionsConfig");

  deploymentConfig = cfg.get("deploymentConfig");

  effectsAnimator = cfg.get("effectsAnimator").toString();
  interactRadius = cfg.get("interactRadius").toFloat();
  walkIntoInteractBias = jsonToVec2F(cfg.get("walkIntoInteractBias"));
  emoteCooldown = cfg.get("emoteCooldown").toFloat();
  blinkInterval = jsonToVec2F(cfg.get("blinkInterval"));
  ageItemsEvery = cfg.get("ageItemsEvery").toFloat();
  foodLowThreshold = cfg.get("foodLowThreshold").toFloat();
#ifdef STAR_PLATFORM_N3DS
  foodLowStatusEffects = {};
  foodEmptyStatusEffects = {};
  inCinematicStatusEffects = {};
#else
  foodLowStatusEffects = cfg.get("foodLowStatusEffects").toArray().transformed(jsonToPersistentStatusEffect);
  foodEmptyStatusEffects = cfg.get("foodEmptyStatusEffects").toArray().transformed(jsonToPersistentStatusEffect);
  inCinematicStatusEffects = cfg.get("inCinematicStatusEffects").toArray().transformed(jsonToPersistentStatusEffect);
#endif

  teleportInTime = cfg.get("teleportInTime").toFloat();
  teleportOutTime = cfg.get("teleportOutTime").toFloat();

  deployInTime = cfg.get("deployInTime").toFloat();
  deployOutTime = cfg.get("deployOutTime").toFloat();

  bodyMaterialKind = cfg.get("bodyMaterialKind").toString();

#ifndef STAR_PLATFORM_N3DS
  for (auto& p : cfg.get("genericScriptContexts").optObject().value(JsonObject()))
    genericScriptContexts[p.first] = p.second.toString();
#endif
}

PlayerFactory::PlayerFactory() : m_rebuilder(make_shared<Rebuilder>("player")) {
  auto assets = Root::singleton().assets();
  m_config = make_shared<PlayerConfig>(assets->json("/player.config").toObject());
}

PlayerPtr PlayerFactory::create() const {
  return make_shared<Player>(m_config);
}

PlayerPtr PlayerFactory::diskLoadPlayer(Json const& diskStore) const {
  PlayerPtr player;
  try {
    player = make_shared<Player>(m_config, diskStore);
  } catch (std::exception const& e) {
    auto exception = std::current_exception();
    bool success = m_rebuilder->rebuild(diskStore, strf("{}", outputException(e, false)), [&](Json const& store) -> String {
      try {
        player = make_shared<Player>(m_config, store);
      } catch (std::exception const& e) {
        exception = std::current_exception();
        return strf("{}", outputException(e, false));
      }
      return {};
    });

    if (!success)
      std::rethrow_exception(exception);
  }
  return player;
}

PlayerPtr PlayerFactory::netLoadPlayer(ByteArray const& netStore, NetCompatibilityRules rules) const {
  return make_shared<Player>(m_config, netStore, rules);
}

}
