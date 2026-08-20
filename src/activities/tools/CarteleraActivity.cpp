#include "CarteleraActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "Epub/converters/ImageDecoderFactory.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

// The homepage lists more than this many titles once banners/rails are
// counted twice, but nobody is going to page through more movies than this on
// a 6" e-ink screen - stop once we have enough, same reasoning as Sismos
// capping at 50 rows regardless of what the feed sends.
constexpr size_t kMaxMovies = 16;

// Distance to look for an <img alt="TITLE"> after a matching
// href="/pelicula/<slug>" before giving up on that occurrence. Measured
// against the live page: the closest card markup puts alt ~400-450 bytes
// after href; this leaves generous room for markup churn without scanning
// the whole 400KB+ page per candidate.
constexpr size_t kAltSearchWindow = 6000;

// Caps a single extracted attribute value so a malformed/huge one can't grow
// these strings unbounded - same guard SismosXmlParser applies per XML tag.
constexpr size_t kMaxAttrLen = 96;

// On success, outEndPos (if non-null) is set just past the value's closing
// quote, so a caller can chain a second findAttr search (e.g. src="" always
// follows alt="" in this markup) without re-scanning from the href.
bool findAttr(const std::string& buffer, size_t from, size_t to, const char* needle, std::string& outValue,
             size_t* outEndPos = nullptr) {
  const size_t needleLen = std::strlen(needle);
  const size_t searchEnd = std::min(to, buffer.size());
  if (from + needleLen > searchEnd) return false;
  const size_t pos = buffer.find(needle, from);
  if (pos == std::string::npos || pos >= searchEnd) return false;
  const size_t valueStart = pos + needleLen;
  const size_t valueEnd = buffer.find('"', valueStart);
  if (valueEnd == std::string::npos) return false;
  outValue = buffer.substr(valueStart, std::min(valueEnd - valueStart, kMaxAttrLen));
  if (outEndPos) *outEndPos = valueEnd + 1;
  return true;
}

// Cinemark serves titles in ALL CAPS ("SPIDER-MAN: UN NUEVO DÍA"); this reads
// better as "Spider-man: Un Nuevo Día". Word boundary is whitespace only, same
// as SismosActivity's toTitleCase - a hyphenated word stays lowercase after
// the hyphen ("Spider-man", not "Spider-Man"). Unlike Sismos's data (plain
// ASCII from INPRES), movie titles carry Spanish accents, so this also
// lowercases the UTF-8 Latin-1 Supplement block (U+00C0-U+00FF, encoded as
// lead byte 0xC3) those accented letters live in - a byte-wise ASCII
// tolower() would silently skip them and leave "DÍA" as "DÍa".
std::string toTitleCase(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  bool startOfWord = true;
  for (size_t i = 0; i < text.size();) {
    const auto b0 = static_cast<unsigned char>(text[i]);
    if (std::isspace(b0)) {
      result += static_cast<char>(b0);
      startOfWord = true;
      i++;
      continue;
    }
    if (b0 < 0x80) {
      result += startOfWord ? static_cast<char>(b0) : static_cast<char>(std::tolower(b0));
      startOfWord = false;
      i++;
      continue;
    }
    if (b0 == 0xC3 && i + 1 < text.size()) {
      const auto b1 = static_cast<unsigned char>(text[i + 1]);
      const bool isUpperAccent = !startOfWord && b1 >= 0x80 && b1 <= 0x9E && b1 != 0x97;  // 0x97 codes '×', not a letter
      result += static_cast<char>(b0);
      result += static_cast<char>(isUpperAccent ? b1 + 0x20 : b1);
      startOfWord = false;
      i += 2;
      continue;
    }
    // Any other multi-byte UTF-8 sequence: copy through unchanged rather than
    // risk corrupting an encoding this function doesn't recognize.
    result += static_cast<char>(b0);
    startOfWord = false;
    i++;
  }
  return result;
}

// A movie's detail page (movieUrl()) is a MUI/Next.js app; every rendered
// <p> carries a "MuiTypography-root MuiTypography-bodyN mui-<hash>" class,
// where the hash is a build-specific Emotion style id - too unstable to
// anchor on directly. The synopsis is reliably the FIRST
// <p class="MuiTypography-root MuiTypography-body2 ...> paragraph on the
// page: the genre/rating row above it renders as <span> chips, and every
// later body2 paragraph (runtime, release date, distributor, cast) comes
// after it - verified live across several movie pages. It sits ~125-130KB
// into the page, well short of the ~300KB+ total, so streaming stops there
// instead of downloading the whole thing.
constexpr const char* kSynopsisAnchor = "<p class=\"MuiTypography-root MuiTypography-body2";

