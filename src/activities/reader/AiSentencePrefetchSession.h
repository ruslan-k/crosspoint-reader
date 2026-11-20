#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "util/AiPrefetchCache.h"

class Page;
class Section;
class TextBlock;

// Used only while the modal Translate activity is on screen. EPUB extraction
// happens in short main-loop ticks; async HTTPS uses Translate's existing
// task stack, not a second FreeRTOS worker.
class AiSentencePrefetchSession {
 public:
  AiSentencePrefetchSession(ai_prefetch::Scope scope, Section* section, uint32_t spine, int firstPage,
                            uint32_t selectedWordOffset);
  ~AiSentencePrefetchSession();

  void tick();
  void cancel();

  struct Job;  // Owns the HTTPS handle and request/response buffers.

 private:
  struct Token {
    std::string text;  // Copy text before releasing Page.
    uint32_t start = 0;
    uint32_t end = 0;
    bool paragraph = false;
    bool discretionaryHyphen = false;
  };
  bool openPage();
  void acceptToken(const Token& token);
  void completeSentence();
  void finishScan();
  bool launchBatch();
  void pollBatch();
  void consumeStream();
  void acceptStreamObject(std::string_view text);

  ai_prefetch::Scope scope_;
  Section* section_ = nullptr;
  uint32_t spine_ = 0;
  uint32_t anchor_ = 0;
  int nextPage_ = 0;
  uint16_t considered_ = 0;
  uint32_t lastWordEnd_ = 0;
  bool scanEnded_ = false;
  bool disabled_ = false;
  bool droppedSentence_ = false;
  bool stopped_ = false;
  bool reportedMemoryPause_ = false;
  std::vector<Token> pageTokens_;
  size_t tokenIndex_ = 0;
  ai_prefetch::Sentence current_;
  std::vector<ai_prefetch::Sentence> batch_;
  std::unique_ptr<Job> worker_;
};
