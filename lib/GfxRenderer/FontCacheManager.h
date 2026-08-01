#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  void releasePageBuffers();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void warmGlyphCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x01);
  // Starts a new render-frame cache transaction. Card-font batches seen later
  // in the same frame are merged; a first miss replaces the previous screen.
  void beginFrame();
  // Returns true when fontId belongs to a card font (including an empty/no-op
  // request), allowing GfxRenderer to skip its direct fallback path.
  bool prepareSdCardGlyphs(int fontId, const char* utf8Text, uint8_t styleMask);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  bool isPagePrewarmedFor(int fontId) const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;
  // Vector is intentionally retained between frames: the UI uses only a few
  // faces, and avoiding one tree-node allocation per face per key press keeps
  // navigation from adding heap churn and needless active CPU time.
  std::vector<SdCardFont*> uiFontsSeenThisFrame_;

  enum class ScanMode : uint8_t { None, Scanning, PagePrewarmed };
  ScanMode scanMode_ = ScanMode::None;
  std::string scanText_;
  uint32_t scanStyleCounts_[4] = {};
  // Font ID 0 is reserved by fontIds.h / SdCardFontManager as the not-found
  // sentinel. Valid generated IDs may be negative, so sign checks are wrong.
  int scanFontId_ = 0;
  int prewarmedFontId_ = 0;
};