// Bytes of trailing context kept between HttpDownloader chunks so an anchor
// or a still-open synopsis paragraph straddling a chunk boundary isn't
// missed - mirrors kAltSearchWindow's role in parseAndStore() above.
constexpr size_t kSynopsisSearchWindow = 4096;

// No single real synopsis has come close to this (longest seen ~1KB); it
// exists to bound a malformed/never-closing <p> instead of buffering
// unboundedly. Matches RSS's description/summary field cap.
constexpr size_t kMaxSynopsisLen = 2048;

// Streaming extractor for HttpDownloader::fetchUrl's DataCallback: scans
// arriving chunks for kSynopsisAnchor and the text up to its closing </p>,
// and reports "no more data needed" via feed()'s return value so the caller
// aborts the HTTP transfer as soon as it has what it came for.
class SynopsisExtractor {
 public:
  // Returns false once the synopsis has been found, or malformed markup has
  // made it clear it won't be - signals the caller to abort the transfer.
  bool feed(const uint8_t* data, size_t len) {
    buffer.append(reinterpret_cast<const char*>(data), len);

    const size_t anchorPos = buffer.find(kSynopsisAnchor);
    if (anchorPos == std::string::npos) {
      if (buffer.size() > kSynopsisSearchWindow) buffer.erase(0, buffer.size() - kSynopsisSearchWindow);
      return true;
    }

    const size_t textStart = buffer.find('>', anchorPos);
    if (textStart == std::string::npos) return true;  // need more data to see the tag's closing '>'

    const size_t textEnd = buffer.find("</p>", textStart);
    if (textEnd == std::string::npos) {
      if (buffer.size() - textStart > kMaxSynopsisLen) return false;  // malformed - not worth trusting further
      return true;  // need more data
    }

    result = buffer.substr(textStart + 1, std::min(textEnd - textStart - 1, kMaxSynopsisLen));
    return false;
  }

  std::string result;

 private:
  std::string buffer;
};

}  // namespace

std::string CarteleraActivity::cachePath() { return "/apps/cartelera/cartelera.html"; }

std::string CarteleraActivity::tmpPath() { return "/apps/cartelera/cartelera.tmp.html"; }

std::string CarteleraActivity::homeUrl() {
  // Cinemark Hoyts Argentina's real showtimes API (bff.cinemark.com.ar) is
  // not reachable from outside the site's own browser session (verified live
  // - consistent HTTP 502 with matching browser headers). The homepage is a
  // plain server-rendered page, though, and lists the current national
  // cartelera as static <a href="/pelicula/<slug>"><img alt="TITLE"> markup.
  return "https://www.cinemark.com.ar/";
}

std::string CarteleraActivity::movieUrl(const std::string& slug) { return "https://www.cinemark.com.ar/pelicula/" + slug; }

std::string CarteleraActivity::posterPath(const std::string& slug) { return "/apps/cartelera/posters/" + slug + ".jpg"; }

