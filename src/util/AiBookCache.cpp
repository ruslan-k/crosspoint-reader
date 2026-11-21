#include "AiBookCache.h"

#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace ai_book_cache {
namespace {

// CPAICPK1 v1: all integers are little-endian; no C++ structure is cast
// directly onto disk bytes. See docs/ai-cache-format.md.
constexpr char MAGIC[8] = {'C', 'P', 'A', 'I', 'C', 'P', 'K', '1'};
constexpr uint32_t VERSION = 1;
constexpr uint32_t FLAG_COMPLETE = 1;
constexpr uint64_t HEADER_BYTES = 96;
constexpr uint64_t INDEX_BYTES = 24;
constexpr uint32_t MAX_ENTRIES = 200000;
constexpr uint32_t MAX_SOURCE = 2048;
constexpr uint32_t MAX_TRANSLATED_SENTENCE = 4096;
constexpr uint32_t MAX_WORD_SOURCE = 160;
constexpr uint32_t MAX_WORD_TRANSLATION = 256;
constexpr uint32_t MAX_WORDS = 48;
constexpr uint32_t MAX_RECORD = 16 + MAX_SOURCE + MAX_TRANSLATED_SENTENCE +
                                MAX_WORDS * (8 + MAX_WORD_SOURCE + MAX_WORD_TRANSLATION);
constexpr uint64_t FNV_OFFSET = UINT64_C(14695981039346656037);
constexpr uint64_t FNV_PRIME = UINT64_C(1099511628211);

struct Header {
  uint8_t bookSha[32]{};
  uint32_t flags = 0;
  uint32_t count = 0;
  uint64_t dataOffset = 0;
  uint64_t fileBytes = 0;
};

struct Index {
  uint64_t hash = 0;
  uint64_t offset = 0;
  uint32_t length = 0;
  uint32_t crc = 0;
};

struct RecentBook {
  std::string path;
  uint64_t bytes = 0;
  uint32_t mtime = 0;
  uint8_t digest[32]{};
  bool hashed = false;
  bool complete = false;
};

// Accessed on the UI task only, including from the reader's background tick.
// A book file edit with unchanged size and FAT mtime may require a reboot.
RecentBook recent;
// Atomic package publication relative to on-device lookup. Do not hold this
// while downloading or validating .part; those run independently on the worker.
std::mutex packageMutex;

uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) {
  return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

uint32_t crcUpdate(uint32_t state, const void* data, size_t count) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < count; ++i) {
    state ^= p[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      state = (state >> 1) ^ ((state & 1) ? UINT32_C(0xedb88320) : 0);
  }
  return state;
}
uint32_t crc32(const void* data, size_t count) {
  return crcUpdate(UINT32_C(0xffffffff), data, count) ^ UINT32_C(0xffffffff);
}

bool readAt(HalFile& f, uint64_t offset, void* data, size_t bytes) {
  return f.seek64(offset) && f.read(data, bytes) == static_cast<int>(bytes);
}

// Fold only whitespace. Do not lowercase, transliterate, normalize Unicode
// forms, or strip punctuation: Python and ESP32 must produce identical UTF-8.
std::string normalize(std::string_view source) {
  std::string out;
  out.reserve(source.size());
  bool space = false;
  for (size_t i = 0; i < source.size();) {
    const auto c = static_cast<uint8_t>(source[i]);
    size_t whitespace = 0;
    if (c == ' ' || (c >= 9 && c <= 13)) whitespace = 1;
    else if (i + 1 < source.size() && c == 0xc2 && static_cast<uint8_t>(source[i + 1]) == 0xa0)
      whitespace = 2;  // NBSP
    else if (i + 2 < source.size() && c == 0xe2 && static_cast<uint8_t>(source[i + 1]) == 0x80 &&
             ((static_cast<uint8_t>(source[i + 2]) >= 0x80 &&
               static_cast<uint8_t>(source[i + 2]) <= 0x8b) ||
              static_cast<uint8_t>(source[i + 2]) == 0xaf))
      whitespace = 3;  // U+2000..U+200B, U+202F
    else if (i + 2 < source.size() && c == 0xe3 && static_cast<uint8_t>(source[i + 1]) == 0x80 &&
             static_cast<uint8_t>(source[i + 2]) == 0x80)
      whitespace = 3;  // U+3000
    if (whitespace) {
      if (!out.empty()) space = true;
      i += whitespace;
    } else {
      if (space) out.push_back(' ');
      space = false;
      out.push_back(source[i++]);
    }
  }
  return out;
}

