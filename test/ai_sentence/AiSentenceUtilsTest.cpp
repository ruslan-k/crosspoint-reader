#include "../../src/util/AiSentenceUtils.h"

#include <cassert>

int main() {
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
