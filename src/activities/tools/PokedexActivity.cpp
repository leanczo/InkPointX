#include "PokedexActivity.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <PngToBmpConverter.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

constexpr int kSpriteBoxSize = 120;

// PokeAPI slugs are lowercase alnum + hyphen only; typed names get folded to
// that shape instead of percent-encoded (there's nothing in a Pokemon name
// that needs it once spaces become hyphens).
std::string normalizeQuery(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    if (c == ' ') {
      out += '-';
    } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '-') {
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return out;
}

std::string capitalize(const std::string& s) {
  if (s.empty()) return s;
  std::string out = s;
  out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}

// This project only ships 5 UI languages (see lib/I18n/I18nKeys.h); PokeAPI's
// per-language names/flavor text cover both ES and KO directly, anything
// else (EN/HE/AR) falls back to English.
const char* pokeApiLangCode() {
  switch (I18N.getLanguage()) {
    case Language::ES:
      return "es";
    case Language::KO:
      return "ko";
    default:
      return "en";
  }
}

// The flavor text API field embeds the original game's manual line breaks as
// literal \f/\n control characters; collapse them to spaces so wrappedText()
// can re-wrap the paragraph to the screen width instead of honoring stale
// breaks meant for a completely different layout.
std::string cleanFlavorText(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    if (c == '\f' || c == '\n' || c == '\r') {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out += c;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string formatDexLabel(int id, const std::string& name) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%03d ", id);
  return std::string(buf) + name;
}

}  // namespace

std::string PokedexActivity::cachePath(int id) const { return "/apps/pokedex/" + std::to_string(id) + ".txt"; }

std::string PokedexActivity::spritePath(int id) const {
  return "/apps/pokedex/sprites/" + std::to_string(id) + ".bmp";
}

void PokedexActivity::loadRecent() {
  recent.clear();
  HalFile f;
  if (!Storage.openFileForRead("POKEDEX", "/apps/pokedex/recent.txt", f)) return;

  std::string line;
  auto flushLine = [&]() {
    if (line.empty()) return;
    size_t tab = line.find('\t');
    if (tab == std::string::npos) return;
    PokedexRecentEntry e;
    e.id = atoi(line.substr(0, tab).c_str());
    e.displayName = line.substr(tab + 1);
    if (e.id > 0) recent.push_back(e);
  };
  while (f.available() > 0) {
    char c = f.read();
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      flushLine();
      line.clear();
    } else {
      line += c;
    }
  }
  flushLine();
  f.close();
}

void PokedexActivity::saveRecent() {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/pokedex");
  HalFile f;
  if (Storage.openFileForWrite("POKEDEX", "/apps/pokedex/recent.txt", f)) {
    for (const auto& e : recent) {
      std::string line = std::to_string(e.id) + "\t" + e.displayName + "\n";
      f.write(line.c_str(), line.length());
    }
    f.close();
  }
}

void PokedexActivity::rememberRecent(int id, const std::string& displayName) {
  recent.erase(std::remove_if(recent.begin(), recent.end(), [id](const PokedexRecentEntry& e) { return e.id == id; }),
              recent.end());
  PokedexRecentEntry e;
  e.id = id;
  e.displayName = displayName;
  recent.insert(recent.begin(), e);
  constexpr size_t kMaxRecent = 10;
  if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
  saveRecent();
}

bool PokedexActivity::saveCacheToSd(const PokedexEntry& entry) const {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/pokedex");
  HalFile f;
  if (!Storage.openFileForWrite("POKEDEX", cachePath(entry.id), f)) return false;

  auto writeLine = [&](const std::string& s) {
    std::string line = s + "\n";
    f.write(line.c_str(), line.length());
  };

  writeLine(std::to_string(entry.id));
  writeLine(entry.name);

  std::string typesJoined;
  for (size_t i = 0; i < entry.types.size(); i++) {
    if (i > 0) typesJoined += ",";
    typesJoined += entry.types[i];
  }
  writeLine(typesJoined);

  writeLine(std::to_string(entry.heightDm));
  writeLine(std::to_string(entry.weightHg));

  std::string statsJoined;
  for (int i = 0; i < 6; i++) {
    if (i > 0) statsJoined += " ";
    statsJoined += std::to_string(entry.stats[i]);
  }
  writeLine(statsJoined);

  // Flavor text is always the last line: cleanFlavorText() already strips
  // embedded newlines, so it's safe to write as a single unescaped line.
  writeLine(entry.flavorText);

  f.close();
  return true;
}

