#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "util/AiApiEndpoint.h"

// No dynamic allocation; apply the same bounded budget to OpenAI-compatible
// chat-completions and Gemini. Completion budgets may include reasoning tokens.
namespace ai_completion {
// Single policy shared by foreground and prefetch. Other endpoints retain the
// user's web-configured effort; Gemini has no chat-completions effort field.
inline bool isHy4Go(const std::string& model, const std::string& url) {
  return model == "hy4-flash-free" && ai_endpoint::isOpenCodeGoUrl(url);
}

inline const char* reasoningEffort(const std::string& model, const std::string& url,
                                   const std::string& configured) {
  return isHy4Go(model, url) ? "low" : configured.c_str();
}

constexpr uint32_t maxOutputTokens(uint8_t attempt) { return attempt == 0 ? 2048u : 4096u; }

constexpr bool isTokenLimit(std::string_view finishReason) {
  return finishReason == "length" || finishReason == "MAX_TOKENS";
}

constexpr bool shouldRetry(std::string_view finishReason, uint8_t attempt) {
  return attempt == 0 && isTokenLimit(finishReason);
}
}  // namespace ai_completion
