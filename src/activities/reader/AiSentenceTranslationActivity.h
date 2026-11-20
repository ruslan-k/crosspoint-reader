#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

class Section;
class AiSentencePrefetchSession;

struct AiPrefetchOrigin {
  Section* section = nullptr;  // Owned by the EPUB reader below this modal.
  uint32_t spine = 0;
  int firstPage = 0;
  uint32_t sentenceStartOffset = UINT32_MAX;
  uint32_t selectedWordOffset = UINT32_MAX;
};

// One foreground BYOK request, followed by a scrollable dictionary-style
// overlay on the retained EPUB page. No vocabulary/history modifications.
class AiSentenceTranslationActivity final : public Activity {
 public:
  AiSentenceTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string word,
                                std::string sentence, std::string bookPath, AiPrefetchOrigin origin);
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
  AiPrefetchOrigin origin_;
  std::unique_ptr<AiSentencePrefetchSession> prefetch_;
  std::string error_;
  bool requestPending_ = false;
  bool retryPending_ = false;
  uint8_t generationAttempt_ = 0;  // At most one bounded retry on token exhaustion.
  bool wifiOwned_ = false;
  uint8_t* background_ = nullptr;  // Optional PSRAM framebuffer snapshot.
  std::string wordTranslation_;
  std::string sentenceTranslation_;
  std::vector<PopupLine> popupLines_;
  int popupScroll_ = 0;
  int popupVisibleLines_ = 1;
};
