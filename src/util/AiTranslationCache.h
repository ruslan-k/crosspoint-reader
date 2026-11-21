#pragma once

#include <string>
#include <string_view>

// Sentence-only BYOK cache. All selected words in the same sentence share
// one validated translation; the key includes book, provider and model.
namespace ai_translation_cache {
bool load(std::string_view book, std::string_view url, std::string_view model,
          std::string_view sentence, std::string& translation);
bool save(std::string_view book, std::string_view url, std::string_view model,
          std::string_view sentence, std::string_view translation);
}  // namespace ai_translation_cache
