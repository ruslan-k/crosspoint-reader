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

// Owned by the Translate popup. EPUB extraction runs in short UI ticks;
// blocking HTTPS runs only in a dedicated PSRAM worker. A global busy gate
// keeps BLE/WiFi teardown out of the active worker's TLS lifetime.
class AiSentencePrefetchSession {
 public:
  AiSentencePrefetchSession(ai_prefetch::Scope scope, Section* section, uint32_t spine, int firstPage,
                            uint32_t selectedWordOffset);
  ~AiSentencePrefetchSession();

  void tick();
  // Returns true if the worker took ownership of a requested WiFi shutdown.
  bool cancel(bool shutdownWifi = false);
  // Safe to query from the reader's BLE policy without an activity pointer.
  static bool httpWorkerBusy();

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
  static void acceptStreamObject(Job& job, std::string_view text);
  static void consumeStream(Job& job);
  static void flushDeferred(Job& job);
  static void runBatch(void* data);

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
  std::shared_ptr<Job> worker_;
};
