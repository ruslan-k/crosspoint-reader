#pragma once

#include <cctype>
#include <cstring>
#include <string_view>

// Bounded punctuation/abbreviation check for EPUB word tokens. This is not
// a full Punkt tokenizer: paragraph boundaries and visible page windows are
// handled by ClipSelectionActivity.
namespace ai_sentence {

// Skip translation for Cyrillic source text, but allow Bulgarian (bg->ru).
// Russian ы/ё/э are absent in Bulgarian; Bulgarian ъ is frequent, Russian hard
// sign is rare. Three letters avoids tripping on a lone Cyrillic name.
inline bool isRussianText(std::string_view text) {
  size_t letters = 0, hardSigns = 0;
  bool russianOnly = false;
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const unsigned char n = static_cast<unsigned char>(text[i + 1]);
    if (!((c == 0xD0 && n >= 0x80) || (c == 0xD1 && n >= 0x80 && n <= 0x8F))) continue;
    ++letters;
    if (text.substr(i, 2) == "\xD1\x91" || text.substr(i, 2) == "\xD1\x8D" ||
        text.substr(i, 2) == "\xD1\x8B") russianOnly = true;
    if (text.substr(i, 2) == "\xD1\x8A") {  // ъ
      ++hardSigns;
      const std::string_view after = text.substr(i + 2, 2);
      if (after == "\xD0\xB0" || after == "\xD0\xB5" || after == "\xD0\xB8" || after == "\xD0\xBE" ||
          after == "\xD0\xB1" || after == "\xD1\x8E" || after == "\xD1\x8F") russianOnly = true;
    }
    ++i;
  }
  if (russianOnly) return true;
  return letters >= 3 && hardSigns == 0;
}

inline bool isEnd(const char* word) {
  if (!word || !*word) return false;
  std::string_view token(word);
  while (!token.empty()) {
    const char c = token.back();
    if (c == '"' || c == '\'' || c == ')' || c == ']' || c == '}' ||
        static_cast<unsigned char>(c) == 0xBB) {  // closing ASCII/Latin-1 quote
      token.remove_suffix(1);
    } else if (token.size() >= 3 &&
               (token.substr(token.size() - 3) == "\xE2\x80\x9D" || token.substr(token.size() - 3) == "\xE2\x80\x99")) {
      token.remove_suffix(3);
    } else {
      break;
    }
  }
  if (token.empty()) return false;
  if (token.ends_with("\xE3\x80\x82") || token.ends_with("\xEF\xBC\x81") || token.ends_with("\xEF\xBC\x9F"))
    return true;  // 。 ！ ？
  const char end = token.back();
  if (end == '!' || end == '?') return true;
  if (end != '.') return false;
  // Common abbreviations do not terminate a sentence. The test is
  // case-insensitive and needs no heap allocation.
  constexpr std::string_view abbrev[] = {"mr.", "mrs.", "ms.",  "dr.",  "prof.", "sr.",  "jr.",  "st.",
                                         "vs.", "etc.", "e.g.", "i.e.", "p.m.",  "a.m.", "u.s.", "u.k."};
  for (const auto a : abbrev) {
    if (a.size() != token.size()) continue;
    bool equal = true;
    for (size_t i = 0; i < a.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(token[i])) != a[i]) {
        equal = false;
        break;
      }
    }
    if (equal) return false;
  }
  // A single-letter initial followed by a period is not a sentence end.
  if (token.size() == 2 && std::isalpha(static_cast<unsigned char>(token[0]))) return false;
  return true;
}

}  // namespace ai_sentence
