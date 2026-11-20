#include <cassert>

#include "../../src/util/AiApiEndpoint.h"

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
}
