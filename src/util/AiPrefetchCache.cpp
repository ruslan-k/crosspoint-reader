#include "AiPrefetchCache.h"

#include <HalMemory.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace ai_prefetch {
namespace {
constexpr char ROOT[] = "/.crosspoint/ai-prefetch";
constexpr uint32_t RECORD_MAGIC = 0x31504641;      // AFP1
constexpr uint32_t CHECKPOINT_MAGIC = 0x31435041;  // APC1
constexpr uint64_t HASH_SEED = UINT64_C(14695981039346656037);
constexpr uint64_t HASH_PRIME = UINT64_C(1099511628211);
constexpr size_t MAX_WORD_BYTES = 160;
constexpr size_t MAX_WORD_TRANSLATION = 256;
constexpr size_t MAX_TRANSLATION = 4096;

struct Header {
  uint32_t magic;
  uint32_t spine;
  uint32_t offset;
  uint32_t sourceBytes;
  uint32_t translatedBytes;
  uint32_t wordCount;
  uint64_t scopeHash;
  uint64_t keyHash;
  uint64_t payloadHash;
};
static_assert(sizeof(Header) == 48);

struct WordHeader {
  uint32_t offset;
  uint16_t sourceBytes;
  uint16_t translatedBytes;
};
static_assert(sizeof(WordHeader) == 8);

struct DiskCheckpoint {
  uint32_t magic;
  uint32_t spine;
  uint32_t anchor;
  uint32_t nextOffset;
  uint32_t scanned;
  uint32_t reserved;
  uint64_t scopeHash;
  uint64_t checksum;
};
static_assert(sizeof(DiskCheckpoint) == 40);

uint64_t hashBytes(uint64_t h, const void* p, size_t length) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < length; ++i) {
    h ^= b[i];
    h *= HASH_PRIME;
  }
  return h;
}

uint64_t hashText(uint64_t h, std::string_view value) {
  const uint64_t len = value.size();
  h = hashBytes(h, &len, sizeof(len));
  return hashBytes(h, value.data(), value.size());
}

uint64_t hashScope(const Scope& scope) {
  uint64_t h = hashText(HASH_SEED, "crosspoint/prefetch/v1/ru");
  h = hashText(h, scope.book);
  h = hashText(h, scope.url);
  return hashText(h, scope.model);
}

uint64_t hashKey(uint64_t scopeHash, const Sentence& s) {
  uint64_t h = hashBytes(scopeHash, &s.spine, sizeof(s.spine));
  h = hashBytes(h, &s.start, sizeof(s.start));
  return hashText(h, s.source);
}

std::string hex(uint64_t hash) {
  char buffer[17];
  snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
  return buffer;
}

std::string directory(uint64_t id) { return std::string(ROOT) + "/" + hex(id); }

std::string filePath(uint64_t scopeHash, uint64_t keyHash) {
  return directory(scopeHash) + "/" + hex(keyHash) + ".bin";
}

bool safeSentence(const Sentence& s, bool requireTranslation) {
  if (s.source.empty() || s.source.size() > MAX_SENTENCE_BYTES || s.words.size() > MAX_WORDS) return false;
  if (requireTranslation && ((s.translation.empty() && s.words.empty()) || s.translation.size() > MAX_TRANSLATION))
    return false;
  if (!requireTranslation) return true;
  uint32_t previousOffset = 0;
  bool first = true;
  for (const auto& w : s.words) {
    if (w.source.empty() || w.source.size() > MAX_WORD_BYTES || w.translation.empty() ||
        w.translation.size() > MAX_WORD_TRANSLATION || w.offset < s.start || w.offset >= s.end ||
        (!first && w.offset < previousOffset))
      return false;
    previousOffset = w.offset;
    first = false;
  }
  return true;
}

bool readPart(HalFile& file, std::string& out, size_t length, uint64_t& checksum) {
  out.resize(length);
  if (length && file.read(out.data(), length) != static_cast<int>(length)) return false;
  checksum = hashBytes(checksum, out.data(), length);
  return true;
}

bool writePart(HalFile& file, std::string_view bytes) {
  return bytes.empty() || file.write(bytes.data(), bytes.size()) == bytes.size();
}