void CarteleraActivity::parseAndStore(HalFile& file) {
  movies.clear();
  movies.reserve(kMaxMovies);

  static constexpr size_t kChunkSize = 2048;
  char chunk[kChunkSize];
  std::string buffer;
  size_t searchPos = 0;

  auto tryExtractFrom = [&](bool eof) -> bool {
    // Returns true if it made progress (found or conclusively rejected a
    // candidate), false if it needs more data before it can decide.
    const size_t hrefPos = buffer.find("href=\"/pelicula/", searchPos);
    if (hrefPos == std::string::npos) {
      // Nothing pending - the whole scanned buffer is safe to drop.
      searchPos = buffer.size();
      return true;
    }
    std::string slug;
    if (!findAttr(buffer, hrefPos, buffer.size(), "href=\"/pelicula/", slug)) {
      if (!eof) return false;  // slug's closing quote may be past the buffer we have so far
      searchPos = hrefPos + 1;
      return true;
    }
    const size_t windowEnd = hrefPos + kAltSearchWindow;
    if (buffer.size() < windowEnd && !eof) return false;  // give it a chance to see the alt=""

    std::string title;
    size_t afterAlt = 0;
    const size_t altSearchStart = hrefPos + std::strlen("href=\"/pelicula/") + slug.size();
    if (findAttr(buffer, altSearchStart, windowEnd, "alt=\"", title, &afterAlt) && !title.empty()) {
      std::string posterUrl;
      findAttr(buffer, afterAlt, windowEnd, "src=\"", posterUrl);  // best-effort; a missing poster isn't fatal
      bool alreadySeen = false;
      for (const auto& m : movies) {
        if (m.slug == slug) {
          alreadySeen = true;
          break;
        }
      }
      if (!alreadySeen) movies.push_back(CarteleraMovie{slug, toTitleCase(title), posterUrl});
    }
    searchPos = hrefPos + std::strlen("href=\"/pelicula/") + slug.size();
    return true;
  };

  bool eof = false;
  while (!eof && movies.size() < kMaxMovies) {
    const int bytesRead = file.read(reinterpret_cast<uint8_t*>(chunk), kChunkSize);
    if (bytesRead <= 0) {
      eof = true;
    } else {
      buffer.append(chunk, static_cast<size_t>(bytesRead));
    }
    // Drain every candidate the buffer can currently resolve; stop as soon as
    // one needs more bytes than we have (unless this was the final read).
    while (movies.size() < kMaxMovies) {
      if (!tryExtractFrom(eof)) break;
      if (searchPos >= buffer.size()) break;  // nothing left to scan until more data arrives (or EOF)
    }
    // Bound memory: keep only the unprocessed tail plus enough lookback for
    // an href straddling the next chunk boundary.
    if (searchPos > kAltSearchWindow) {
      buffer.erase(0, searchPos - kAltSearchWindow);
      searchPos = kAltSearchWindow;
    }
  }
  LOG_DBG("CARTELERA", "Parsed %u movie(s)", static_cast<unsigned>(movies.size()));
}

bool CarteleraActivity::loadCacheFromSd() {
  HalFile file;
  if (!Storage.openFileForRead("CARTELERA", cachePath().c_str(), file)) return false;
  parseAndStore(file);
  loaded = !movies.empty();
  return loaded;
}

void CarteleraActivity::onEnter() {
  Activity::onEnter();
  if (!loadCacheFromSd()) startFetch();
  requestUpdate();
}

void CarteleraActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void CarteleraActivity::startFetch() {
  refreshing = true;
  refreshFailed = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/cartelera");
  Storage.ensureDirectoryExists("/apps/cartelera/posters");
  movies.clear();
  loaded = false;
  errorMessage.clear();
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                            [this](const ActivityResult& result) {
                              if (result.isCancelled) {
                                refreshing = false;
                                loadCacheFromSd();
                                if (!loaded) {
                                  errorMessage = tr(STR_CARTELERA_WIFI_REQUIRED);
                                } else {
                                  refreshFailed = true;
                                }
                                requestUpdate();
                              } else {
                                doFetch();
                              }
                            });
    return;
  }

  doFetch();
}

void CarteleraActivity::doFetch() {
  requestUpdateAndWait();  // paint the "Refreshing..." state before the blocking call below
  wifiWasUsed = true;

  const auto result = HttpDownloader::downloadToFile(homeUrl(), tmpPath());
  refreshing = false;

  if (result == HttpDownloader::OK) {
    Storage.remove(cachePath().c_str());
    Storage.rename(tmpPath().c_str(), cachePath().c_str());
  }

  // The list was cleared before the fetch started, so the reload must happen
  // unconditionally - on failure this is the only way to get the old
  // (still-good, untouched-on-disk) data back.
  loadCacheFromSd();
  if (!loaded) {
    if (errorMessage.empty()) errorMessage = tr(STR_CARTELERA_NO_DATA);
  } else if (result != HttpDownloader::OK) {
    refreshFailed = true;
  }
  if (loaded) fetchPosters();
  requestUpdate();
}

void CarteleraActivity::fetchPosters() {
  // Runs after doFetch() already cleared `refreshing`, so the screen stays on
  // whatever requestUpdate() below last painted while these downloads happen
  // - deliberately not calling requestUpdateAndWait() per poster, which would
  // mean up to kMaxMovies full e-ink refreshes back to back for one list.
  // Best-effort: a poster that fails to download just leaves that row without
  // a thumbnail (see render()), same "degrade gracefully" spirit as the rest
  // of this activity's error handling. Re-fetched on every manual refresh -
  // this only runs from the explicit Right-button refresh, not on every
  // onEnter(), so redownloading everything each time is an acceptable trade
  // for never showing a poster that no longer matches its listed movie.
  for (const auto& movie : movies) {
    if (movie.posterUrl.empty()) continue;
    const std::string dest = posterPath(movie.slug);
    if (HttpDownloader::downloadToFile(movie.posterUrl, dest) != HttpDownloader::OK) {
      Storage.remove(dest.c_str());
    }
  }
}