uint64_t sentenceHash(std::string_view source) {
  uint64_t h = FNV_OFFSET;
  for (unsigned char b : source) {
    h ^= b;
    h *= FNV_PRIME;
  }
  return h;
}

bool parseHeader(HalFile& f, Header& out) {
  uint8_t raw[HEADER_BYTES];
  if (f.fileSize64() < HEADER_BYTES || !readAt(f, 0, raw, sizeof(raw)) ||
      memcmp(raw, MAGIC, sizeof(MAGIC)) != 0 || le32(raw + 8) != VERSION ||
      (le32(raw + 12) & ~FLAG_COMPLETE) != 0 ||
      le32(raw + 52) != 0 || crc32(raw, 92) != le32(raw + 92))
    return false;
  out.flags = le32(raw + 12);
  memcpy(out.bookSha, raw + 16, sizeof(out.bookSha));
  out.count = le32(raw + 48);
  const uint64_t indexOffset = le64(raw + 56);
  const uint64_t indexBytes = le64(raw + 64);
  out.dataOffset = le64(raw + 72);
  out.fileBytes = le64(raw + 80);
  if (!out.count || out.count > MAX_ENTRIES || indexOffset != HEADER_BYTES ||
      indexBytes != out.count * INDEX_BYTES ||
      out.dataOffset != HEADER_BYTES + indexBytes ||
      out.fileBytes != f.fileSize64() || out.dataOffset >= out.fileBytes)
    return false;

  // Validate the entire fixed-width, sorted index before allowing COMPLETE
  // to disable prefetch. This is a bounded sequential SD scan, not a RAM index.
  uint32_t crc = UINT32_C(0xffffffff);
  uint64_t lastHash = 0;
  // Batch adjacent entries: one SD read per 64 rows, rather than thousands
  // of separate 24-byte SPI transactions on a long book.
  uint8_t rows[64 * INDEX_BYTES];
  for (uint32_t base = 0; base < out.count; base += 64) {
    const uint32_t count = std::min<uint32_t>(64, out.count - base);
    const size_t bytes = count * INDEX_BYTES;
    if (f.read(rows, bytes) != static_cast<int>(bytes)) return false;
    crc = crcUpdate(crc, rows, bytes);
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* row = rows + i * INDEX_BYTES;
      const uint64_t key = le64(row);
      const uint64_t offset = le64(row + 8);
      const uint32_t length = le32(row + 16);
      if ((base + i && key < lastHash) || length < 16 || length > MAX_RECORD ||
          offset < out.dataOffset || offset > out.fileBytes ||
          length > out.fileBytes - offset)
        return false;
      lastHash = key;
    }
  }
  return (crc ^ UINT32_C(0xffffffff)) == le32(raw + 88);
}

bool sha256Book(const std::string& bookPath, uint8_t (&digest)[32]) {
  HalFile epub;
  if (!Storage.openFileForRead("AI_PACK", bookPath, epub) || epub.isDirectory()) return false;
  const uint64_t expected = epub.fileSize64();
  if (!expected) return false;
  Sha256 ctx{};
  if (wc_InitSha256(&ctx) != 0) return false;
  std::array<uint8_t, 2048> buffer{};
  uint64_t done = 0;
  bool ok = true;
  while (done < expected) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), expected - done));
    if (epub.read(buffer.data(), chunk) != static_cast<int>(chunk) ||
        wc_Sha256Update(&ctx, buffer.data(), static_cast<uint32_t>(chunk)) != 0) {
      ok = false;
      break;
    }
    done += chunk;
  }
  if (ok) ok = wc_Sha256Final(&ctx, digest) == 0;
  wc_Sha256Free(&ctx);
  return ok && done == expected;
}

bool currentBookDigest(const std::string& bookPath, uint8_t (&digest)[32]) {
  HalFile epub;
  if (!Storage.openFileForRead("AI_PACK", bookPath, epub) || epub.isDirectory()) return false;
  const auto bytes = epub.fileSize64();
  const auto mtime = epub.modificationTime();
  epub.close();
  if (recent.path != bookPath || recent.bytes != bytes || recent.mtime != mtime) {
    recent.path = bookPath;
    recent.bytes = bytes;
    recent.mtime = mtime;
    recent.hashed = false;
    recent.complete = false;
  }
  if (!recent.hashed) {
    if (!sha256Book(bookPath, recent.digest)) return false;
    recent.hashed = true;
  }
  memcpy(digest, recent.digest, 32);
  return true;
}

Index decodeIndex(const uint8_t* row) {
  return {le64(row), le64(row + 8), le32(row + 16), le32(row + 20)};
}

