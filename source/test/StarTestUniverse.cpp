#include "StarTestUniverse.hpp"
#include "StarFile.hpp"
#include "StarQuests.hpp"
#include "StarPlayerFactory.hpp"
#include "StarPlayerStorage.hpp"
#include "StarStatistics.hpp"
#include "StarStatisticsService.hpp"
#include "StarPlayer.hpp"
#include "StarAssets.hpp"
#include "StarWorldClient.hpp"

namespace Star {

namespace {

LuaCallbacks makeNoInputCallbacks() {
  LuaCallbacks callbacks;

  callbacks.registerCallbackWithSignature<Maybe<unsigned>, String, String>("bindDown", [](String const&, String const&) -> Maybe<unsigned> { return {}; });
  callbacks.registerCallbackWithSignature<bool, String, String>("bindHeld", [](String const&, String const&) { return false; });
  callbacks.registerCallbackWithSignature<bool, String, String>("bind", [](String const&, String const&) { return false; });
  callbacks.registerCallbackWithSignature<Maybe<unsigned>, String, String>("bindUp", [](String const&, String const&) -> Maybe<unsigned> { return {}; });
  callbacks.registerCallbackWithSignature<Maybe<unsigned>, String, Maybe<StringList>>("keyDown", [](String const&, Maybe<StringList> const&) -> Maybe<unsigned> { return {}; });
  callbacks.registerCallbackWithSignature<bool, String>("keyHeld", [](String const&) { return false; });
  callbacks.registerCallbackWithSignature<bool, String>("key", [](String const&) { return false; });
  callbacks.registerCallbackWithSignature<Maybe<unsigned>, String>("keyUp", [](String const&) -> Maybe<unsigned> { return {}; });
  callbacks.registerCallbackWithSignature<Maybe<List<Vec2F>>, String>("mouseDown", [](String const&) -> Maybe<List<Vec2F>> { return {}; });
  callbacks.registerCallbackWithSignature<bool, String>("mouseHeld", [](String const&) { return false; });
  callbacks.registerCallbackWithSignature<bool, String>("mouse", [](String const&) { return false; });
  callbacks.registerCallbackWithSignature<Maybe<List<Vec2F>>, String>("mouseUp", [](String const&) -> Maybe<List<Vec2F>> { return {}; });
  callbacks.registerCallbackWithSignature<void, String, String>("resetBinds", [](String const&, String const&) {});
  callbacks.registerCallbackWithSignature<void, String, String, Json>("setBinds", [](String const&, String const&, Json const&) {});
  callbacks.registerCallbackWithSignature<Json, String, String>("getDefaultBinds", [](String const&, String const&) { return JsonObject{}; });
  callbacks.registerCallbackWithSignature<Json, String, String>("getBinds", [](String const&, String const&) { return JsonObject{}; });
  callbacks.registerCallbackWithSignature<Json>("events", []() { return JsonArray{}; });
  callbacks.registerCallbackWithSignature<Vec2F>("mousePosition", []() { return Vec2F(); });
  callbacks.registerCallbackWithSignature<unsigned, String>("getTag", [](String const&) { return 0u; });

  return callbacks;
}

}

TestUniverse::TestUniverse(Vec2U clientWindowSize) {
  auto& root = Root::singleton();

  m_clientWindowSize = clientWindowSize;

  m_storagePath = File::temporaryDirectory();
  auto playerStorage = make_shared<PlayerStorage>(File::relativeTo(m_storagePath, "player"));
  auto statistics = make_shared<Statistics>(File::relativeTo(m_storagePath, "statistics"));
  m_server = make_shared<UniverseServer>(File::relativeTo(m_storagePath, "universe"));
  m_client = make_shared<UniverseClient>(playerStorage, statistics);
  m_client->setLuaCallbacks("input", makeNoInputCallbacks());

  m_server->start();

  m_mainPlayer = root.playerFactory()->create();
  m_mainPlayer->finalizeCreation();
  m_mainPlayer->setName("test");
  m_mainPlayer->setSpecies("human");
  m_mainPlayer->setShipSpecies("human");
  m_mainPlayer->setAdmin(true);
  m_mainPlayer->setModeType(PlayerMode::Survival);
  m_client->setMainPlayer(m_mainPlayer);
  m_client->connect(m_server->addLocalClient(), "test", "");
}

TestUniverse::~TestUniverse() {
  m_client = {};
  m_server = {};
  m_mainPlayer = {};
  File::removeDirectoryRecursive(m_storagePath);
}

void TestUniverse::warpPlayer(WorldId worldId) {
  m_client->warpPlayer(WarpToWorld(worldId), true);
  while (m_mainPlayer->isTeleporting() || m_client->playerWorld().empty()) {
    m_client->update(0.016f);
    Thread::sleep(16);
  }
}

WorldId TestUniverse::currentPlayerWorld() const {
  return m_client->clientContext()->playerWorldId();
}

void TestUniverse::update(unsigned times) {
  for (unsigned i = 0; i < times; ++i) {
    m_client->update(0.016f);
    Thread::sleep(16);
  }
}

List<Drawable> TestUniverse::currentClientDrawables() {
  WorldRenderData renderData;
  auto worldClient = m_client->worldClient();
  worldClient->centerClientWindowOnPlayer(m_clientWindowSize);
  worldClient->render(renderData, 0);

  List<Drawable> drawables;
  for (auto& ed : renderData.entityDrawables) {
    for (auto& p : ed.layers)
      drawables.appendAll(std::move(p.second));
  }

  return drawables;
}

}