void CarteleraActivity::showTicketsForSelected() {
  if (selectedRow < 0 || selectedRow >= static_cast<int>(movies.size())) return;
  startActivityForResult(makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, movieUrl(movies[selectedRow].slug)),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void CarteleraActivity::startSynopsisFetch() {
  if (selectedRow < 0 || selectedRow >= static_cast<int>(movies.size())) return;
  showingSynopsis = true;
  if (!movies[selectedRow].synopsis.empty()) {
    // Already fetched this session (e.g. the user backed out and reopened
    // it) - nothing to do over the network.
    requestUpdate();
    return;
  }
  synopsisLoading = true;
  synopsisFetchFailed = false;
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               synopsisLoading = false;
                               synopsisFetchFailed = true;
                               requestUpdate();
                             } else {
                               doFetchSynopsis();
                             }
                           });
    return;
  }

  doFetchSynopsis();
}

void CarteleraActivity::doFetchSynopsis() {
  if (selectedRow < 0 || selectedRow >= static_cast<int>(movies.size())) return;
  requestUpdateAndWait();  // paint the "Cargando sinopsis..." state before the blocking call below
  wifiWasUsed = true;

  SynopsisExtractor extractor;
  HttpDownloader::fetchUrl(movieUrl(movies[selectedRow].slug),
                           [&extractor](const uint8_t* data, size_t len) { return extractor.feed(data, len); });

  synopsisLoading = false;
  if (!extractor.result.empty()) {
    movies[selectedRow].synopsis = std::move(extractor.result);
    synopsisFetchFailed = false;
  } else {
    synopsisFetchFailed = true;
  }
  requestUpdate();
}

void CarteleraActivity::loop() {
  if (showingSynopsis) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      showingSynopsis = false;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::APPS_MENU);
    return;
  }
  if (!movies.empty() && mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    selectedRow = (selectedRow - 1 + static_cast<int>(movies.size())) % static_cast<int>(movies.size());
    requestUpdate();
  } else if (!movies.empty() && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selectedRow = (selectedRow + 1) % static_cast<int>(movies.size());
    requestUpdate();
  } else if (!movies.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showTicketsForSelected();
  } else if (!movies.empty() && mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    startSynopsisFetch();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    startFetch();
  }
}

void CarteleraActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CARTELERA_TITLE));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (showingSynopsis) {
    const auto& movie = movies[selectedRow];
    GUI.drawSubHeader(renderer, Rect{0, listTop, pageWidth, metrics.subHeaderHeight}, movie.title.c_str());
    const int textTop = listTop + metrics.subHeaderHeight + metrics.verticalSpacing;

    if (synopsisLoading) {
      const int textY = textTop + (listBottom - textTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_CARTELERA_SYNOPSIS_LOADING));
    } else if (movie.synopsis.empty()) {
      const char* msg = tr(STR_CARTELERA_SYNOPSIS_UNAVAILABLE);
      const int errWidth = pageWidth - 2 * metrics.contentSidePadding;
      auto errLines = renderer.wrappedText(UI_12_FONT_ID, msg, errWidth, 2, EpdFontFamily::REGULAR);
      const int errLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
      int errY = textTop + (listBottom - textTop) / 2 - (static_cast<int>(errLines.size()) * errLineHeight) / 2;
      for (const auto& line : errLines) {
        renderer.drawCenteredText(UI_12_FONT_ID, errY, line.c_str());
        errY += errLineHeight;
      }
    } else {
      const int wrapWidth = pageWidth - 2 * metrics.contentSidePadding;
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int maxLines = std::max(1, (listBottom - textTop) / lineHeight);
      auto lines = renderer.wrappedText(UI_10_FONT_ID, movie.synopsis.c_str(), wrapWidth, maxLines, EpdFontFamily::REGULAR);
      int textY = textTop;
      for (const auto& line : lines) {
        renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, textY, line.c_str(), true, EpdFontFamily::REGULAR);
        textY += lineHeight;
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (refreshing) {
    const int textY = listTop + (listBottom - listTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_CARTELERA_REFRESHING));
  } else if (!loaded) {
    const char* msg = !errorMessage.empty() ? errorMessage.c_str() : tr(STR_CARTELERA_LOADING);
    // drawCenteredText is single-line only; wrap long error text instead of
    // letting it overflow the screen edge (same fix as Football's/Sismos').
    const int errWidth = pageWidth - 2 * metrics.contentSidePadding;
    auto errLines = renderer.wrappedText(UI_12_FONT_ID, msg, errWidth, 2, EpdFontFamily::REGULAR);
    const int errLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    int errY = listTop + (listBottom - listTop) / 2 - (static_cast<int>(errLines.size()) * errLineHeight) / 2;
    for (const auto& line : errLines) {
      renderer.drawCenteredText(UI_12_FONT_ID, errY, line.c_str());
      errY += errLineHeight;
    }
  } else {
    // GUI.drawList has no per-row image slot (only baked-in UIIcon glyphs),
    // so posters need a hand-rolled paginated list instead. Kept deliberately
    // simpler than drawList: no RTL, accessories or scrollbar - this is a
    // plain movie title list, not a settings/library row.
    constexpr int kRowPadding = 8;
    constexpr int kThumbToTitleGap = 10;
    constexpr int kPosterAspectNum = 2;  // width:height ~= 2:3, standard poster proportions
    constexpr int kPosterAspectDen = 3;
    const int thumbHeight = std::max(24, (listBottom - listTop) / 4 - 2 * kRowPadding);
    const int thumbWidth = thumbHeight * kPosterAspectNum / kPosterAspectDen;
    const int rowHeight = thumbHeight + 2 * kRowPadding;
    const int pageItems = std::max(1, (listBottom - listTop) / rowHeight);
    const int pageStart = selectedRow >= 0 ? (selectedRow / pageItems) * pageItems : 0;
    const int pageEnd = std::min(static_cast<int>(movies.size()), pageStart + pageItems);
    const int rowLeft = metrics.contentSidePadding;
    const int rowRight = pageWidth - metrics.contentSidePadding;
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int excerptLineHeight = renderer.getLineHeight(SMALL_FONT_ID);

    for (int i = pageStart; i < pageEnd; ++i) {
      const int rowY = listTop + (i - pageStart) * rowHeight;
      if (i == selectedRow) {
        renderer.drawRoundedRect(rowLeft - 4, rowY + 2, rowRight - rowLeft + 8, rowHeight - 4, 2, 6, true);
      }

      const int thumbX = rowLeft + 4;
      const int thumbY = rowY + kRowPadding;
      const std::string poster = posterPath(movies[i].slug);
      if (Storage.exists(poster.c_str())) {
        auto* decoder = ImageDecoderFactory::getDecoder(poster);
        RenderConfig config;
        config.x = thumbX;
        config.y = thumbY;
        config.maxWidth = thumbWidth;
        config.maxHeight = thumbHeight;
        config.useGrayscale = true;
        config.useDithering = true;
        if (!decoder || !decoder->decodeToFramebuffer(poster, renderer, config)) {
          renderer.drawRoundedRect(thumbX, thumbY, thumbWidth, thumbHeight, 1, 3, true);
        }
      } else {
        renderer.drawRoundedRect(thumbX, thumbY, thumbWidth, thumbHeight, 1, 3, true);
      }

      const int textLeft = thumbX + thumbWidth + kThumbToTitleGap;
      const int textWidth = std::max(0, rowRight - 4 - textLeft);
      // Capped at 2 lines (was 3) to leave room for the synopsis excerpt
      // below - only shown once the user has opened "Sinopsis" for this
      // movie at least once this session, see startSynopsisFetch().
      auto titleLines = renderer.wrappedText(UI_12_FONT_ID, movies[i].title.c_str(), textWidth, 2, EpdFontFamily::REGULAR);
      int textY = rowY + std::max(0, (rowHeight - static_cast<int>(titleLines.size()) * titleLineHeight) / 2);
      for (const auto& line : titleLines) {
        renderer.drawText(UI_12_FONT_ID, textLeft, textY, line.c_str(), true, EpdFontFamily::REGULAR);
        textY += titleLineHeight;
      }
      if (!movies[i].synopsis.empty()) {
        auto excerptLines = renderer.wrappedText(SMALL_FONT_ID, movies[i].synopsis.c_str(), textWidth, 1, EpdFontFamily::REGULAR);
        for (const auto& line : excerptLines) {
          renderer.drawText(SMALL_FONT_ID, textLeft, textY, line.c_str(), true, EpdFontFamily::REGULAR);
          textY += excerptLineHeight;
        }
      }
    }
  }

  if (refreshFailed) {
    GUI.drawPopup(renderer, tr(STR_CARTELERA_REFRESH_FAILED));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CARTELERA_BUY_TICKET),
                                            movies.empty() ? nullptr : tr(STR_CARTELERA_SYNOPSIS), tr(STR_CARTELERA_REFRESH));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