bool consume(HalFile& file, void* output, size_t bytes, uint64_t& remaining, uint32_t& crc) {
  if (bytes > remaining || (bytes && file.read(output, bytes) != static_cast<int>(bytes))) return false;
  remaining -= bytes;
  crc = crcUpdate(crc, output, bytes);
  return true;
}

bool skip(HalFile& file, size_t bytes, uint64_t& remaining, uint32_t& crc) {
  uint8_t buffer[128];
  while (bytes) {
    const size_t n = std::min(bytes, sizeof(buffer));
    if (!consume(file, buffer, n, remaining, crc)) return false;
    bytes -= n;
  }
  return true;
}

enum class ReadStatus { Hit, WrongSource, Corrupt };

// Read only a single record. No entire-book allocations; check its CRC before
// exposing any translated text. Duplicate hash collisions verify source bytes.
ReadStatus readRecord(HalFile& file, const Index& index, std::string_view source,
                      std::string& sentenceTranslation) {
  if (!file.seek64(index.offset)) return ReadStatus::Corrupt;
  uint64_t remaining = index.length;
  uint32_t crc = UINT32_C(0xffffffff);
  uint8_t rh[16];
  if (!consume(file, rh, sizeof(rh), remaining, crc)) return ReadStatus::Corrupt;
  const uint32_t sourceBytes = le32(rh);
  const uint32_t translatedBytes = le32(rh + 4);
  const uint16_t wordCount = le16(rh + 8);
  if (!sourceBytes || sourceBytes > MAX_SOURCE || !translatedBytes ||
      translatedBytes > MAX_TRANSLATED_SENTENCE || wordCount > MAX_WORDS ||
      le16(rh + 10) != 0 || le32(rh + 12) != 0 ||
      static_cast<uint64_t>(sourceBytes) + translatedBytes > remaining)
    return ReadStatus::Corrupt;

  std::string candidateSource(sourceBytes, '\0');
  if (!consume(file, candidateSource.data(), sourceBytes, remaining, crc)) return ReadStatus::Corrupt;
  if (candidateSource != source) return ReadStatus::WrongSource;
  std::string sentenceOut(translatedBytes, '\0');
  if (!consume(file, sentenceOut.data(), translatedBytes, remaining, crc)) return ReadStatus::Corrupt;

  // Old v1 records may contain word entries. Validate their structure and
  // entire-record CRC, but never require a selected-word mapping for a hit.
  for (uint16_t i = 0; i < wordCount; ++i) {
    uint8_t wh[8];
    if (!consume(file, wh, sizeof(wh), remaining, crc)) return ReadStatus::Corrupt;
    const uint16_t sourceSize = le16(wh + 2);
    const uint16_t translationSize = le16(wh + 4);
    if (le16(wh + 6) != 0 || !sourceSize || sourceSize > MAX_WORD_SOURCE ||
        !translationSize || translationSize > MAX_WORD_TRANSLATION ||
        static_cast<uint64_t>(sourceSize) + translationSize > remaining ||
        !skip(file, static_cast<size_t>(sourceSize) + translationSize, remaining, crc))
      return ReadStatus::Corrupt;
  }
  if (remaining || (crc ^ UINT32_C(0xffffffff)) != index.crc) return ReadStatus::Corrupt;
  sentenceTranslation = std::move(sentenceOut);
  return ReadStatus::Hit;
}

}  // namespace

bool sha256File(const std::string& path, uint8_t digest[32]) {
  uint8_t raw[32]{};
  if (!sha256Book(path, raw)) return false;
  memcpy(digest, raw, sizeof(raw));
  return true;
}

bool validatePackage(const std::string& candidatePath, const uint8_t expectedBookSha[32], bool& complete) {
  complete = false;
  HalFile file;
  if (!Storage.openFileForRead("AI_SRV", candidatePath, file) || file.isDirectory()) return false;
  Header header;
  if (!parseHeader(file, header) || memcmp(header.bookSha, expectedBookSha, 32) != 0) return false;
  complete = (header.flags & FLAG_COMPLETE) != 0;
  return true;
}

bool needsServer(const std::string& bookPath) {
  std::lock_guard<std::mutex> guard(packageMutex);
  HalFile file;
  if (!Storage.openFileForRead("AI_SRV", bookPath + ".aic", file) || file.isDirectory()) return true;
  uint8_t prefix[16]{};
  if (!readAt(file, 0, prefix, sizeof(prefix))) return true;
  return memcmp(prefix, MAGIC, sizeof(MAGIC)) != 0 || le32(prefix + 8) != VERSION ||
         (le32(prefix + 12) & FLAG_COMPLETE) == 0;
}

