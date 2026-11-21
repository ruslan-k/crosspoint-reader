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
  assert(ai_sentence::isRussianText("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));  // Привет
  assert(!ai_sentence::isRussianText("Hello world"));
  assert(!ai_sentence::isRussianText("Ivan"));
  assert(!ai_sentence::isRussianText("\xD0\xA2\xD0\xBE\xD0\xB2\xD0\xB0 \xD0\xB5 \xD0\xB1\xD1\x8A\xD0\xBB\xD0\xB3\xD0\xB0\xD1\x80\xD1\x81\xD0\xBA\xD0\xB8 \xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82."));  // Bulgarian
  assert(ai_sentence::isRussianText("\xD1\x81\xD1\x8A\xD0\xB5\xD0\xB7\xD0\xB4"));  // съезд
  assert(ai_sentence::isRussianLanguage("ru"));
  assert(ai_sentence::isRussianLanguage("ru-RU"));
  assert(!ai_sentence::isRussianLanguage("bg"));
  assert(!ai_sentence::isRussianLanguage("uk"));
  assert(!ai_sentence::isRussianLanguage(""));
  return 0;
}
