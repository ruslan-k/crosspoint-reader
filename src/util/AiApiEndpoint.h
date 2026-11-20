#pragma once

#include <string>

// OpenCode's legacy Go endpoint remained in older CrossPoint web-settings
// examples. Upgrade only exact known legacy URLs to the documented Console
// endpoint; never rewrite arbitrary third-party/self-hosted providers.
namespace ai_endpoint {
inline std::string migrateOpenCodeGoUrl(const std::string& url) {
  constexpr const char* OLD = "https://opencode.ai/zen/go/v1";
  constexpr const char* CURRENT = "https://opencode.ai/inference/go/openai/v1";
  const std::string oldBase(OLD);
  if (url == oldBase) return CURRENT;
  if (url == oldBase + "/chat/completions") return std::string(CURRENT) + "/chat/completions";
  return url;
}

inline bool isOpenCodeGoUrl(const std::string& url) {
  constexpr const char* BASE = "https://opencode.ai/inference/go/openai/v1";
  const std::string base(BASE);
  return url == base || url == base + "/chat/completions";
}
}  // namespace ai_endpoint