bool readRecord(const Scope& scope, const Sentence& needle, Sentence& loaded) {
  if (!safeSentence(needle, false)) return false;
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < 20 * 1024 || heap.largestBlockBytes < 8 * 1024) return false;
  const uint64_t scopeHash = hashScope(scope);
  const uint64_t keyHash = hashKey(scopeHash, needle);
  const std::string path = filePath(scopeHash, keyHash);
  // A cache miss is expected; do not emit an SD open error for every lookup.
  if (!Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("AI_PRE", path, file)) return false;
  Header header{};
  bool ok = file.read(&header, sizeof(header)) == sizeof(header) && header.magic == RECORD_MAGIC &&
            header.scopeHash == scopeHash && header.keyHash == keyHash && header.spine == needle.spine &&
            header.offset == needle.start && header.sourceBytes == needle.source.size() &&
            (header.translatedBytes > 0 || header.wordCount > 0) && header.translatedBytes <= MAX_TRANSLATION &&
            header.wordCount <= MAX_WORDS &&
            file.fileSize() <= sizeof(header) + MAX_SENTENCE_BYTES + MAX_TRANSLATION +
                                   MAX_WORDS * (sizeof(WordHeader) + MAX_WORD_BYTES + MAX_WORD_TRANSLATION);
  if (!ok) return false;

  loaded.spine = header.spine;
  loaded.start = header.offset;
  loaded.end = needle.end;
  uint64_t checksum = HASH_SEED;
  ok = readPart(file, loaded.source, header.sourceBytes, checksum) && loaded.source == needle.source &&
       readPart(file, loaded.translation, header.translatedBytes, checksum);
  if (!ok) return false;
  loaded.words.reserve(header.wordCount);
  uint32_t previousOffset = 0;
  for (uint32_t i = 0; i < header.wordCount; ++i) {
    WordHeader wh{};
    if (file.read(&wh, sizeof(wh)) != sizeof(wh) || !wh.sourceBytes || wh.sourceBytes > MAX_WORD_BYTES ||
        !wh.translatedBytes || wh.translatedBytes > MAX_WORD_TRANSLATION || wh.offset < needle.start ||
        wh.offset >= needle.end || (i && wh.offset < previousOffset))
      return false;
    checksum = hashBytes(checksum, &wh, sizeof(wh));
    Word word;
    word.offset = wh.offset;
    if (!readPart(file, word.source, wh.sourceBytes, checksum) ||
        !readPart(file, word.translation, wh.translatedBytes, checksum))
      return false;
    previousOffset = wh.offset;
    loaded.words.push_back(std::move(word));
  }
  // Reject trailing garbage or truncated/partially written records.
  return file.position() == file.fileSize() && checksum == header.payloadHash;
}

}  // namespace

bool has(const Scope& scope, const Sentence& sentence) {
  Sentence loaded;
  return readRecord(scope, sentence, loaded);
}

bool pending(const Scope& scope, const Sentence& fullSentence, Sentence& delta) {
  if (!safeSentence(fullSentence, false)) return false;
  delta = fullSentence;
  delta.translation.clear();
  Sentence stored;
  if (readRecord(scope, fullSentence, stored)) {
    delta.translation = stored.translation;
    delta.words.clear();
    for (const Word& expected : fullSentence.words) {
      const auto translated = std::find_if(stored.words.begin(), stored.words.end(), [&](const Word& saved) {
        return saved.offset == expected.offset && saved.source == expected.source && !saved.translation.empty();
      });
      if (translated == stored.words.end()) {
        Word missing = expected;
        missing.translation.clear();
        delta.words.push_back(std::move(missing));
      }
    }
  } else {
    for (Word& word : delta.words) word.translation.clear();
  }
  // Complete sentence and full word map are already durable.
  return delta.translation.empty() || !delta.words.empty();
}

bool lookup(const Scope& scope, const Sentence& sentence, uint32_t selectedOffset, std::string_view selectedWord,
            std::string& translatedWord, std::string& translatedSentence) {
  Sentence loaded;
  if (!readRecord(scope, sentence, loaded)) return false;
  const auto word = std::find_if(loaded.words.begin(), loaded.words.end(), [&](const Word& item) {
    return item.offset == selectedOffset && item.source == selectedWord;
  });
  if (word == loaded.words.end() || loaded.translation.empty() || word->translation.empty())
    return false;  // Never invent a word mapping or a missing sentence translation.
  translatedWord = word->translation;
  translatedSentence = loaded.translation;
  return true;
}

