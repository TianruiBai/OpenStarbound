#include "StarPlayerStorage.hpp"
#include "StarFile.hpp"
#include "StarLogging.hpp"
#include "StarIterator.hpp"
#include "StarTime.hpp"
#include "StarConfiguration.hpp"
#include "StarPlayer.hpp"
#include "StarAssets.hpp"
#include "StarEntityFactory.hpp"
#include "StarRoot.hpp"
#include "StarText.hpp"

namespace Star {

PlayerStorage::PlayerStorage(String const& storageDir) {
  m_storageDirectory = storageDir;
  m_backupDirectory = File::relativeTo(m_storageDirectory, "backup");
  if (!File::isDirectory(m_storageDirectory)) {
    Logger::info("Creating player storage directory");
    File::makeDirectory(m_storageDirectory);
    return;
  }

#ifdef STAR_PLATFORM_N3DS
  auto n3dsConfiguration = Root::singleton().configuration();
  if (n3dsConfiguration->get("clearPlayerFiles").toBool()) {
    Logger::info("Clearing all player files");
    for (auto file : File::dirList(m_storageDirectory)) {
      if (!file.second)
        File::remove(File::relativeTo(m_storageDirectory, file.first));
    }
  } else {
    for (auto file : File::dirList(m_storageDirectory)) {
      if (file.second || !file.first.endsWith(".player"))
        continue;

      auto fileName = file.first.rsplit('.', 1).at(0);
      try {
        Uuid uuid(fileName);
        if (m_playerFileNames.insert(uuid, fileName))
          m_savedPlayersCache[uuid] = Json();
      } catch (std::exception const& e) {
        Logger::error("N3DS PlayerStorage: ignoring player file with invalid uuid {} : {}", file.first, outputException(e, false));
      }
    }
  }

  try {
    String filename = File::relativeTo(m_storageDirectory, "metadata");
    m_metadata = Json::parseJson(File::readFileString(filename)).toObject();

    if (auto order = m_metadata.value("order")) {
      for (auto const& jUuid : order.iterateArray()) {
        auto entry = m_savedPlayersCache.find(Uuid(jUuid.toString()));
        if (entry != m_savedPlayersCache.end())
          m_savedPlayersCache.toBack(entry);
      }
    }
  } catch (std::exception const& e) {
    Logger::warn("N3DS PlayerStorage: error loading player storage metadata file, resetting: {}", outputException(e, false));
  }

  Logger::info("N3DS PlayerStorage: indexed {} player file(s) for lazy load", m_savedPlayersCache.size());
  return;
#endif

  auto configuration = Root::singleton().configuration();
  if (configuration->get("clearPlayerFiles").toBool()) {
    Logger::info("Clearing all player files");
    for (auto file : File::dirList(m_storageDirectory)) {
      if (!file.second)
        File::remove(File::relativeTo(m_storageDirectory, file.first));
    }
  } else {
    auto versioningDatabase = Root::singleton().versioningDatabase();
    auto entityFactory = Root::singleton().entityFactory();

    for (auto file : File::dirList(m_storageDirectory)) {
      if (file.second)
        continue;

      String filename = File::relativeTo(m_storageDirectory, file.first);
      if (filename.endsWith(".player")) {
        try {
          auto json = VersionedJson::readFile(filename);
          Uuid uuid(json.content.getString("uuid"));
          if (m_playerFileNames.insert(uuid, file.first.rsplit('.', 1).at(0))) {
            auto& playerCacheData = m_savedPlayersCache[uuid];
            playerCacheData = entityFactory->loadVersionedJson(json, EntityType::Player);
          } else {
            Logger::warn("Duplicate player? Skipping player file {} because it has the same UUID as {}.player ({})", file.first, m_playerFileNames.getRight(uuid), uuid.hex());
          }
        } catch (std::exception const& e) {
          Logger::error("Error loading player file, ignoring! {} : {}", filename, outputException(e, false));
        }
      }
    }

    // Remove all the player entries that are missing player data or fail to
    // load.
    auto it = makeSMutableMapIterator(m_savedPlayersCache);
    while (it.hasNext()) {
      auto& entry = it.next();
      if (entry.second.isNull()) {
        it.remove();
      } else {
        try {
          auto entityFactory = Root::singleton().entityFactory();
          auto player = as<Player>(entityFactory->diskLoadEntity(EntityType::Player, entry.second));
          if (player->uuid() != entry.first)
            throw PlayerException(strf("Uuid mismatch in loaded player with filename uuid '{}'", entry.first.hex()));
        } catch (StarException const& e) {
          auto& fileName = uuidFileName(entry.first);
          String uuidHex = entry.first.hex();
          if (uuidHex == fileName)
            Logger::error("Failed to validate player with uuid {} : {}", uuidHex, outputException(e, true));
          else
            Logger::error("Failed to validate player with uuid {} ({}.player) : {}", uuidHex, fileName, outputException(e, true));
          it.remove();
        }
      }
    }
  }

  try {
    String filename = File::relativeTo(m_storageDirectory, "metadata");
    m_metadata = Json::parseJson(File::readFileString(filename)).toObject();

    if (auto order = m_metadata.value("order")) {
      for (auto const& jUuid : order.iterateArray()) {
        auto entry = m_savedPlayersCache.find(Uuid(jUuid.toString()));
        if (entry != m_savedPlayersCache.end())
          m_savedPlayersCache.toBack(entry);
      }
    }
  } catch (std::exception const& e) {
    Logger::warn("Error loading player storage metadata file, resetting: {}", outputException(e, false));
  }
}

PlayerStorage::~PlayerStorage() {
  writeMetadata();
}

size_t PlayerStorage::playerCount() const {
  RecursiveMutexLocker locker(m_mutex);
  return m_savedPlayersCache.size();
}

Maybe<Uuid> PlayerStorage::playerUuidAt(size_t index) {
  RecursiveMutexLocker locker(m_mutex);
  if (index < m_savedPlayersCache.size())
    return m_savedPlayersCache.keyAt(index);
  else
    return {};
}

Maybe<Uuid> PlayerStorage::playerUuidByName(String const& name, Maybe<Uuid> except) {
  String cleanMatch = Text::stripEscapeCodes(name).toLower();
  Maybe<Uuid> uuid;

  RecursiveMutexLocker locker(m_mutex);

  size_t longest = std::numeric_limits<size_t>::max();
  for (auto& cache : m_savedPlayersCache) {
    if (except && *except == cache.first)
      continue;
#ifdef STAR_PLATFORM_N3DS
    else if (cache.second.isNull() && cleanMatch.empty())
      return cache.first;
#endif
    else if (auto name = cache.second.optQueryString("identity.name")) {
      auto cleanName = Text::stripEscapeCodes(*name).toLower();
      auto len = cleanName.size();
      if (len < longest && cleanName.utf8().rfind(cleanMatch.utf8()) == 0) {
        longest = len;
        uuid = cache.first;
      }
    }
  }

  return uuid;
}

List<Uuid> PlayerStorage::playerUuidListByName(String const& name, Maybe<Uuid> except) {
  String cleanMatch = Text::stripEscapeCodes(name).toLower();
  List<Uuid> list = {};

  RecursiveMutexLocker locker(m_mutex);

  for (auto& cache : m_savedPlayersCache) {
    if (except && *except == cache.first)
      continue;
#ifdef STAR_PLATFORM_N3DS
    else if (cache.second.isNull() && cleanMatch.empty())
      list.append(cache.first);
#endif
    else if (auto name = cache.second.optQueryString("identity.name")) {
      auto cleanName = Text::stripEscapeCodes(*name).toLower();
      if (cleanMatch == "" || cleanName.utf8().rfind(cleanMatch.utf8()) != NPos) {
        list.append(cache.first);
      }
    }
  }

  return list;
}


Json PlayerStorage::savePlayer(PlayerPtr const& player) {
  auto entityFactory = Root::singleton().entityFactory();
  auto versioningDatabase = Root::singleton().versioningDatabase();

  RecursiveMutexLocker locker(m_mutex);

  auto uuid = player->uuid();

  auto& playerCacheData = m_savedPlayersCache[uuid];
  if (!m_playerFileNames.hasLeftValue(uuid))
    m_playerFileNames.insert(uuid, uuid.hex());
  auto newPlayerData = player->diskStore();
  if (playerCacheData != newPlayerData) {
    playerCacheData = newPlayerData;
    VersionedJson versionedJson = entityFactory->storeVersionedJson(EntityType::Player, playerCacheData);
    auto fileName = strf("{}.player", uuidFileName(uuid));
    VersionedJson::writeFile(versionedJson, File::relativeTo(m_storageDirectory, fileName));
    Logger::debug("Saved player {} to {}", Text::stripEscapeCodes(player->name()), fileName);
  }
  return newPlayerData;
}

Maybe<Json> PlayerStorage::maybeGetPlayerData(Uuid const& uuid) {
  RecursiveMutexLocker locker(m_mutex);
  if (auto cache = m_savedPlayersCache.ptr(uuid)) {
#ifdef STAR_PLATFORM_N3DS
    if (cache->isNull()) {
      try {
        String filename = File::relativeTo(m_storageDirectory, strf("{}.player", uuidFileName(uuid)));
        if (!File::exists(filename))
          return {};

        auto entityFactory = Root::singleton().entityFactory();
        *cache = entityFactory->loadVersionedJson(VersionedJson::readFile(filename), EntityType::Player);
        Logger::info("N3DS PlayerStorage: lazily loaded player {}", uuid.hex());
      } catch (std::exception const& e) {
        Logger::error("N3DS PlayerStorage: error loading player {}, ignoring! {}", uuid.hex(), outputException(e, false));
        m_savedPlayersCache.remove(uuid);
        return {};
      }
    }
#endif
    return *cache;
  } else
    return {};
}

Json PlayerStorage::getPlayerData(Uuid const& uuid) {
  auto data = maybeGetPlayerData(uuid);
  if (!data)
    throw PlayerException(strf("No such stored player with uuid '{}'", uuid.hex()));
  else
    return *data;
}

PlayerPtr PlayerStorage::loadPlayer(Uuid const& uuid) {
  auto playerCacheData = getPlayerData(uuid);
  auto entityFactory = Root::singleton().entityFactory();
  try {
    auto player = convert<Player>(entityFactory->diskLoadEntity(EntityType::Player, playerCacheData));
    if (player->uuid() != uuid)
      throw PlayerException(strf("Uuid mismatch in loaded player with filename uuid '{}'", uuid.hex()));
    return player;
  } catch (std::exception const& e) {
    Logger::error("Error loading player file, ignoring! {}", outputException(e, false));
    RecursiveMutexLocker locker(m_mutex);
    m_savedPlayersCache.remove(uuid);
    return {};
  }
}

void PlayerStorage::deletePlayer(Uuid const& uuid) {
  RecursiveMutexLocker locker(m_mutex);
  if (!m_savedPlayersCache.contains(uuid))
    throw PlayerException(strf("No such stored player with uuid '{}'", uuid.hex()));

  m_savedPlayersCache.remove(uuid);

  auto uuidHex = uuid.hex();
  auto storagePrefix = File::relativeTo(m_storageDirectory, uuidHex);
  auto backupPrefix = File::relativeTo(m_backupDirectory, uuidHex);

  auto removeIfExists = [](String const& prefix, String const& suffix) {
    if (File::exists(prefix + suffix)) {
      File::remove(prefix + suffix);
    }
  };

  removeIfExists(storagePrefix, ".player");
  removeIfExists(storagePrefix, ".shipworld");

  auto configuration = Root::singleton().configuration();
  unsigned playerBackupFileCount = configuration->get("playerBackupFileCount").toUInt();

  for (unsigned i = 1; i <= playerBackupFileCount; ++i) {
    removeIfExists(backupPrefix, strf(".player.bak{}", i));
    removeIfExists(backupPrefix, strf(".shipworld.bak{}", i));
  }
}

WorldChunks PlayerStorage::loadShipData(Uuid const& uuid) {
  RecursiveMutexLocker locker(m_mutex);
  if (!m_savedPlayersCache.contains(uuid)) {
#ifdef STAR_PLATFORM_N3DS
    Logger::info("N3DS PlayerStorage: using empty ship data for unsaved player {}", uuid.hex());
    return {};
#else
    throw PlayerException(strf("No such stored player with uuid '{}'", uuid.hex()));
#endif
  }

  String filename = File::relativeTo(m_storageDirectory, strf("{}.shipworld", uuidFileName(uuid)));
  try {
    if (File::exists(filename))
      return WorldStorage::getWorldChunksFromFile(filename);
  } catch (StarException const& e) {
    Logger::error("Failed to load shipworld file, removing {} : {}", filename, outputException(e, false));
    File::remove(filename);
  }

  return {};
}

void PlayerStorage::applyShipUpdates(Uuid const& uuid, WorldChunks const& updates) {
  RecursiveMutexLocker locker(m_mutex);
  if (!m_savedPlayersCache.contains(uuid))
    throw PlayerException(strf("No such stored player with uuid '{}'", uuid.hex()));

  if (updates.empty())
    return;
  String filePath = File::relativeTo(m_storageDirectory, strf("{}.shipworld", uuidFileName(uuid)));
  WorldStorage::applyWorldChunksUpdateToFile(filePath, updates);
}

void PlayerStorage::moveToFront(Uuid const& uuid) {
  m_savedPlayersCache.toFront(uuid);
  writeMetadata();
}

void PlayerStorage::backupCycle(Uuid const& uuid) {
  RecursiveMutexLocker locker(m_mutex);

  auto configuration = Root::singleton().configuration();
  unsigned playerBackupFileCount = configuration->get("playerBackupFileCount").toUInt();
  auto& fileName = uuidFileName(uuid);

  auto path = [&](String const& dir, String const& extension) {
    return File::relativeTo(dir, strf("{}.{}", fileName, extension));
  };

  if (!File::isDirectory(m_backupDirectory))
    File::makeDirectory(m_backupDirectory);

  File::backupFileInSequence(path(m_storageDirectory, "player"), path(m_backupDirectory, "player"), playerBackupFileCount, ".bak");
  File::backupFileInSequence(path(m_storageDirectory, "shipworld"), path(m_backupDirectory, "shipworld"), playerBackupFileCount, ".bak");
  File::backupFileInSequence(path(m_storageDirectory, "metadata"), path(m_backupDirectory, "metadata"), playerBackupFileCount, ".bak");
}

void PlayerStorage::setMetadata(String key, Json value) {
  auto& val = m_metadata[std::move(key)];
  if (val != value) {
    val = std::move(value);
    writeMetadata();
  }
}

Json PlayerStorage::getMetadata(String const& key) {
  return m_metadata.value(key);
}

String const& PlayerStorage::uuidFileName(Uuid const& uuid) {
  if (auto fileName = m_playerFileNames.rightPtr(uuid))
    return *fileName;
  else {
    m_playerFileNames.insert(uuid, uuid.hex());
    return *m_playerFileNames.rightPtr(uuid);
  }
}

void PlayerStorage::writeMetadata() {
  JsonArray order;
  for (auto const& p : m_savedPlayersCache)
    order.append(p.first.hex());

  m_metadata["order"] = std::move(order);

  String filename = File::relativeTo(m_storageDirectory, "metadata");
#ifdef STAR_PLATFORM_N3DS
  File::writeFile(Json(m_metadata).printJson(0), filename);
#else
  File::overwriteFileWithRename(Json(m_metadata).printJson(0), filename);
#endif
}

}
