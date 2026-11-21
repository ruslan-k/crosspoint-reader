#include <cassert>

#include "../../src/util/AiApiEndpoint.h"
#include "../../src/util/AiCompletionPolicy.h"

int main() {
  using ai_endpoint::isOpenCodeGoUrl;
  using ai_endpoint::migrateOpenCodeGoUrl;
  assert(migrateOpenCodeGoUrl("https://opencode.ai/zen/go/v1") == "https://opencode.ai/inference/go/openai/v1");
  assert(migrateOpenCodeGoUrl("https://opencode.ai/zen/go/v1/chat/completions") ==
         "https://opencode.ai/inference/go/openai/v1/chat/completions");
  assert(migrateOpenCodeGoUrl("https://opencode.ai/inference/go/openai/v1") ==
         "https://opencode.ai/inference/go/openai/v1");
  assert(migrateOpenCodeGoUrl("https://api.groq.com/openai/v1") == "https://api.groq.com/openai/v1");
  assert(migrateOpenCodeGoUrl("https://opencode.ai/zen/v1") == "https://opencode.ai/zen/v1");
  assert(migrateOpenCodeGoUrl("https://opencode.ai/zen/go/v1/some/path") == "https://opencode.ai/zen/go/v1/some/path");
  assert(isOpenCodeGoUrl("https://opencode.ai/inference/go/openai/v1"));
  assert(isOpenCodeGoUrl("https://opencode.ai/inference/go/openai/v1/chat/completions"));
  assert(!isOpenCodeGoUrl("https://opencode.ai/inference/openai/v1"));
  assert(!isOpenCodeGoUrl("https://api.groq.com/openai/v1"));

  // Both foreground and batch prefetch call this single effective effort policy.
  const std::string go = "https://opencode.ai/inference/go/openai/v1/chat/completions";
  assert(ai_completion::isHy4Go("hy4-flash-free", go));
  assert(!ai_completion::isHy4Go("other-model", go));
  assert(ai_completion::reasoningEffort("hy4-flash-free", go, "default") == std::string_view("low"));
  assert(ai_completion::reasoningEffort("hy4-flash-free", go, "high") == std::string_view("low"));
  assert(ai_completion::reasoningEffort("other-model", go, "medium") == std::string_view("medium"));
  assert(ai_completion::reasoningEffort("hy4-flash-free", "https://api.example/v1", "high") ==
         std::string_view("high"));
}
