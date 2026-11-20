#include <cassert>

#include "../../src/util/AiCompletionPolicy.h"
#include "../../src/util/AiSentenceUtils.h"

int main() {
  assert(ai_completion::maxOutputTokens(0) == 2048);
  assert(ai_completion::maxOutputTokens(1) == 4096);
  assert(ai_completion::shouldRetry("length", 0));
  assert(ai_completion::shouldRetry("MAX_TOKENS", 0));
  assert(!ai_completion::shouldRetry("stop", 0));
  assert(!ai_completion::shouldRetry("length", 1));
  assert(!ai_completion::shouldRetry("MAX_TOKENS", 1));
  assert(!ai_completion::isTokenLimit("content_filter"));
  assert(ai_sentence::isEnd("world."));
  assert(ai_sentence::isEnd("world!"));
  assert(ai_sentence::isEnd("world?"));
  assert(ai_sentence::isEnd("world.\""));
  assert(ai_sentence::isEnd("終了。"));
  assert(ai_sentence::isEnd("終わり！"));
  assert(ai_sentence::isEnd("世界？"));
  assert(!ai_sentence::isEnd("Dr."));
  assert(!ai_sentence::isEnd("e.g."));
  assert(!ai_sentence::isEnd("p.m."));
  assert(!ai_sentence::isEnd("U."));
  assert(!ai_sentence::isEnd("no"));
  return 0;
}