bool installPackage(const std::string& candidatePath, const std::string& bookPath) {
  std::lock_guard<std::mutex> guard(packageMutex);
  return Storage.replaceFile(candidatePath.c_str(), (bookPath + ".aic").c_str());
}

bool knownComplete(const std::string& bookPath) {
  return recent.hashed && recent.complete && recent.path == bookPath;
}

const char* resultName(Result result) {
  switch (result) {
    case Result::Hit: return "hit";
    case Result::NoPackage: return "package_missing";
    case Result::InvalidPackage: return "invalid_header_or_index";
    case Result::BookMismatch: return "book_id_mismatch";
    case Result::LowMemory: return "low_memory";
    case Result::SentenceMissing: return "sentence_missing";
    case Result::CorruptRecord: return "record_crc_or_length";
  }
  return "unknown";
}

Result lookupSentence(const std::string& bookPath, std::string_view sentence,
                      std::string& translatedSentence, bool& complete) {
  std::lock_guard<std::mutex> guard(packageMutex);
  complete = false;
  translatedSentence.clear();
  if (sentence.empty() || sentence.size() > MAX_SOURCE) return Result::SentenceMissing;
  const std::string packagePath = bookPath + ".aic";
  if (!Storage.exists(packagePath.c_str())) {
    if (recent.path == bookPath) recent.complete = false;
    LOG_INF("AI_PACK", "not found (.epub.aic alongside book)");
    return Result::NoPackage;
  }
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < 24 * 1024 || heap.largestBlockBytes < 8 * 1024)
    return Result::LowMemory;
  HalFile file;
  if (!Storage.openFileForRead("AI_PACK", packagePath, file) || file.isDirectory())
    return Result::InvalidPackage;
  Header header;
  if (!parseHeader(file, header)) {
    if (recent.path == bookPath) recent.complete = false;
    LOG_INF("AI_PACK", "rejected: invalid header, index or checksum");
    return Result::InvalidPackage;
  }
  uint8_t digest[32];
  if (!currentBookDigest(bookPath, digest)) return Result::InvalidPackage;
  if (memcmp(header.bookSha, digest, 32) != 0) {
    recent.complete = false;
    LOG_INF("AI_PACK", "rejected: book-id SHA-256 mismatch");
    return Result::BookMismatch;
  }
  recent.complete = (header.flags & FLAG_COMPLETE) != 0;
  complete = recent.complete;
  char id[17];
  snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x%02x%02x",
           static_cast<unsigned>(digest[0]), static_cast<unsigned>(digest[1]),
           static_cast<unsigned>(digest[2]), static_cast<unsigned>(digest[3]),
           static_cast<unsigned>(digest[4]), static_cast<unsigned>(digest[5]),
           static_cast<unsigned>(digest[6]), static_cast<unsigned>(digest[7]));
  LOG_INF("AI_PACK", "found book-id=%s records=%u complete=%u",
          id, static_cast<unsigned>(header.count), complete ? 1u : 0u);

  std::string normalized = normalize(sentence);
  if (normalized.empty() || normalized.size() > MAX_SOURCE) return Result::SentenceMissing;
  const uint64_t hash = sentenceHash(normalized);
  uint32_t lo = 0, hi = header.count;
  uint8_t row[INDEX_BYTES];
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (!readAt(file, HEADER_BYTES + static_cast<uint64_t>(mid) * INDEX_BYTES, row, sizeof(row)))
      return Result::InvalidPackage;
    if (le64(row) < hash) lo = mid + 1;
    else hi = mid;
  }
  // A deliberate hash collision can have adjacent rows. Cap adversarial runs.
  for (uint32_t n = lo; n < header.count && n - lo < 64; ++n) {
    if (!readAt(file, HEADER_BYTES + static_cast<uint64_t>(n) * INDEX_BYTES, row, sizeof(row)))
      return Result::InvalidPackage;
    const Index index = decodeIndex(row);
    if (index.hash != hash) break;
    const auto status = readRecord(file, index, normalized, translatedSentence);
    if (status == ReadStatus::Hit) return Result::Hit;
    if (status == ReadStatus::Corrupt) return Result::CorruptRecord;
  }
  LOG_INF("AI_CACHE", "miss hash=%016llx len=%u", static_cast<unsigned long long>(hash),
          static_cast<unsigned>(normalized.size()));
  return Result::SentenceMissing;
}
}  // namespace ai_book_cache
