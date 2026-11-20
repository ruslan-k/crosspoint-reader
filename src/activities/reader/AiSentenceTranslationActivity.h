#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

// One foreground BYOK request, followed by a scrollable dictionary-style
// overlay on the retained EPUB page. No vocabulary/history modifications.
class AiSentenceTranslationActivity final : public Activity {
 public:
  AiSentenceTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string word,
                                std::string sentence, std::string bookPath);
  ~AiSentenceTranslationActivity() override;
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
  void captureUnderlyingPage();
  void releaseBackground();
  void preparePopupLines();
  void scrollPopup(int delta);
  void drawPopup() const;

  struct PopupLine {
    std::string text;
    bool bold = false;
  };

  State state_ = State::Connecting;
  std::string word_;
  std::string sentence_;
  std::string bookPath_;
  std::string error_;
  bool requestPending_ = false;
  bool wifiOwned_ = false;
  uint8_t* background_ = nullptr;  // Optional PSRAM framebuffer snapshot.
  std::string wordTranslation_;
  std::string sentenceTranslation_;
  std::vector<PopupLine> popupLines_;
  int popupScroll_ = 0;
  int popupVisibleLines_ = 1;
};
