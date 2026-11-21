#include "util/AiSourceLanguage.h"

#include <cstring>

namespace {

bool startsWithLanguage(std::string_view value, char a, char b) {
  if (value.size() < 2) return false;
  const char first = value[0] | 0x20;
  const char second = value[1] | 0x20;
  return first == a && second == b;
}

}  // namespace

void LanguageProbe::feed(uint8_t byte) {
  if (inTag_) {
    if (byte == '>') {
      endTag();
      return;
    }
    if (tagLen_ == 0 && byte == '/') closingTag_ = true;
    if (tagLen_ < sizeof(tag_) - 1) {
      if (byte >= 'A' && byte <= 'Z') byte = static_cast<uint8_t>(byte - 'A' + 'a');
      tag_[tagLen_++] = static_cast<char>(byte);
    }
    return;
  }
  if (byte == '<') {
    inTag_ = true;
    closingTag_ = false;
    tagLen_ = 0;
    tag_[0] = '\0';
    return;
  }
  if (inScriptStyle_) return;
  if (byte == '&') {
    inEntity_ = true;
    return;
  }
  if (inEntity_) {
    if (byte == ';') inEntity_ = false;
    return;
  }

  if (needed_ == 0) {
    if (byte < 0x80) {
      emit(byte);
    } else if ((byte & 0xE0) == 0xC0) {
      cp_ = byte & 0x1F;
      needed_ = 1;
    } else if ((byte & 0xF0) == 0xE0) {
      cp_ = byte & 0x0F;
      needed_ = 2;
    } else if ((byte & 0xF8) == 0xF0) {
      cp_ = byte & 0x07;
      needed_ = 3;
    }
    return;
  }
  if ((byte & 0xC0) != 0x80) {
    needed_ = 0;
    return;
  }
  cp_ = (cp_ << 6) | (byte & 0x3F);
  if (--needed_ == 0) emit(cp_);
}

void LanguageProbe::emit(uint32_t cp) {
  if (cp >= 0x410 && cp <= 0x52F) {
    ++counters_.cyrillic;
    if (cp == 0x42B || cp == 0x44B || cp == 0x42D || cp == 0x44D || cp == 0x401 || cp == 0x451) {
      ++counters_.ruUnique;
    }
    if (cp == 0x407 || cp == 0x457 || cp == 0x404 || cp == 0x454 || cp == 0x490 || cp == 0x491) {
      ++counters_.ukUnique;
    }
    if (cp == 0x42A || cp == 0x44A) ++counters_.hardSign;
  } else if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
             (cp >= 0xF900 && cp <= 0xFAFF)) {
    ++counters_.han;
  } else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
    ++counters_.latin;
  }
}

void LanguageProbe::endTag() {
  tag_[tagLen_] = '\0';
  const char* name = tag_;
  if (name[0] == '/') ++name;
  const bool richMarkup = std::strncmp(name, "script", 6) == 0 || std::strncmp(name, "style", 5) == 0;
  if (richMarkup) inScriptStyle_ = !closingTag_;
  inTag_ = false;
}

SourceLanguage classifySourceLanguage(const LanguageCounters& c, std::string_view metadata) {
  const bool metaRussian = startsWithLanguage(metadata, 'r', 'u');
  const bool metaChinese = startsWithLanguage(metadata, 'z', 'h');
  const bool metaBulgarian = startsWithLanguage(metadata, 'b', 'g');
  const bool metaUkrainian = startsWithLanguage(metadata, 'u', 'k');

  // Explicit metadata is authoritative except when the text sample gives a
  // strong contradicting signal (a "bg" file with mostly Russian text).
  const uint32_t total = c.total();
  const bool strongRussian = c.ruUnique >= 4 && c.cyrillic > 0 && c.ruUnique * 1000 >= c.cyrillic * 5;
  const bool strongUkrainian = c.ukUnique >= 3 && c.cyrillic > 0 && c.ukUnique * 1000 >= c.cyrillic * 5;
  const bool strongChinese = c.han >= 20 && total > 0 && c.han * 100 >= total * 35;

  if (metaChinese || strongChinese) return SourceLanguage::Chinese;
  if (metaRussian || strongRussian) return SourceLanguage::Russian;
  if (metaUkrainian || strongUkrainian) return SourceLanguage::Ukrainian;
  if (metaBulgarian && !strongRussian && !strongUkrainian) return SourceLanguage::Bulgarian;
  if (total >= 20 && c.latin * 100 >= total * 80 && c.cyrillic < total / 20 && c.han < 10) {
    return SourceLanguage::Other;
  }
  return SourceLanguage::Unknown;
}

bool sourceAllowed(SourceLanguage language) {
  return language == SourceLanguage::Bulgarian || language == SourceLanguage::Ukrainian ||
         language == SourceLanguage::Other;
}

const char* sourceLanguageName(SourceLanguage language) {
  switch (language) {
    case SourceLanguage::Unknown: return "unknown";
    case SourceLanguage::Russian: return "russian";
    case SourceLanguage::Chinese: return "chinese";
    case SourceLanguage::Bulgarian: return "bulgarian";
    case SourceLanguage::Ukrainian: return "ukrainian";
    case SourceLanguage::Other: return "other";
  }
  return "unknown";
}