bool PokedexActivity::loadCacheFromSd(int id) {
  HalFile f;
  if (!Storage.openFileForRead("POKEDEX", cachePath(id), f)) return false;

  std::vector<std::string> lines;
  std::string line;
  while (f.available() > 0) {
    char c = f.read();
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
      line.clear();
    } else {
      line += c;
    }
  }
  if (!line.empty()) lines.push_back(line);
  f.close();

  if (lines.size() < 6) return false;

  PokedexEntry entry;
  entry.id = atoi(lines[0].c_str());
  entry.name = lines[1];

  {
    const std::string& typesField = lines[2];
    size_t pos = 0;
    while (pos <= typesField.size()) {
      size_t comma = typesField.find(',', pos);
      std::string token = (comma == std::string::npos) ? typesField.substr(pos) : typesField.substr(pos, comma - pos);
      if (!token.empty()) entry.types.push_back(token);
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
  }

  entry.heightDm = atoi(lines[3].c_str());
  entry.weightHg = atoi(lines[4].c_str());

  {
    const std::string& statsField = lines[5];
    int idx = 0;
    size_t pos = 0;
    while (idx < 6 && pos <= statsField.size()) {
      size_t sp = statsField.find(' ', pos);
      std::string token = (sp == std::string::npos) ? statsField.substr(pos) : statsField.substr(pos, sp - pos);
      entry.stats[idx++] = atoi(token.c_str());
      if (sp == std::string::npos) break;
      pos = sp + 1;
    }
  }

  entry.flavorText = lines.size() > 6 ? lines[6] : "";

  current = entry;
  hasCurrent = true;
  hasSprite = Storage.exists(spritePath(id).c_str());
  return true;
}

bool PokedexActivity::fetchSprite(int id, const std::string& spriteUrl) {
  if (spriteUrl.empty()) return false;
  const std::string tmpPngPath = "/apps/pokedex/sprite.tmp";
  if (HttpDownloader::downloadToFile(spriteUrl, tmpPngPath) != HttpDownloader::OK) {
    Storage.remove(tmpPngPath.c_str());
    return false;
  }

  bool converted = false;
  const std::string destPath = spritePath(id);
  {
    HalFile pngSrc;
    HalFile bmpDest;
    if (Storage.openFileForRead("POKEDEX", tmpPngPath, pngSrc) &&
        Storage.openFileForWrite("POKEDEX", destPath, bmpDest)) {
      converted = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(pngSrc, bmpDest, kSpriteBoxSize, kSpriteBoxSize);
    }
    if (pngSrc) pngSrc.close();
    if (bmpDest) bmpDest.close();
  }
  if (!converted) Storage.remove(destPath.c_str());
  Storage.remove(tmpPngPath.c_str());
  return converted;
}

