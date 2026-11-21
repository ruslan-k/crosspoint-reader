#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Picco: a separate, position-addressed lookahead cache. This does not change
// the existing exact word+sentence cache or its on-disk format.
namespace ai_prefetch {
inline constexpr size_t WINDOW_SENTENCES = 100;
inline constexpr size_t BATCH_SENTENCES = 4;
inline constexpr size_t MAX_SENTENCE_BYTES = 2048;
inline constexpr size_t MAX_WORDS = 48;

struct Scope {
  std::string book;
  std::string url;
  std::string model;
};

struct Word {
  uint32_t offset = 0;  // Absolute visible-codepoint offset in the spine.
  std::string source;
  std::string translation;
};

struct Sentence {
  uint32_t spine = 0;
  uint32_t start = 0;
  uint32_t end = 0;
  std::string source;
  std::string translation;
  std::vector<Word> words;
};

// All lookups validate content, scope, lengths and checksums, not just hashes.
// SD access is permitted only inside Translate (never on a page-turn path).
bool has(const Scope& scope, const Sentence& sentence);
// Sentence-only deduplication. A missing/corrupt record must be retranslated.
// Legacy stored word maps remain readable, but new requests do not need them.
bool pending(const Scope& scope, const Sentence& fullSentence, Sentence& delta);
bool lookupSentence(const Scope& scope, const Sentence& sentence, std::string& translatedSentence);
// Merge a validated sentence and/or a subset of translated words into the
// previous atomic record. Existing complete translations are never lost.
bool save(const Scope& scope, const Sentence& sentence);

struct Checkpoint {
  uint32_t spine = 0;
  uint32_t anchor = 0;
  uint32_t nextOffset = 0;
  uint16_t scanned = 0;
};

// Advisory only: deduplication MUST always verify each individual record.
// The checkpoint is atomic and supports resume/diagnostics after power loss.
bool loadCheckpoint(const Scope& scope, Checkpoint& out);
bool saveCheckpoint(const Scope& scope, const Checkpoint& checkpoint);
}  // namespace ai_prefetch
