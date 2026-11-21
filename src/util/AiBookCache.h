#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Read-only, indexed, whole-book AI translation cache. One .aic file sits
// alongside its EPUB. All SD access happens when Translate is opened.
namespace ai_book_cache {

enum class Result : uint8_t {
  Hit,
  NoPackage,
  InvalidPackage,
  BookMismatch,
  LowMemory,
  SentenceMissing,
  WordMissing,
  CorruptRecord,
};

// 0-based occurrence among identical visible word tokens in this sentence.
// UINT16_MAX means unknown: the record is usable only if the word is unique.
Result lookup(const std::string& bookPath, std::string_view selectedWord,
              std::string_view sentence, uint16_t wordOccurrence,
              std::string& translatedWord, std::string& translatedSentence,
              bool& complete);

// In-memory hint only; never scans the SD card from a reader page-turn tick.
// True only after validating the package header, index CRC, and EPUB SHA-256.
bool knownComplete(const std::string& bookPath);
// Cheap header hint for starting a background server job on book open.
// Full validation still happens only in lookup()/validatePackage().
bool needsServer(const std::string& bookPath);

// Validate a downloaded candidate without touching the UI-only book digest hint.
bool sha256File(const std::string& path, uint8_t digest[32]);
bool validatePackage(const std::string& candidatePath, const uint8_t expectedBookSha[32], bool& complete);
// Serialize package replacement with foreground lookup on the UI thread.
bool installPackage(const std::string& candidatePath, const std::string& bookPath);
const char* resultName(Result result);

}  // namespace ai_book_cache
