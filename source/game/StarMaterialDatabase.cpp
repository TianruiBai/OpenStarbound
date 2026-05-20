#include "StarMaterialDatabase.hpp"
#include "StarJsonExtra.hpp"
#include "StarFormat.hpp"
#include "StarAssets.hpp"
#include "StarRoot.hpp"
#include "StarLogging.hpp"
#include "StarParticleDatabase.hpp"

namespace Star {

MaterialDatabase::MaterialDatabase() {
  m_metaModIndex = {
      {"metamod:none", NoModId},
      {"metamod:biome", BiomeModId},
      {"metamod:undergroundbiome", UndergroundBiomeModId}};

  auto assets = Root::singleton().assets();
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS MaterialDatabase: skipped mining particle database preload");
#else
  auto pdb = Root::singleton().particleDatabase();
#endif

  setMetaMaterial(EmptyMaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:empty", EmptyMaterialId, CollisionKind::None, false});
  setMetaMaterial(NullMaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:null", NullMaterialId, CollisionKind::Block, true});
  setMetaMaterial(StructureMaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:structure", StructureMaterialId, CollisionKind::Block, true});
  setMetaMaterial(BiomeMaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:biome", BiomeMaterialId, CollisionKind::Block, true});
  setMetaMaterial(Biome1MaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:biome1", Biome1MaterialId, CollisionKind::Block, true});
  setMetaMaterial(Biome2MaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:biome2", Biome2MaterialId, CollisionKind::Block, true});
  setMetaMaterial(Biome3MaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:biome3", Biome3MaterialId, CollisionKind::Block, true});
  setMetaMaterial(Biome4MaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:biome4", Biome4MaterialId, CollisionKind::Block, true});
  setMetaMaterial(Biome5MaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:biome5", Biome5MaterialId, CollisionKind::Block, true});
  setMetaMaterial(BoundaryMaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:boundary", BoundaryMaterialId, CollisionKind::Slippery, true});
  setMetaMaterial(ObjectSolidMaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:objectsolid", ObjectSolidMaterialId, CollisionKind::Block, true});
  setMetaMaterial(ObjectPlatformMaterialId, MaterialDatabase::MetaMaterialInfo{"metamaterial:objectplatform", ObjectPlatformMaterialId, CollisionKind::Platform, false});

  auto metaMaterialConfig = assets->json("/metamaterials.config");
  for (auto metaMaterial : metaMaterialConfig.iterateArray()) {
    auto matName = "metamaterial:" + metaMaterial.getString("name");
    if (isMaterialName(matName)) {
      Logger::info("Metamaterial '{}' has duplicate material name!", matName);
      continue;
    }

    MaterialId matId = metaMaterial.getUInt("materialId");
    if (isRealMaterial(matId) || matId >= FirstEngineMetaMaterialId) {
      Logger::info("Material id {} for metamaterial '{}' does not fall within the valid range!", matId, matName);
      continue;
    } else if (containsMetaMaterial(matId)) {
      Logger::info("Material id {} for metamaterial '{}' conflicts with another metamaterial id!", matId, matName);
      continue;
    }

    auto matCollisionKind = CollisionKindNames.getLeft(metaMaterial.getString("collisionKind"));

    bool blocksLiquidFlow = metaMaterial.getBool("blocksLiquidFlow", isSolidColliding(matCollisionKind));

    setMetaMaterial(matId, MaterialDatabase::MetaMaterialInfo{matName, matId, matCollisionKind, blocksLiquidFlow});
  }

  auto& materials = assets->scanExtension("material");
  auto& mods = assets->scanExtension("matmod");

#ifdef STAR_PLATFORM_N3DS
  m_lazyMaterialFiles = materials.values();
  m_lazyModFiles = mods.values();
  m_defaultFootstepSound = assets->json("/client.config:defaultFootstepSound").toString();
  Logger::info("N3DS MaterialDatabase: indexed {} material files and {} mod files for lazy loading", m_lazyMaterialFiles.size(), m_lazyModFiles.size());
  return;
#endif

  assets->queueJsons(materials);
  assets->queueJsons(mods);

  for (auto& file : materials) {
    try {
      auto matConfig = assets->json(file);

      MaterialInfo material;

      auto materialId = matConfig.getInt("materialId");
      material.id = materialId;

      material.name = matConfig.getString("materialName");
      material.path = file;
      material.config = matConfig;

      material.itemDrop = matConfig.getString("itemDrop", "");

      JsonObject descriptions;
      for (auto entry : matConfig.iterateObject())
        if (entry.first.endsWith("Description"))
          descriptions[entry.first] = entry.second;
      descriptions["description"] = matConfig.getString("description", "");
      descriptions["shortdescription"] = matConfig.getString("shortdescription", "");
      material.descriptions = descriptions;

      material.category = matConfig.getString("category");

      material.particleColor = jsonToColor(matConfig.get("particleColor", JsonArray{0, 0, 0, 255}));
#ifndef STAR_PLATFORM_N3DS
      if (matConfig.contains("miningParticle"))
        material.miningParticle = pdb->config(matConfig.getString("miningParticle"));
#endif
      if (matConfig.contains("miningSounds"))
        material.miningSounds = transform<StringList>(
            jsonToStringList(matConfig.get("miningSounds")), bind(AssetPath::relativeTo, file, _1));
      if (matConfig.contains("footstepSound"))
        material.footstepSound = AssetPath::relativeTo(file, matConfig.getString("footstepSound"));

      material.tillableMod = matConfig.getInt("tillableMod", NoModId);
      material.soil = matConfig.getBool("soil", false);
      material.falling = matConfig.getBool("falling", false);
      material.cascading = matConfig.getBool("cascading", false);

#ifdef STAR_PLATFORM_N3DS
      if (matConfig.contains("renderTemplate")) {
        static bool loggedDeferredMaterialProfiles = false;
        if (!loggedDeferredMaterialProfiles) {
          Logger::info("N3DS MaterialDatabase: deferring material render profile parsing");
          loggedDeferredMaterialProfiles = true;
        }
      }
#else
      if (matConfig.contains("renderTemplate")) {
        auto renderTemplate = assets->fetchJson(matConfig.get("renderTemplate"), file);
        auto renderParameters = matConfig.get("renderParameters");
        material.materialRenderProfile = make_shared<MaterialRenderProfile>(parseMaterialRenderProfile(jsonMerge(renderTemplate, renderParameters), file));
      }
#endif

      material.damageParameters =
          TileDamageParameters(assets->fetchJson(matConfig.get("damageTable", "/tiles/defaultDamage.config")),
              matConfig.optFloat("health"),
              matConfig.optUInt("requiredHarvestLevel"));

      material.collisionKind = CollisionKindNames.getLeft(matConfig.getString("collisionKind", "block"));
      material.foregroundOnly = matConfig.getBool("foregroundOnly", false);
      material.supportsMods = matConfig.getBool("supportsMods", !(material.falling || material.cascading || material.collisionKind != CollisionKind::Block));

      material.blocksLiquidFlow = matConfig.getBool("blocksLiquidFlow", isSolidColliding(material.collisionKind));

      if (material.id != materialId || !isRealMaterial(material.id))
        throw MaterialException(strf("Material id {} does not fall in the valid range\n", materialId));

      if (containsMaterial(material.id))
        throw MaterialException(strf("Duplicate material id {} found for material {}", material.id, material.name));

      if (isMaterialName(material.name))
        throw MaterialException(strf("Duplicate material name '{}' found", material.name));

      setMaterial(material.id, material);

      for (auto liquidInteraction : matConfig.getArray("liquidInteractions", {})) {
        LiquidId liquidId = liquidInteraction.getUInt("liquidId");
        LiquidMaterialInteraction interaction;
        interaction.consumeLiquid = liquidInteraction.getFloat("consumeLiquid", 0.0f);
        interaction.transformTo = liquidInteraction.getUInt("transformMaterialId", NullMaterialId);
        interaction.topOnly = liquidInteraction.getBool("topOnly", false);
        m_liquidMaterialInteractions[{liquidId, material.id}] = interaction;
      }
    } catch (StarException const& e) {
      throw MaterialException(strf("Error loading material file {}", file), e);
    }
  }

  for (auto& file : mods) {
    try {
      auto modConfig = assets->json(file);

      ModInfo mod;

      auto modId = modConfig.getInt("modId");
      mod.id = modId;

      mod.name = modConfig.getString("modName");
      mod.path = file;
      mod.config = modConfig;

      mod.itemDrop = modConfig.getString("itemDrop", "");

      JsonObject descriptions;
      for (auto entry : modConfig.iterateObject())
        if (entry.first.endsWith("Description"))
          descriptions[entry.first] = entry.second;
      descriptions["description"] = modConfig.getString("description", "");
      mod.descriptions = descriptions;

      mod.particleColor = jsonToColor(modConfig.get("particleColor", JsonArray{0, 0, 0, 255}));
#ifndef STAR_PLATFORM_N3DS
      if (modConfig.contains("miningParticle"))
        mod.miningParticle = pdb->config(modConfig.getString("miningParticle"));
#endif
      if (modConfig.contains("miningSounds"))
        mod.miningSounds = transform<StringList>(
            jsonToStringList(modConfig.get("miningSounds")), bind(AssetPath::relativeTo, file, _1));
      if (modConfig.contains("footstepSound"))
        mod.footstepSound = AssetPath::relativeTo(file, modConfig.getString("footstepSound"));

      mod.tilled = modConfig.getBool("tilled", false);

      mod.breaksWithTile = modConfig.getBool("breaksWithTile", false);

#ifdef STAR_PLATFORM_N3DS
      if (modConfig.contains("renderTemplate")) {
        static bool loggedDeferredModProfiles = false;
        if (!loggedDeferredModProfiles) {
          Logger::info("N3DS MaterialDatabase: deferring mod render profile parsing");
          loggedDeferredModProfiles = true;
        }
      }
#else
      if (modConfig.contains("renderTemplate")) {
        auto renderTemplate = assets->fetchJson(modConfig.get("renderTemplate"));
        auto renderParameters = modConfig.get("renderParameters");
        mod.modRenderProfile = make_shared<MaterialRenderProfile>(parseMaterialRenderProfile(jsonMerge(renderTemplate, renderParameters), file));
      }
#endif

      mod.damageParameters =
          TileDamageParameters(assets->fetchJson(modConfig.get("damageTable", "/tiles/defaultDamage.config")),
              modConfig.optFloat("health"),
              modConfig.optUInt("harvestLevel"));

      if (modId != mod.id || !isRealMod(mod.id))
        throw MaterialException(strf("Mod id {} does not fall in the valid range\n", mod.id));

      if (containsMod(mod.id))
        throw MaterialException(strf("Duplicate mod id {} found for mod {}", mod.id, mod.name));

      if (m_modIndex.contains(mod.name) || m_metaModIndex.hasLeftValue(mod.name))
        throw MaterialException(strf("Duplicate mod name '{}' found", mod.name));

      setMod(mod.id, mod);
      m_modIndex[mod.name] = mod.id;

      for (auto liquidInteraction : modConfig.getArray("liquidInteractions", {})) {
        LiquidId liquidId = liquidInteraction.getUInt("liquidId");
        LiquidModInteraction interaction;
        interaction.consumeLiquid = liquidInteraction.getFloat("consumeLiquid", 0.0f);
        interaction.transformTo = liquidInteraction.getUInt("transformModId", NoModId);
        interaction.topOnly = liquidInteraction.getBool("topOnly", false);
        m_liquidModInteractions[{liquidId, mod.id}] = interaction;
      }
    } catch (StarException const& e) {
      throw MaterialException(strf("Error loading mod file {}", file), e);
    }
  }

  m_defaultFootstepSound = assets->json("/client.config:defaultFootstepSound").toString();
}

bool MaterialDatabase::lazyLoadMaterialFile(String const& file, Maybe<String> const& wantedName, Maybe<MaterialId> const& wantedId) const {
  auto assets = Root::singleton().assets();
  auto matConfig = assets->json(file);
  MaterialId materialId = matConfig.getInt("materialId");
  String materialName = matConfig.getString("materialName");
  if ((wantedName && materialName != wantedName.value()) || (wantedId && materialId != wantedId.value()))
    return false;

  if (containsMaterial(materialId) || m_materialIndex.contains(materialName))
    return true;

  MaterialInfo material;
  material.id = materialId;
  material.name = materialName;
  material.path = file;
  material.config = matConfig;
  material.itemDrop = matConfig.getString("itemDrop", "");

  JsonObject descriptions;
  for (auto entry : matConfig.iterateObject())
    if (entry.first.endsWith("Description"))
      descriptions[entry.first] = entry.second;
  descriptions["description"] = matConfig.getString("description", "");
  descriptions["shortdescription"] = matConfig.getString("shortdescription", "");
  material.descriptions = descriptions;

  material.category = matConfig.getString("category");
  material.particleColor = jsonToColor(matConfig.get("particleColor", JsonArray{0, 0, 0, 255}));
  if (matConfig.contains("miningSounds"))
    material.miningSounds = transform<StringList>(
        jsonToStringList(matConfig.get("miningSounds")), bind(AssetPath::relativeTo, file, _1));
  if (matConfig.contains("footstepSound"))
    material.footstepSound = AssetPath::relativeTo(file, matConfig.getString("footstepSound"));

  material.tillableMod = matConfig.getInt("tillableMod", NoModId);
  material.soil = matConfig.getBool("soil", false);
  material.falling = matConfig.getBool("falling", false);
  material.cascading = matConfig.getBool("cascading", false);
  material.damageParameters =
      TileDamageParameters(assets->fetchJson(matConfig.get("damageTable", "/tiles/defaultDamage.config")),
          matConfig.optFloat("health"),
          matConfig.optUInt("requiredHarvestLevel"));
  material.collisionKind = CollisionKindNames.getLeft(matConfig.getString("collisionKind", "block"));
  material.foregroundOnly = matConfig.getBool("foregroundOnly", false);
  material.supportsMods = matConfig.getBool("supportsMods", !(material.falling || material.cascading || material.collisionKind != CollisionKind::Block));
  material.blocksLiquidFlow = matConfig.getBool("blocksLiquidFlow", isSolidColliding(material.collisionKind));

  const_cast<MaterialDatabase*>(this)->setMaterial(material.id, std::move(material));

  for (auto liquidInteraction : matConfig.getArray("liquidInteractions", {})) {
    LiquidId liquidId = liquidInteraction.getUInt("liquidId");
    LiquidMaterialInteraction interaction;
    interaction.consumeLiquid = liquidInteraction.getFloat("consumeLiquid", 0.0f);
    interaction.transformTo = liquidInteraction.getUInt("transformMaterialId", NullMaterialId);
    interaction.topOnly = liquidInteraction.getBool("topOnly", false);
    const_cast<MaterialDatabase*>(this)->m_liquidMaterialInteractions[{liquidId, materialId}] = interaction;
  }

#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS MaterialDatabase: lazily indexed material '{}' ({})", materialName, materialId);
#endif
  return true;
}

bool MaterialDatabase::lazyLoadModFile(String const& file, Maybe<String> const& wantedName, Maybe<ModId> const& wantedId) const {
  auto assets = Root::singleton().assets();
  auto modConfig = assets->json(file);
  ModId modId = modConfig.getInt("modId");
  String modName = modConfig.getString("modName");
  if ((wantedName && modName != wantedName.value()) || (wantedId && modId != wantedId.value()))
    return false;

  if (containsMod(modId) || m_modIndex.contains(modName))
    return true;

  ModInfo mod;
  mod.id = modId;
  mod.name = modName;
  mod.path = file;
  mod.config = modConfig;
  mod.itemDrop = modConfig.getString("itemDrop", "");

  JsonObject descriptions;
  for (auto entry : modConfig.iterateObject())
    if (entry.first.endsWith("Description"))
      descriptions[entry.first] = entry.second;
  descriptions["description"] = modConfig.getString("description", "");
  mod.descriptions = descriptions;

  mod.particleColor = jsonToColor(modConfig.get("particleColor", JsonArray{0, 0, 0, 255}));
  if (modConfig.contains("miningSounds"))
    mod.miningSounds = transform<StringList>(
        jsonToStringList(modConfig.get("miningSounds")), bind(AssetPath::relativeTo, file, _1));
  if (modConfig.contains("footstepSound"))
    mod.footstepSound = AssetPath::relativeTo(file, modConfig.getString("footstepSound"));

  mod.tilled = modConfig.getBool("tilled", false);
  mod.breaksWithTile = modConfig.getBool("breaksWithTile", false);
  mod.damageParameters =
      TileDamageParameters(assets->fetchJson(modConfig.get("damageTable", "/tiles/defaultDamage.config")),
          modConfig.optFloat("health"),
          modConfig.optUInt("harvestLevel"));

  const_cast<MaterialDatabase*>(this)->setMod(mod.id, std::move(mod));

  for (auto liquidInteraction : modConfig.getArray("liquidInteractions", {})) {
    LiquidId liquidId = liquidInteraction.getUInt("liquidId");
    LiquidModInteraction interaction;
    interaction.consumeLiquid = liquidInteraction.getFloat("consumeLiquid", 0.0f);
    interaction.transformTo = liquidInteraction.getUInt("transformModId", NoModId);
    interaction.topOnly = liquidInteraction.getBool("topOnly", false);
    const_cast<MaterialDatabase*>(this)->m_liquidModInteractions[{liquidId, modId}] = interaction;
  }

#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS MaterialDatabase: lazily indexed mod '{}' ({})", modName, modId);
#endif
  return true;
}

void MaterialDatabase::lazyLoadMaterialByName(String const& materialName) const {
#ifdef STAR_PLATFORM_N3DS
  String suffix = strf("/{}.material", materialName);
  for (auto const& file : m_lazyMaterialFiles) {
    if (file.endsWith(suffix)) {
      try {
        if (lazyLoadMaterialFile(file, materialName, {}))
          return;
      } catch (std::exception const& e) {
        Logger::warn("N3DS MaterialDatabase: failed direct material load {}: {}", file, e.what());
      }
    }
  }
#endif
  for (auto const& file : m_lazyMaterialFiles) {
    try {
      if (lazyLoadMaterialFile(file, materialName, {}))
        return;
    } catch (std::exception const& e) {
      Logger::warn("N3DS MaterialDatabase: failed checking material file {}: {}", file, e.what());
    }
  }
}

void MaterialDatabase::lazyLoadMaterialById(MaterialId materialId) const {
  for (auto const& file : m_lazyMaterialFiles) {
    try {
      if (lazyLoadMaterialFile(file, {}, materialId))
        return;
    } catch (std::exception const& e) {
      Logger::warn("N3DS MaterialDatabase: failed checking material file {}: {}", file, e.what());
    }
  }
}

void MaterialDatabase::lazyLoadModByName(String const& modName) const {
#ifdef STAR_PLATFORM_N3DS
  String suffix = strf("/{}.matmod", modName);
  for (auto const& file : m_lazyModFiles) {
    if (file.endsWith(suffix)) {
      try {
        if (lazyLoadModFile(file, modName, {}))
          return;
      } catch (std::exception const& e) {
        Logger::warn("N3DS MaterialDatabase: failed direct mod load {}: {}", file, e.what());
      }
    }
  }
#endif
  for (auto const& file : m_lazyModFiles) {
    try {
      if (lazyLoadModFile(file, modName, {}))
        return;
    } catch (std::exception const& e) {
      Logger::warn("N3DS MaterialDatabase: failed checking mod file {}: {}", file, e.what());
    }
  }
}

void MaterialDatabase::lazyLoadModById(ModId modId) const {
  for (auto const& file : m_lazyModFiles) {
    try {
      if (lazyLoadModFile(file, {}, modId))
        return;
    } catch (std::exception const& e) {
      Logger::warn("N3DS MaterialDatabase: failed checking mod file {}: {}", file, e.what());
    }
  }
}

StringList MaterialDatabase::materialNames() const {
  StringList names = m_materialIndex.keys();
  names.appendAll(m_metaMaterialIndex.keys());
  return names;
}

bool MaterialDatabase::isMetaMaterialName(String const& name) const {
  return m_metaMaterialIndex.contains(name);
}

bool MaterialDatabase::isMaterialName(String const& name) const {
#ifdef STAR_PLATFORM_N3DS
  if (!m_materialIndex.contains(name) && !m_metaMaterialIndex.contains(name))
    lazyLoadMaterialByName(name);
#endif
  return m_materialIndex.contains(name) || m_metaMaterialIndex.contains(name);
}

bool MaterialDatabase::isValidMaterialId(MaterialId material) const {
  if (isRealMaterial(material)) {
#ifdef STAR_PLATFORM_N3DS
    if (!containsMaterial(material))
      lazyLoadMaterialById(material);
#endif
    return containsMaterial(material);
  } else {
    return containsMetaMaterial(material);
  }
}

MaterialId MaterialDatabase::materialId(String const& matName) const {
  if (auto m = m_metaMaterialIndex.maybe(matName))
    return *m;
  else {
#ifdef STAR_PLATFORM_N3DS
    if (!m_materialIndex.contains(matName))
      lazyLoadMaterialByName(matName);
#endif
    return m_materialIndex.get(matName);
  }
}

String MaterialDatabase::materialName(MaterialId materialId) const {
  if (isRealMaterial(materialId))
    return getMaterialInfo(materialId)->name;
  else
    return getMetaMaterialInfo(materialId)->name;
}

Maybe<String> MaterialDatabase::materialPath(MaterialId materialId) const {
  if (isRealMaterial(materialId))
    return getMaterialInfo(materialId)->path;
  else
    return {};
}

Maybe<Json> MaterialDatabase::materialConfig(MaterialId materialId) const {
  if (isRealMaterial(materialId))
    return getMaterialInfo(materialId)->config;
  else
    return {};
}

String MaterialDatabase::materialDescription(MaterialId materialNumber, String const& species) const {
  auto material = getMaterialInfo(materialNumber);
  return material->descriptions.getString(
      strf("{}Description", species), material->descriptions.getString("description"));
}

String MaterialDatabase::materialDescription(MaterialId materialNumber) const {
  auto material = getMaterialInfo(materialNumber);
  return material->descriptions.getString("description");
}

String MaterialDatabase::materialShortDescription(MaterialId materialNumber) const {
  auto material = getMaterialInfo(materialNumber);
  return material->descriptions.getString("shortdescription");
}

String MaterialDatabase::materialCategory(MaterialId materialNumber) const {
  auto material = getMaterialInfo(materialNumber);
  return material->category;
}

StringList MaterialDatabase::modNames() const {
  StringList modNames = m_modIndex.keys();
  modNames.appendAll(m_metaModIndex.leftValues());
  return modNames;
}

bool MaterialDatabase::isModName(String const& name) const {
#ifdef STAR_PLATFORM_N3DS
  if (!m_modIndex.contains(name) && !m_metaModIndex.hasLeftValue(name))
    lazyLoadModByName(name);
#endif
  return m_modIndex.contains(name);
}

bool MaterialDatabase::isValidModId(ModId mod) const {
  if (isRealMod(mod)) {
#ifdef STAR_PLATFORM_N3DS
    if (!containsMod(mod))
      lazyLoadModById(mod);
#endif
    return mod < m_mods.size() && (bool)m_mods[mod];
  } else {
    return m_metaModIndex.hasRightValue(mod);
  }
}

ModId MaterialDatabase::modId(String const& modName) const {
  if (auto m = m_metaModIndex.maybeRight(modName))
    return *m;
  else {
#ifdef STAR_PLATFORM_N3DS
    if (!m_modIndex.contains(modName))
      lazyLoadModByName(modName);
#endif
    return m_modIndex.get(modName);
  }
}

String const& MaterialDatabase::modName(ModId mod) const {
  if (isRealMod(mod))
    return getModInfo(mod)->name;
  else
    return m_metaModIndex.getLeft(mod);
}

Maybe<String> MaterialDatabase::modPath(ModId mod) const {
  if (isRealMod(mod))
    return getModInfo(mod)->path;
  else
    return {};
}

Maybe<Json> MaterialDatabase::modConfig(ModId mod) const {
  if (isRealMod(mod))
    return getModInfo(mod)->config;
  else
    return {};
}

String MaterialDatabase::modDescription(ModId modId, String const& species) const {
  auto mod = getModInfo(modId);
  return mod->descriptions.getString(strf("{}Description", species), mod->descriptions.getString("description"));
}

String MaterialDatabase::modDescription(ModId modId) const {
  auto mod = getModInfo(modId);
  return mod->descriptions.getString("description");
}

String MaterialDatabase::modShortDescription(ModId modId) const {
  auto mod = getModInfo(modId);
  return mod->descriptions.getString("shortdescription");
}

String MaterialDatabase::defaultFootstepSound() const {
  return m_defaultFootstepSound;
}

TileDamageParameters MaterialDatabase::materialDamageParameters(MaterialId materialId) const {
  if (!isRealMaterial(materialId))
    return {};
  else
    return getMaterialInfo(materialId)->damageParameters;
}

TileDamageParameters MaterialDatabase::modDamageParameters(ModId modId) const {
  if (!isRealMod(modId))
    return {};
  else
    return getModInfo(modId)->damageParameters;
}

bool MaterialDatabase::modBreaksWithTile(ModId modId) const {
  if (!isRealMod(modId))
    return {};
  else
    return getModInfo(modId)->breaksWithTile;
}

CollisionKind MaterialDatabase::materialCollisionKind(MaterialId materialId) const {
  if (isRealMaterial(materialId))
    return getMaterialInfo(materialId)->collisionKind;
  else if (containsMetaMaterial(materialId))
    return getMetaMaterialInfo(materialId)->collisionKind;
  else
    return CollisionKind::Block;
}

bool MaterialDatabase::canPlaceInLayer(MaterialId materialId, TileLayer layer) const {
  return layer != TileLayer::Background || !getMaterialInfo(materialId)->foregroundOnly;
}

ItemDescriptor MaterialDatabase::materialItemDrop(MaterialId materialId) const {
  if (isRealMaterial(materialId)) {
    auto matInfo = getMaterialInfo(materialId);
    if (!matInfo->itemDrop.empty())
      return ItemDescriptor(matInfo->itemDrop, 1, JsonObject());
  }

  return {};
}

ItemDescriptor MaterialDatabase::modItemDrop(ModId modId) const {
  if (isRealMod(modId)) {
    auto modInfo = getModInfo(modId);
    if (!modInfo->itemDrop.empty())
      return ItemDescriptor(modInfo->itemDrop, 1);
  }

  return {};
}

MaterialColorVariant MaterialDatabase::materialColorVariants(MaterialId materialId) const {
  if (isRealMaterial(materialId))
    if (auto profile = materialRenderProfile(materialId))
      return profile->colorVariants;

  return 0;
}

MaterialColorVariant MaterialDatabase::modColorVariants(ModId modId) const {
  if (isRealMod(modId))
    if (auto profile = modRenderProfile(modId))
      return profile->colorVariants;

  return 0;
}

bool MaterialDatabase::isMultiColor(MaterialId materialId) const {
  if (isRealMaterial(materialId))
    if (auto profile = materialRenderProfile(materialId))
      return profile->colorVariants > 0;

  return false;
}

ParticleConfigPtr MaterialDatabase::miningParticle(MaterialId materialId, ModId modId) const {
  if (isRealMod(modId)) {
    auto const& modInfo = getModInfo(modId);
    if (modInfo->miningParticle)
      return modInfo->miningParticle;
  }

  if (isRealMaterial(materialId)) {
    auto const& matInfo = getMaterialInfo(materialId);
    if (matInfo->miningParticle)
      return matInfo->miningParticle;
  }

  return ParticleConfigPtr();
}

String MaterialDatabase::miningSound(MaterialId materialId, ModId modId) const {
  if (isRealMod(modId)) {
    auto const& modInfo = getModInfo(modId);
    if (!modInfo->miningSounds.empty())
      return Random::randValueFrom(modInfo->miningSounds);
  }

  if (isRealMaterial(materialId)) {
    auto const& matInfo = getMaterialInfo(materialId);
    if (!matInfo->miningSounds.empty())
      return Random::randValueFrom(matInfo->miningSounds);
  }

  return String();
}

String MaterialDatabase::footstepSound(MaterialId materialId, ModId modId) const {
  if (isRealMod(modId)) {
    auto const& modInfo = getModInfo(modId);
    if (!modInfo->footstepSound.empty())
      return modInfo->footstepSound;
  }

  if (isRealMaterial(materialId)) {
    auto const& matInfo = getMaterialInfo(materialId);
    if (!matInfo->footstepSound.empty())
      return matInfo->footstepSound;
  }

  return m_defaultFootstepSound;
}

Color MaterialDatabase::materialParticleColor(MaterialId materialId, MaterialHue hueShift) const {
  auto color = getMaterialInfo(materialId)->particleColor;
  color.setHue(pfmod(color.hue() + materialHueToDegrees(hueShift) / 360.0f, 1.0f));
  return color;
}

bool MaterialDatabase::isTilledMod(ModId modId) const {
  if (!isRealMod(modId))
    return false;
  return getModInfo(modId)->tilled;
}

bool MaterialDatabase::isSoil(MaterialId materialId) const {
  if (!isRealMaterial(materialId))
    return false;
  return getMaterialInfo(materialId)->soil;
}

ModId MaterialDatabase::tilledModFor(MaterialId materialId) const {
  if (!isRealMaterial(materialId))
    return NoModId;
  return getMaterialInfo(materialId)->tillableMod;
}

bool MaterialDatabase::isFallingMaterial(MaterialId materialId) const {
  if (!isRealMaterial(materialId))
    return false;
  return getMaterialInfo(materialId)->falling;
}

bool MaterialDatabase::isCascadingFallingMaterial(MaterialId materialId) const {
  if (!isRealMaterial(materialId))
    return false;
  return getMaterialInfo(materialId)->cascading;
}

bool MaterialDatabase::supportsMod(MaterialId materialId, ModId modId) const {
  if (modId == NoModId)
    return true;
  if (!isRealMaterial(materialId))
    return false;
  if (!isRealMod(modId))
    return false;

  return getMaterialInfo(materialId)->supportsMods;
}

MaterialDatabase::MetaMaterialInfo::MetaMaterialInfo(String name, MaterialId id, CollisionKind collisionKind, bool blocksLiquidFlow)
  : name(name), id(id), collisionKind(collisionKind), blocksLiquidFlow(blocksLiquidFlow) {}

MaterialDatabase::MaterialInfo::MaterialInfo()
  : id(NullMaterialId), tillableMod(NoModId), falling(), cascading(), materialRenderProfileAttempted(false) {}

MaterialDatabase::ModInfo::ModInfo() : id(NoModId), tilled(), breaksWithTile(), modRenderProfileAttempted(false) {}

MaterialRenderProfileConstPtr MaterialDatabase::materialRenderProfile(MaterialId materialId) const {
  if (materialId < m_materials.size()) {
    if (auto const& mat = m_materials[materialId]) {
      if (!mat->materialRenderProfile && !mat->materialRenderProfileAttempted) {
        mat->materialRenderProfileAttempted = true;
        if (mat->config.contains("renderTemplate")) {
          try {
            auto assets = Root::singleton().assets();
            auto renderTemplate = assets->fetchJson(mat->config.get("renderTemplate"), mat->path);
            auto renderParameters = mat->config.get("renderParameters");
            mat->materialRenderProfile = make_shared<MaterialRenderProfile>(parseMaterialRenderProfile(jsonMerge(renderTemplate, renderParameters), mat->path));
#ifdef STAR_PLATFORM_N3DS
            Logger::info("N3DS MaterialDatabase: parsed material render profile '{}'", mat->name);
#endif
          } catch (std::exception const& e) {
            Logger::warn("N3DS MaterialDatabase: failed to lazily parse material render profile '{}': {}", mat->name, e.what());
          }
        }
      }
      return mat->materialRenderProfile;
    }
  }

  return {};
}

MaterialRenderProfileConstPtr MaterialDatabase::modRenderProfile(ModId modId) const {
  if (modId < m_mods.size()) {
    if (auto const& mod = m_mods[modId]) {
      if (!mod->modRenderProfile && !mod->modRenderProfileAttempted) {
        mod->modRenderProfileAttempted = true;
        if (mod->config.contains("renderTemplate")) {
          try {
            auto assets = Root::singleton().assets();
            auto renderTemplate = assets->fetchJson(mod->config.get("renderTemplate"), mod->path);
            auto renderParameters = mod->config.get("renderParameters");
            mod->modRenderProfile = make_shared<MaterialRenderProfile>(parseMaterialRenderProfile(jsonMerge(renderTemplate, renderParameters), mod->path));
#ifdef STAR_PLATFORM_N3DS
            Logger::info("N3DS MaterialDatabase: parsed mod render profile '{}'", mod->name);
#endif
          } catch (std::exception const& e) {
            Logger::warn("N3DS MaterialDatabase: failed to lazily parse mod render profile '{}': {}", mod->name, e.what());
          }
        }
      }
      return mod->modRenderProfile;
    }
  }

  return {};
}

size_t MaterialDatabase::metaMaterialIndex(MaterialId materialId) const {
  return materialId - FirstMetaMaterialId;
}

bool MaterialDatabase::containsMetaMaterial(MaterialId materialId) const {
  auto i = metaMaterialIndex(materialId);
  return m_metaMaterials.size() > i && m_metaMaterials[i];
}

void MaterialDatabase::setMetaMaterial(MaterialId materialId, MetaMaterialInfo info) {
  auto i = metaMaterialIndex(materialId);
  if (i >= m_metaMaterials.size())
    m_metaMaterials.resize(i + 1);
  m_metaMaterials[i] = make_shared<MetaMaterialInfo>(info);
  m_metaMaterialIndex[info.name] = materialId;
}

bool MaterialDatabase::containsMaterial(MaterialId materialId) const {
  return m_materials.size() > materialId && m_materials[materialId];
}

void MaterialDatabase::setMaterial(MaterialId materialId, MaterialInfo info) {
  if (materialId >= m_materials.size())
    m_materials.resize(materialId + 1);
  m_materials[materialId] = make_shared<MaterialInfo>(info);
  m_materialIndex[info.name] = materialId;
}

bool MaterialDatabase::containsMod(ModId modId) const {
  return m_mods.size() > modId && m_mods[modId];
}

void MaterialDatabase::setMod(ModId modId, ModInfo info) {
  if (modId >= m_mods.size())
    m_mods.resize(modId + 1);
  m_mods[modId] = make_shared<ModInfo>(info);
}

shared_ptr<MaterialDatabase::MetaMaterialInfo const> const& MaterialDatabase::getMetaMaterialInfo(MaterialId materialId) const {
  if (!containsMetaMaterial(materialId))
    throw MaterialException(strf("No such metamaterial id: {}\n", materialId));
  else
    return m_metaMaterials[metaMaterialIndex(materialId)];
}

shared_ptr<MaterialDatabase::MaterialInfo const> const& MaterialDatabase::getMaterialInfo(MaterialId materialId) const {
  if (materialId >= m_materials.size() || !m_materials[materialId])
    throw MaterialException(strf("No such material id: {}\n", materialId));
  else
    return m_materials[materialId];
}

shared_ptr<MaterialDatabase::ModInfo const> const& MaterialDatabase::getModInfo(ModId modId) const {
  if (modId >= m_mods.size() || !m_mods[modId])
    throw MaterialException(strf("No such mod id: {}\n", modId));
  else
    return m_mods[modId];
}

}
