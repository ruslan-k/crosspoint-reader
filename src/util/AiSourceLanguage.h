#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#ifdef ARDUINO
#include <Arduino.h>
#else
// Minimal host-side stand-in for unit tests.
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t byte) = 0;
  virtual size_t write(const uint8_t* bytes, size_t length) = 0;
};
#endif

// Local, bounded language classification of an EPUB without network access.
// Used to stop Russian/Chinese books from being uploaded to the AI cache
// server when dc:language is wrong (for example "en").
enum class SourceLanguage : uint8_t { Unknown, Russian, Chinese, Bulgarian, Ukrainian, Other };

struct LanguageCounters {
  uint32_t cyrillic = 0;
  uint32_t latin = 0;
  uint32_t han = 0;
  uint32_t ruUnique = 0;   // ы ё э Ы Ё Э
  uint32_t ukUnique = 0;   // ї є ґ Ї Є Ґ
  uint32_t hardSign = 0;   // ъ Ъ

  void add(const LanguageCounters& other) {
    cyrillic += other.cyrillic;
    latin += other.latin;
    han += other.han;
    ruUnique += other.ruUnique;
    ukUnique += other.ukUnique;
    hardSign += other.hardSign;
  }
  uint32_t total() const { return cyrillic + latin + han; }
};

// Stream sink: counts only text nodes, never tag attributes, script/style
// bodies or HTML entities. Stops after a fixed number of input bytes so a
// short write can end ZipFile::readFileToStream(..., allowEarlyStop=true).
class LanguageProbe final : public Print {
 public:
  explicit LanguageProbe(size_t byteBudget) : budget_(byteBudget) {}

  size_t write(uint8_t byte) override {
    if (budget_ == 0) return 0;
    --budget_;
    feed(byte);
    return 1;
  }
  size_t write(const uint8_t* bytes, size_t length) override {
    size_t done = 0;
    while (done < length && budget_ > 0) {
      --budget_;
      feed(bytes[done++]);
    }
    return done;
  }
  bool exhausted() const { return budget_ == 0; }
  const LanguageCounters& counters() const { return counters_; }

 private:
  void feed(uint8_t byte);
  void emit(uint32_t cp);
  void endTag();

  size_t budget_;
  LanguageCounters counters_;
  bool inTag_ = false;
  bool inScriptStyle_ = false;
  bool closingTag_ = false;
  bool inEntity_ = false;
  uint8_t tagLen_ = 0;
  char tag_[16] = {};
  uint32_t cp_ = 0;
  uint8_t needed_ = 0;
};

SourceLanguage classifySourceLanguage(const LanguageCounters& counters, std::string_view metadata);
bool sourceAllowed(SourceLanguage language);
const char* sourceLanguageName(SourceLanguage language);
