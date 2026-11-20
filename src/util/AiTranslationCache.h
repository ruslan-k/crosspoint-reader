#pragma once

#include <string>
#include <string_view>

// A tiny, persistent, exact-match cache for BYOK translations. No RAM index:
// one bounded file read on a hit, and no filesystem access during page turns.
// The key includes book, provider, model, source word, sentence and cache version.
namespace ai_translation_cache {
bool load(std::string_view book, std::string_view url, std::string_view model, std::string_view word,
          std::string_view sentence, std::string& wordTranslation, std::string& sentenceTranslation);
bool save(std::string_view book, std::string_view url, std::string_view model, std::string_view word,
          std::string_view sentence, std::string_view wordTranslation, std::string_view sentenceTranslation);
}  // namespace ai_translation_cache
