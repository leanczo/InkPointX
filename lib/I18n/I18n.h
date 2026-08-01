#pragma once

#include <cstdint>

#include "I18nKeys.h"
/**
 * Internationalization (i18n) system for CrossPoint Reader
 */

class I18n {
 public:
  static I18n& getInstance();

  // Disable copy
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Get localized string by ID
  const char* get(StrId id) const;

  const char* operator[](StrId id) const { return get(id); }

  Language getLanguage() const { return _language; }

  // True when the active interface language reads right to left.
  //
  // Rows already mirror themselves from the direction of their own content, which
  // is right for a book title. Chrome cannot: a footer counter, a path bar or an
  // empty-state message has no content to infer from, so it stayed flush left
  // against an otherwise right-aligned interface. This is the locale-level signal
  // those elements need.
  static bool isRtlLanguage(Language lang) { return lang == Language::HE || lang == Language::AR; }
  bool isRtl() const { return isRtlLanguage(_language); }
  void setLanguage(Language lang);
  const char* getLanguageName(Language lang) const;
  static Language languageFromCode(const char* code);

  // Get all unique characters used in a specific language
  // Returns a sorted string of unique characters
  static const char* getCharacterSet(Language lang);

 private:
  I18n() : _language(Language::EN) {}

  Language _language;
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
