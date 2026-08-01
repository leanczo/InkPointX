#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>
#include <string>
#include <vector>

class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + fontSize enum.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /// One interface slot: the font id the UI draws with, and the point size the
  /// built-in face at that slot has.
  struct InterfaceSlot {
    int fontId;
    uint8_t pointSize;
    // How far from `pointSize` a card face may be and still take this slot.
    // Reader font packs ship 12-18 pt while the interface scale starts at 8,
    // and a face 50% too large does not "look bigger", it breaks the rows it
    // is measured into. Structural slots stay tight; the accent line, which is
    // sized by eye rather than by layout, can take whatever is closest.
    uint8_t maxDelta;
  };

  /// Binds `familyName`'s closest sizes to the given interface slots, so the
  /// whole UI (and the handwritten accent) can come off the card. Unlike the
  /// reader's single loaded face these stay resident together, so sizes that
  /// resolve to the same file are loaded once and shared. Returns how many
  /// slots were bound; the caller keeps the built-in face for the rest.
  int loadInterfaceFaces(const char* familyName, GfxRenderer& renderer, const InterfaceSlot* slots, size_t slotCount);

  /// Drops every interface face and its registration. Safe to call when none
  /// are loaded. Must run under the render lock: it frees glyph data.
  void unloadInterfaceFaces(GfxRenderer& renderer);

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  // Interface faces are owned here rather than by the manager: the manager is
  // built around one loaded family (the reader's) and unloads it wholesale.
  struct InterfaceFace {
    SdCardFont* font;
    std::string path;  // so two slots that want the same size share one load
  };
  std::vector<InterfaceFace> uiFaces_;
  std::vector<int> uiBoundIds_;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
