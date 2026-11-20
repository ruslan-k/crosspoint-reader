#include "AiTranslationCache.h"

#include <HalMemory.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace ai_translation_cache {
namespace {

constexpr uint64_t FNV_OFFSET = UINT64_C(14695981039346656037);
constexpr uint64_t FNV_PRIME = UINT64_C(1099511628211);
constexpr uint32_t MAGIC = 0x31494143;  // "CAI1"; bump when the record changes.
constexpr size_t MAX_WORD = 256;
constexpr size_t MAX_SENTENCE = 2048;
constexpr size_t MAX_WORD_RESULT = 256;
constexpr size_t MAX_SENTENCE_RESULT = 4096;
constexpr const char* ROOT = "/.crosspoint/ai-cache";

struct Header {
  uint32_t magic = MAGIC;
  uint32_t wordBytes = 0;
  uint32_t sentenceBytes = 0;
  uint32_t translatedWordBytes = 0;
  uint32_t translatedSentenceBytes = 0;
  uint32_t reserved = 0;
  uint64_t scopeHash = 0;
  uint64_t keyHash = 0;
  uint64_t payloadHash = 0;
};
static_assert(sizeof(Header) == 48);

uint64_t update(uint64_t h, const void* bytes, size_t size) {
  const auto* p = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= FNV_PRIME;
  }
  return h;
}

uint64_t combine(uint64_t h, std::string_view s) {
  // Include the length, to distinguish ("ab","c") from ("a","bc").
  const uint64_t n = s.size();
  for (unsigned i = 0; i < 8; ++i) {
    const uint8_t byte = static_cast<uint8_t>(n >> (i * 8));
    h = update(h, &byte, 1);
  }
  return update(h, s.data(), s.size());
}

uint64_t scope(std::string_view book, std::string_view url, std::string_view model) {
  uint64_t h = combine(FNV_OFFSET, "ru/v1");
  h = combine(h, book);
  h = combine(h, url);
  return combine(h, model);
}

uint64_t key(uint64_t scopeHash, std::string_view word, std::string_view sentence) {
  return combine(combine(scopeHash, word), sentence);
}

std::string hex(uint64_t n) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(n));
  return buf;
}

std::string directory(uint64_t scopeHash) { return std::string(ROOT) + "/" + hex(scopeHash); }

std::string path(uint64_t scopeHash, uint64_t keyHash) { return directory(scopeHash) + "/" + hex(keyHash) + ".bin"; }

bool validSource(std::string_view word, std::string_view sentence) {
  return word.size() <= MAX_WORD && !sentence.empty() && sentence.size() <= MAX_SENTENCE;
}

bool readMatching(HalFile& file, std::string_view expected, uint64_t& hash) {
  char part[128];
  size_t offset = 0;
  while (offset < expected.size()) {
    const size_t n = std::min(sizeof(part), expected.size() - offset);
    if (file.read(part, n) != static_cast<int>(n) || memcmp(part, expected.data() + offset, n) != 0) return false;
    hash = update(hash, part, n);
    offset += n;
  }
  return true;
}

bool readResult(HalFile& file, uint32_t size, std::string& result, uint64_t& hash) {
  if (size == 0) {
    result.clear();
    return true;
  }
  result.resize(size);
  if (file.read(result.data(), size) != static_cast<int>(size)) return false;
  hash = update(hash, result.data(), result.size());
  return true;
}

bool writePart(HalFile& file, std::string_view bytes) {
  return bytes.empty() || file.write(bytes.data(), bytes.size()) == bytes.size();
}

}  // namespace

bool load(std::string_view book, std::string_view url, std::string_view model, std::string_view word,
          std::string_view sentence, std::string& wordTranslation, std::string& sentenceTranslation) {
  if (!validSource(word, sentence)) return false;
  const auto memory = HalMemory::getDefaultHeap();
  // Cache lookup must never consume the reader's last contiguous block.
  if (memory.freeBytes < 16 * 1024 || memory.largestBlockBytes < 8 * 1024) return false;
  const uint64_t s = scope(book, url, model);
  const uint64_t k = key(s, word, sentence);
  const std::string filename = path(s, k);
  if (!Storage.exists(filename.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("AI_CACHE", filename, file)) return false;
  Header h;
  bool ok =
      file.read(&h, sizeof(h)) == sizeof(h) && h.magic == MAGIC && h.reserved == 0 && h.scopeHash == s &&
      h.keyHash == k && h.wordBytes == word.size() && h.sentenceBytes == sentence.size() &&
      h.translatedWordBytes <= MAX_WORD_RESULT && h.translatedSentenceBytes > 0 &&
      h.translatedSentenceBytes <= MAX_SENTENCE_RESULT &&
      file.fileSize() == sizeof(h) + h.wordBytes + h.sentenceBytes + h.translatedWordBytes + h.translatedSentenceBytes;
  uint64_t check = FNV_OFFSET;
  if (ok) ok = readMatching(file, word, check) && readMatching(file, sentence, check);
  if (ok) ok = readResult(file, h.translatedWordBytes, wordTranslation, check);
  if (ok) ok = readResult(file, h.translatedSentenceBytes, sentenceTranslation, check);
  file.close();
  if (!ok || check != h.payloadHash) {
    wordTranslation.clear();
    sentenceTranslation.clear();
    return false;
  }
  return true;
}

bool save(std::string_view book, std::string_view url, std::string_view model, std::string_view word,
          std::string_view sentence, std::string_view wordTranslation, std::string_view sentenceTranslation) {
  if (!validSource(word, sentence) || wordTranslation.size() > MAX_WORD_RESULT || sentenceTranslation.empty() ||
      sentenceTranslation.size() > MAX_SENTENCE_RESULT)
    return false;
  const auto memory = HalMemory::getDefaultHeap();
  if (memory.freeBytes < 12 * 1024 || memory.largestBlockBytes < 4 * 1024) return false;
  const uint64_t s = scope(book, url, model);
  const uint64_t k = key(s, word, sentence);
  const std::string folder = directory(s);
  if (!Storage.ensureDirectoryExists(ROOT) || !Storage.ensureDirectoryExists(folder.c_str())) return false;
  const std::string filename = path(s, k);
  const std::string temp = filename + ".tmp";
  Header h;
  h.scopeHash = s;
  h.keyHash = k;
  h.wordBytes = word.size();
  h.sentenceBytes = sentence.size();
  h.translatedWordBytes = wordTranslation.size();
  h.translatedSentenceBytes = sentenceTranslation.size();
  h.payloadHash = update(update(update(update(FNV_OFFSET, word.data(), word.size()), sentence.data(), sentence.size()),
                                wordTranslation.data(), wordTranslation.size()),
                         sentenceTranslation.data(), sentenceTranslation.size());
  HalFile file;
  if (!Storage.openFileForWrite("AI_CACHE", temp, file)) return false;
  const bool ok = file.write(&h, sizeof(h)) == sizeof(h) && writePart(file, word) && writePart(file, sentence) &&
                  writePart(file, wordTranslation) && writePart(file, sentenceTranslation);
  file.flush();
  file.close();
  if (ok && Storage.replaceFile(temp.c_str(), filename.c_str())) return true;
  Storage.remove(temp.c_str());
  return false;
}

}  // namespace ai_translation_cache
