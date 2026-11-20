#pragma once

#include <string>

#include "activities/Activity.h"

// One foreground BYOK request, then a plain Original/Translation reader page.
// No Linga endpoints, history, related words or vocabulary writes.
class AiSentenceTranslationActivity final : public Activity {
 public:
  AiSentenceTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                std::string word, std::string sentence);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ != State::Error; }

 private:
  enum class State : uint8_t { Connecting, Translating, Showing, Error };
  bool translate(std::string& wordTranslation, std::string& sentenceTranslation);
  void fail(std::string reason);
  void openResult(std::string wordTranslation, std::string sentenceTranslation);

  State state_ = State::Connecting;
  std::string word_;
  std::string sentence_;
  std::string error_;
  bool requestPending_ = false;
  bool wifiOwned_ = false;
};
