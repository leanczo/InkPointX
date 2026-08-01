#include "SdCardFontSystem.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <SdCardFont.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <new>

#include "CrossPointSettings.h"

namespace {

static uint8_t fontSizeEnumFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;  // default to MEDIUM
  return e;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, fontSizeEnumFromSettings())) {
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.sdFontFamilyName[0] = '\0';
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    const auto* selected = family->findClosestReaderSize(sizeEnum);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, sizeEnum)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}

int SdCardFontSystem::loadInterfaceFaces(const char* familyName, GfxRenderer& renderer, const InterfaceSlot* slots,
                                         const size_t slotCount) {
  if (!familyName || !*familyName || !slots || slotCount == 0) return 0;
  const auto* family = registry_.findFamily(familyName);
  if (!family || family->files.empty()) {
    LOG_ERR("SDFS", "Interface font family not on card: %s", familyName);
    return 0;
  }

  // The whole interface stays resident at once, unlike the reader's single
  // face, so leave the heap enough room for a chapter to still paginate.
  constexpr uint32_t MIN_FREE_HEAP = 60000;

  int bound = 0;
  for (size_t i = 0; i < slotCount; ++i) {
    // Closest available size wins. A family that ships only 12 and 16 pt maps
    // the small end of the scale onto 12 and the large end onto 16 instead of
    // refusing to load.
    const SdCardFontFileInfo* best = nullptr;
    int bestDelta = 0;
    for (const auto& file : family->files) {
      const int delta = std::abs(static_cast<int>(file.pointSize) - static_cast<int>(slots[i].pointSize));
      if (!best || delta < bestDelta) {
        best = &file;
        bestDelta = delta;
      }
    }
    if (!best || bestDelta > static_cast<int>(slots[i].maxDelta)) {
      LOG_DBG("SDFS", "No %u pt face within %u pt in %s: slot keeps the built-in", slots[i].pointSize,
              slots[i].maxDelta, familyName);
      continue;
    }

    // Already loaded for an earlier slot: register the same face again rather
    // than paying for a second copy of identical glyph data.
    const auto existing = std::find_if(uiFaces_.cbegin(), uiFaces_.cend(),
                                       [best](const InterfaceFace& loadedFace) { return loadedFace.path == best->path; });
    SdCardFont* face = existing == uiFaces_.cend() ? nullptr : existing->font;

    if (!face) {
      if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
        LOG_ERR("SDFS", "Stopping interface font load at %s: %u bytes free", best->path.c_str(),
                (unsigned)ESP.getFreeHeap());
        break;
      }
      auto* loaded = new (std::nothrow) SdCardFont();
      if (!loaded) break;
      if (!loaded->load(best->path.c_str())) {
        LOG_ERR("SDFS", "Cannot load interface face %s", best->path.c_str());
        delete loaded;
        continue;
      }
      uiFaces_.push_back({loaded, best->path});
      face = loaded;
    }

    renderer.registerSdCardFont(slots[i].fontId, face);
    renderer.insertFont(slots[i].fontId,
                        EpdFontFamily(face->getEpdFont(0), face->getEpdFont(1), face->getEpdFont(2),
                                      face->getEpdFont(3)));
    uiBoundIds_.push_back(slots[i].fontId);
    ++bound;
  }

  LOG_DBG("SDFS", "Interface family %s: %d of %u slots from %u file(s)", familyName, bound, (unsigned)slotCount,
          (unsigned)uiFaces_.size());
  return bound;
}

void SdCardFontSystem::unloadInterfaceFaces(GfxRenderer& renderer) {
  for (const int id : uiBoundIds_) renderer.removeFont(id);
  uiBoundIds_.clear();
  for (const auto& face : uiFaces_) delete face.font;
  uiFaces_.clear();
}