bool PokedexActivity::fetchAndParse(PokedexEntry& outEntry) {
  const std::string query = normalizeQuery(pendingQuery);
  if (query.empty()) return false;

  const std::string pokemonUrl = "https://pokeapi.co/api/v2/pokemon/" + query;
  const char* pokemonTmpPath = "/apps/pokedex/pokemon.tmp";

  if (HttpDownloader::downloadToFile(pokemonUrl, pokemonTmpPath) != HttpDownloader::OK) {
    Storage.remove(pokemonTmpPath);
    return false;
  }

  int id = 0;
  std::string englishName;
  std::string spriteUrl;
  std::vector<std::string> types;
  int stats[6] = {0, 0, 0, 0, 0, 0};
  {
    HalFile file;
    if (!Storage.openFileForRead("POKEDEX", pokemonTmpPath, file)) {
      Storage.remove(pokemonTmpPath);
      return false;
    }

    JsonDocument filter;
    filter["id"] = true;
    filter["name"] = true;
    filter["height"] = true;
    filter["weight"] = true;
    filter["types"][0]["type"]["name"] = true;
    filter["stats"][0]["base_stat"] = true;
    filter["stats"][0]["stat"]["name"] = true;
    filter["sprites"]["front_default"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, file, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(10));
    file.close();
    Storage.remove(pokemonTmpPath);
    if (err) return false;

    id = doc["id"] | 0;
    englishName = doc["name"] | "";
    outEntry.heightDm = doc["height"] | 0;
    outEntry.weightHg = doc["weight"] | 0;
    spriteUrl = doc["sprites"]["front_default"] | "";

    for (JsonObject t : doc["types"].as<JsonArray>()) {
      std::string typeName = t["type"]["name"] | "";
      if (!typeName.empty()) types.push_back(capitalize(typeName));
    }

    static const std::pair<const char*, int> kStatOrder[6] = {{"hp", 0},          {"attack", 1},
                                                               {"defense", 2},     {"special-attack", 3},
                                                               {"special-defense", 4}, {"speed", 5}};
    for (JsonObject s : doc["stats"].as<JsonArray>()) {
      std::string statName = s["stat"]["name"] | "";
      int base = s["base_stat"] | 0;
      for (const auto& entry : kStatOrder) {
        if (statName == entry.first) {
          stats[entry.second] = base;
          break;
        }
      }
    }
  }
  if (id == 0) return false;

  // Species lookup is best-effort: on failure, fall back to the English slug
  // name and no flavor text rather than failing the whole lookup.
  std::string localizedName = capitalize(englishName);
  std::string flavorText;
  {
    const std::string speciesUrl = "https://pokeapi.co/api/v2/pokemon-species/" + std::to_string(id);
    const char* speciesTmpPath = "/apps/pokedex/species.tmp";
    if (HttpDownloader::downloadToFile(speciesUrl, speciesTmpPath) == HttpDownloader::OK) {
      HalFile file;
      if (Storage.openFileForRead("POKEDEX", speciesTmpPath, file)) {
        JsonDocument filter;
        filter["names"][0]["name"] = true;
        filter["names"][0]["language"]["name"] = true;
        filter["flavor_text_entries"][0]["flavor_text"] = true;
        filter["flavor_text_entries"][0]["language"]["name"] = true;

        JsonDocument doc;
        DeserializationError speciesErr = deserializeJson(doc, file, DeserializationOption::Filter(filter),
                                                           DeserializationOption::NestingLimit(10));
        file.close();
        if (!speciesErr) {
          const std::string langCode = pokeApiLangCode();
          std::string nameEn, nameLang, flavorEn, flavorLang;
          for (JsonObject n : doc["names"].as<JsonArray>()) {
            std::string lang = n["language"]["name"] | "";
            std::string nm = n["name"] | "";
            if (lang == "en" && nameEn.empty()) nameEn = nm;
            if (lang == langCode && nameLang.empty()) nameLang = nm;
          }
          for (JsonObject f : doc["flavor_text_entries"].as<JsonArray>()) {
            std::string lang = f["language"]["name"] | "";
            std::string txt = f["flavor_text"] | "";
            if (lang == "en" && flavorEn.empty()) flavorEn = txt;
            if (lang == langCode && flavorLang.empty()) flavorLang = txt;
            if (!flavorLang.empty() && (langCode == std::string("en") || !flavorEn.empty())) break;
          }
          if (!nameLang.empty()) {
            localizedName = nameLang;
          } else if (!nameEn.empty()) {
            localizedName = nameEn;
          }
          flavorText = !flavorLang.empty() ? flavorLang : flavorEn;
        }
      }
    }
    Storage.remove(speciesTmpPath);
  }

  outEntry.id = id;
  outEntry.name = localizedName;
  outEntry.types = types;
  for (int i = 0; i < 6; i++) outEntry.stats[i] = stats[i];
  outEntry.flavorText = cleanFlavorText(flavorText);

  hasSprite = fetchSprite(id, spriteUrl);
  return true;
}

