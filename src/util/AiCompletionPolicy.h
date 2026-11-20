#pragma once

#include <cstdint>
#include <string_view>

// No dynamic allocation; apply the same bounded budget to OpenAI-compatible
// chat-completions and Gemini. Completion budgets may include reasoning tokens.
namespace ai_completion {
constexpr uint32_t maxOutputTokens(uint8_t attempt) { return attempt == 0 ? 2048u : 4096u; }

constexpr bool isTokenLimit(std::string_view finishReason) {
  return finishReason == "length" || finishReason == "MAX_TOKENS";
}

constexpr bool shouldRetry(std::string_view finishReason, uint8_t attempt) {
  return attempt == 0 && isTokenLimit(finishReason);
}
}  // namespace ai_completion