bool save(const Scope& scope, const Sentence& update) {
  if (!safeSentence(update, true)) return false;
  const auto internal = HalMemory::getInternalHeap();
  if (internal.freeBytes < 26 * 1024 || internal.largestBlockBytes < 12 * 1024) return false;

  // Merge with a validated prior record. In particular a partial streamed
  // reply cannot overwrite a complete word map or erase an earlier sentence.
  Sentence record;
  if (!readRecord(scope, update, record)) {
    record = update;
  } else {
    if (!update.translation.empty()) record.translation = update.translation;
    for (const Word& word : update.words) {
      auto found = std::find_if(record.words.begin(), record.words.end(), [&](const Word& old) {
        return old.offset == word.offset && old.source == word.source;
      });
      if (found == record.words.end()) {
        record.words.push_back(word);
      } else {
        found->translation = word.translation;
      }
    }
    record.end = update.end;
    std::sort(record.words.begin(), record.words.end(),
              [](const Word& lhs, const Word& rhs) { return lhs.offset < rhs.offset; });
  }
  if (!safeSentence(record, true)) return false;
  const uint64_t scopeHash = hashScope(scope);
  const uint64_t keyHash = hashKey(scopeHash, record);
  const std::string dir = directory(scopeHash);
  if (!Storage.ensureDirectoryExists(ROOT) || !Storage.ensureDirectoryExists(dir.c_str())) return false;
  const std::string dest = filePath(scopeHash, keyHash);
  const std::string temp = dest + ".tmp";

  Header header{RECORD_MAGIC,
                record.spine,
                record.start,
                static_cast<uint32_t>(record.source.size()),
                static_cast<uint32_t>(record.translation.size()),
                static_cast<uint32_t>(record.words.size()),
                scopeHash,
                keyHash,
                0};
  uint64_t checksum = hashBytes(HASH_SEED, record.source.data(), record.source.size());
  checksum = hashBytes(checksum, record.translation.data(), record.translation.size());
  for (const Word& word : record.words) {
    const WordHeader wh{word.offset, static_cast<uint16_t>(word.source.size()),
                        static_cast<uint16_t>(word.translation.size())};
    checksum = hashBytes(checksum, &wh, sizeof(wh));
    checksum = hashBytes(checksum, word.source.data(), word.source.size());
    checksum = hashBytes(checksum, word.translation.data(), word.translation.size());
  }
  header.payloadHash = checksum;

  HalFile file;
  if (!Storage.openFileForWrite("AI_PRE", temp, file)) return false;
  bool ok = file.write(&header, sizeof(header)) == sizeof(header) && writePart(file, record.source) &&
            writePart(file, record.translation);
  for (const Word& word : record.words) {
    if (!ok) break;
    const WordHeader wh{word.offset, static_cast<uint16_t>(word.source.size()),
                        static_cast<uint16_t>(word.translation.size())};
    ok = file.write(&wh, sizeof(wh)) == sizeof(wh) && writePart(file, word.source) && writePart(file, word.translation);
  }
  if (ok) file.flush();
  file.close();
  if (ok && Storage.replaceFile(temp.c_str(), dest.c_str())) return true;
  Storage.remove(temp.c_str());
  return false;
}

bool loadCheckpoint(const Scope& scope, Checkpoint& out) {
  const uint64_t scopeHash = hashScope(scope);
  const std::string path = directory(scopeHash) + "/checkpoint.bin";
  if (!Storage.exists(path.c_str())) return false;  // No checkpoint on first use is normal.
  HalFile file;
  if (!Storage.openFileForRead("AI_PRE", path, file)) return false;
  DiskCheckpoint c{};
  if (file.fileSize() != sizeof(c) || file.read(&c, sizeof(c)) != sizeof(c) || c.magic != CHECKPOINT_MAGIC ||
      c.scopeHash != scopeHash || c.reserved != 0 || c.scanned > WINDOW_SENTENCES || c.nextOffset < c.anchor)
    return false;
  const uint64_t check = hashBytes(HASH_SEED, &c, sizeof(c) - sizeof(c.checksum));
  if (check != c.checksum) return false;
  out = {c.spine, c.anchor, c.nextOffset, static_cast<uint16_t>(c.scanned)};
  return true;
}

bool saveCheckpoint(const Scope& scope, const Checkpoint& c) {
  if (c.scanned > WINDOW_SENTENCES || c.nextOffset < c.anchor) return false;
  const uint64_t scopeHash = hashScope(scope);
  const std::string dir = directory(scopeHash);
  if (!Storage.ensureDirectoryExists(ROOT) || !Storage.ensureDirectoryExists(dir.c_str())) return false;
  DiskCheckpoint disk{CHECKPOINT_MAGIC, c.spine, c.anchor, c.nextOffset, c.scanned, 0, scopeHash, 0};
  disk.checksum = hashBytes(HASH_SEED, &disk, sizeof(disk) - sizeof(disk.checksum));
  const std::string dest = dir + "/checkpoint.bin";
  const std::string temp = dest + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("AI_PRE", temp, file)) return false;
  const bool ok = file.write(&disk, sizeof(disk)) == sizeof(disk);
  if (ok) file.flush();
  file.close();
  if (ok && Storage.replaceFile(temp.c_str(), dest.c_str())) return true;
  Storage.remove(temp.c_str());
  return false;
}
}  // namespace ai_prefetch