void PokedexActivity::promptSearch() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_POKEDEX_SEARCH), "", 30);
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
    if (!keyboardResult || keyboardResult->text.empty()) {
      requestUpdate();
      return;
    }
    openByQuery(keyboardResult->text);
  });
}

void PokedexActivity::openByQuery(const std::string& query) {
  pendingQuery = query;
  errorPopupMessage.clear();
  state = PokedexState::Loading;
  requestUpdate();
  ensureWifiThenFetch();
}

void PokedexActivity::openById(int id) {
  if (loadCacheFromSd(id)) {
    errorPopupMessage.clear();
    selectedIndex = 0;
    state = PokedexState::Detail;
    requestUpdate();
    return;
  }
  openByQuery(std::to_string(id));
}

void PokedexActivity::ensureWifiThenFetch() {
  if (WiFi.status() == WL_CONNECTED) {
    doFetch();
    return;
  }

  WiFi.mode(WIFI_STA);
  startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                          [this](const ActivityResult& result) {
                            if (result.isCancelled) {
                              errorPopupMessage = tr(STR_POKEDEX_WIFI_REQUIRED);
                              if (!hasCurrent) {
                                state = PokedexState::Landing;
                              } else {
                                state = PokedexState::Detail;
                              }
                              requestUpdate();
                            } else {
                              doFetch();
                            }
                          });
}

void PokedexActivity::doFetch() {
  requestUpdateAndWait();  // paint the "Loading" state before the blocking calls below

  wifiWasUsed = true;
  PokedexEntry entry;
  bool ok = fetchAndParse(entry);

  if (ok) {
    current = entry;
    hasCurrent = true;
    saveCacheToSd(current);
    rememberRecent(current.id, formatDexLabel(current.id, current.name));
    errorPopupMessage.clear();
    state = PokedexState::Detail;
  } else {
    errorPopupMessage = tr(STR_POKEDEX_NOT_FOUND);
    state = hasCurrent ? PokedexState::Detail : PokedexState::Landing;
  }
  requestUpdate();
}

void PokedexActivity::drawSprite(int x, int y, int boxSize) const {
  if (!hasSprite) return;
  HalFile file;
  if (!Storage.openFileForRead("POKEDEX", spritePath(current.id), file)) return;

  Bitmap bitmap(file, false);
  if (bitmap.parseHeaders() == BmpReaderError::Ok) {
    const int drawX = x + (boxSize - bitmap.getWidth()) / 2;
    const int drawY = y + (boxSize - bitmap.getHeight()) / 2;
    renderer.drawBitmap1Bit(bitmap, drawX, drawY, boxSize, boxSize);
  }
  file.close();
}

void PokedexActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/pokedex");
  Storage.ensureDirectoryExists("/apps/pokedex/sprites");

  loadRecent();
  state = PokedexState::Landing;
  selectedIndex = 0;
  hasCurrent = false;
  hasSprite = false;
  errorPopupMessage.clear();

  requestUpdate();
}

void PokedexActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void PokedexActivity::loop() {
  using Button = MappedInputManager::Button;

  if (state == PokedexState::Loading) return;  // owned by the blocking doFetch() call that triggered it

  if (mappedInput.wasReleased(Button::Back)) {
    if (state == PokedexState::Detail) {
      state = PokedexState::Landing;
      errorPopupMessage.clear();
      requestUpdate();
    } else {
      onGoHome(HomeMenuItem::APPS_MENU);
    }
    return;
  }

  if (state == PokedexState::Landing) {
    const int totalItems = static_cast<int>(recent.size()) + 1;  // row 0 = "+ Buscar"
    if (mappedInput.wasReleased(Button::Up)) {
      selectedIndex = (selectedIndex - 1 + totalItems) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      selectedIndex = (selectedIndex + 1) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      if (selectedIndex == 0) {
        promptSearch();
      } else {
        openById(recent[selectedIndex - 1].id);
      }
    }
    return;
  }

  if (state == PokedexState::Detail) {
    if (mappedInput.wasReleased(Button::Left)) {
      if (current.id > 1) openById(current.id - 1);
    } else if (mappedInput.wasReleased(Button::Right)) {
      openById(current.id + 1);
    }
    return;
  }
}

void PokedexActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  if (state == PokedexState::Landing) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POKEDEX_TITLE));

    GUI.drawButtonMenu(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(recent.size()) + 1, selectedIndex,
        [this](int index) {
          if (index == 0) return std::string(tr(STR_POKEDEX_SEARCH_PROMPT));
          return recent[index - 1].displayName;
        },
        [](int index) { return index == 0 ? UIIcon::Pokedex : UIIcon::Book; });

    if (!errorPopupMessage.empty()) GUI.drawPopup(renderer, errorPopupMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == PokedexState::Loading) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POKEDEX_TITLE));

    const int textY = contentTop + contentHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_POKEDEX_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    char dexBuf[8];
    snprintf(dexBuf, sizeof(dexBuf), "#%03d", current.id);
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, current.name.c_str(),
                  dexBuf);

    const int spriteBoxX = metrics.contentSidePadding;
    const int spriteBoxY = contentTop;
    drawSprite(spriteBoxX, spriteBoxY, kSpriteBoxSize);

    const int infoX = spriteBoxX + kSpriteBoxSize + 20;
    const int lineHeight10 = renderer.getLineHeight(UI_10_FONT_ID);
    const int lineHeightSmall = renderer.getLineHeight(SMALL_FONT_ID);
    int infoY = contentTop;

    std::string typesLine;
    for (size_t i = 0; i < current.types.size(); i++) {
      if (i > 0) typesLine += " / ";
      typesLine += current.types[i];
    }
    renderer.drawText(UI_10_FONT_ID, infoX, infoY, typesLine.c_str(), true, EpdFontFamily::BOLD);
    infoY += lineHeight10 + 6;

    char heightWeightBuf[64];
    snprintf(heightWeightBuf, sizeof(heightWeightBuf), tr(STR_POKEDEX_HEIGHT_WEIGHT), current.heightDm / 10.0f,
             current.weightHg / 10.0f);
    renderer.drawText(UI_10_FONT_ID, infoX, infoY, heightWeightBuf, true);
    infoY += lineHeight10 + 10;

    static const StrId kStatLabels[6] = {StrId::STR_POKEDEX_STAT_HP,    StrId::STR_POKEDEX_STAT_ATK,
                                         StrId::STR_POKEDEX_STAT_DEF,   StrId::STR_POKEDEX_STAT_SPATK,
                                         StrId::STR_POKEDEX_STAT_SPDEF, StrId::STR_POKEDEX_STAT_SPEED};
    for (int i = 0; i < 6; i++) {
      char statBuf[32];
      snprintf(statBuf, sizeof(statBuf), "%s: %d", I18N.get(kStatLabels[i]), current.stats[i]);
      renderer.drawText(SMALL_FONT_ID, infoX, infoY, statBuf, true);
      infoY += lineHeightSmall + 2;
    }

    const int flavorTop = std::max(infoY, spriteBoxY + kSpriteBoxSize) + 14;
    auto flavorLines = renderer.wrappedText(UI_10_FONT_ID, current.flavorText.c_str(),
                                            pageWidth - 2 * metrics.contentSidePadding, 4, EpdFontFamily::REGULAR);
    int flavorY = flavorTop;
    for (const auto& l : flavorLines) {
      if (flavorY + lineHeight10 > contentBottom) break;
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, flavorY, l.c_str(), true);
      flavorY += lineHeight10;
    }

    if (!errorPopupMessage.empty()) GUI.drawPopup(renderer, errorPopupMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
