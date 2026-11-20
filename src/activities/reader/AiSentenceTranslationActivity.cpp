#include "AiSentenceTranslationActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <TrustedTime.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

#include "AITranslationStore.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/AiTranslationRoots.h"

namespace {

constexpr size_t MAX_RESPONSE_BYTES = 8 * 1024;
constexpr size_t MAX_SENTENCE_BYTES = 2048;
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

std::string trim(std::string s) {
  const auto space = [](const unsigned char c) { return std::isspace(c); };
  while (!s.empty() && space(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t start = 0;
  while (start < s.size() && space(static_cast<unsigned char>(s[start]))) ++start;
  if (start) s.erase(0, start);
  return s;
}

bool safeEndpoint(const std::string& url) {
  if (url.size() < 12 || url.size() > 255 || url.compare(0, 8, "https://") != 0 ||
      url.find_first_of("?#@\r\n\t ") != std::string::npos) return false;
  return url.find('/', 8) != 8 && url.find(':', 8) != 8;
}

bool extractLabeledTranslation(const std::string& output, std::string& word, std::string& sentence) {
  // The response body is a user-configured model's output, so do not assume
  // it obeyed the format or try to treat it as a trusted markup document.
  const size_t w = output.find("WORD:");
  if (w == std::string::npos) return false;
  const size_t lineEnd = output.find('\n', w);
  if (lineEnd == std::string::npos) return false;
  const size_t s = output.find("SENTENCE:", lineEnd);
  if (s == std::string::npos) return false;
  word = trim(output.substr(w + 5, lineEnd - w - 5));
  sentence = trim(output.substr(s + 9));
  if (word.empty() || sentence.empty() || word.size() > 256 || sentence.size() > 4096) return false;
  // Strip a stray closing fence, but never interpret content as HTML.
  const size_t fence = sentence.find("\n```");
  if (fence != std::string::npos) sentence.resize(fence);
  return !sentence.empty();
}

}  // namespace

AiSentenceTranslationActivity::AiSentenceTranslationActivity(GfxRenderer& renderer,
                                                             MappedInputManager& mappedInput,
                                                             std::string word, std::string sentence)
    : Activity("AiSentenceTranslation", renderer, mappedInput),
      word_(std::move(word)),
      sentence_(std::move(sentence)) {}

void AiSentenceTranslationActivity::fail(std::string reason) {
  error_ = std::move(reason);
  state_ = State::Error;
  requestUpdate();
}

void AiSentenceTranslationActivity::onEnter() {
  Activity::onEnter();
  if (!AI_TRANSLATION_STORE.loadFromFile() || !AI_TRANSLATION_STORE.configured()) {
    fail(tr(STR_AI_NOT_CONFIGURED));
    return;
  }
  if (sentence_.empty() || sentence_.size() > MAX_SENTENCE_BYTES || word_.empty()) {
    fail(tr(STR_AI_BAD_RESPONSE));
    return;
  }

  if (auto* fontCache = renderer.getFontCacheManager()) fontCache->releaseSdFontCaches();

  if (WiFi.status() == WL_CONNECTED) {
    state_ = State::Translating;
    requestPending_ = true;
    requestUpdate();
    return;
  }

  wifiOwned_ = true;
  state_ = State::Connecting;
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true, true);
  if (!wifi) {
    fail(tr(STR_AI_LOW_MEMORY));
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      // Manual Wi-Fi selection cancelled: return directly to the EPUB reader.
      finish();
    } else if (WiFi.status() != WL_CONNECTED) {
      fail(tr(STR_AI_NETWORK_FAILED));
    } else {
      state_ = State::Translating;
      requestPending_ = true;
      requestUpdate();
    }
  });
}

bool AiSentenceTranslationActivity::translate(std::string& wordTranslation, std::string& sentenceTranslation) {
  if (!trustedtime::trustedNow() && !trustedtime::syncNow(5000)) {
    fail(tr(STR_AI_CLOCK_ERROR));
    return false;
  }
  if (auto* fontCache = renderer.getFontCacheManager()) fontCache->releaseSdFontCaches();
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < MIN_FREE_FOR_TLS || heap.largestBlockBytes < MIN_BLOCK_FOR_TLS) {
    fail(tr(STR_AI_LOW_MEMORY));
    return false;
  }

  const auto& config = AI_TRANSLATION_STORE;
  std::string url = config.getUrl();
  if (!safeEndpoint(url)) {
    fail(tr(STR_AI_NOT_CONFIGURED));
    return false;
  }
  const bool gemini = url == "https://generativelanguage.googleapis.com" ||
                      url.rfind("https://generativelanguage.googleapis.com/", 0) == 0;
  if (gemini) {
    if (config.getModel().find('/') != std::string::npos ||
        config.getModel().find(':') != std::string::npos) {
      fail(tr(STR_AI_NOT_CONFIGURED));
      return false;
    }
    const size_t models = url.find("/models/");
    if (models != std::string::npos) url.resize(models);
    if (url.back() == '/') url.pop_back();
    url += "/models/" + config.getModel() + ":generateContent?fields=candidates.content.parts.text";
  } else {
    if (url.back() == '/') url.pop_back();
    if (!url.ends_with("/chat/completions")) url += "/chat/completions";
  }

  const std::string prompt =
      "Translate the selected word in context and its complete source sentence into Russian. "
      "Reply with exactly two lines, without markdown or commentary:\n"
      "WORD: <Russian translation of the word>\n"
      "SENTENCE: <Fluent Russian translation of the entire sentence>\n\n"
      "Selected word: " + word_ + "\nSource sentence: " + sentence_;

  JsonDocument req;
  if (gemini) {
    JsonObject item = req["contents"].to<JsonArray>().add<JsonObject>();
    item["parts"].to<JsonArray>().add<JsonObject>()["text"] = prompt;
    req["generationConfig"].to<JsonObject>()["maxOutputTokens"] = 384;
  } else {
    req["model"] = config.getModel();
    req["max_tokens"] = 384;
    JsonObject msg = req["messages"].to<JsonArray>().add<JsonObject>();
    msg["role"] = "user";
    msg["content"] = prompt;
  }

  std::string body;
  serializeJson(req, body);
  if (req.overflowed() || body.size() > 4096) {
    fail(tr(STR_AI_LOW_MEMORY));
    return false;
  }

  freeink::SecureHttpClient http;
  http.setCACert(ai_translation::ROOTS);  // Verify peer chain AND hostname.
  http.setTimeout(30000);
  http.setUserAgent("CrossPoint-AI-Translation");
  if (!http.begin(url) || !http.connect()) {
    fail(tr(STR_AI_NETWORK_FAILED));
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  if (gemini) {
    http.addHeader("x-goog-api-key", config.getKey());
  } else {
    http.addHeader("Authorization", "Bearer " + config.getKey());
  }

  std::string response;
  response.reserve(2048);
  bool overflow = false;
  const int status = http.sendRequest(
      "POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&](const uint8_t* data, const size_t len) {
        if (len > MAX_RESPONSE_BYTES - response.size()) {
          overflow = true;
          return false;
        }
        response.append(reinterpret_cast<const char*>(data), len);
        return true;
      });
  const bool complete = http.responseComplete();
  http.end();

  if (status != 200 || overflow || !complete) {
    char code[40];
    snprintf(code, sizeof(code), "HTTP %d", status);
    LOG_ERR("AI", "Translation request failed: status=%d overflow=%d", status, overflow);
    fail(status > 0 ? code : tr(STR_AI_NETWORK_FAILED));
    return false;
  }

  JsonDocument answer;
  if (deserializeJson(answer, response) != DeserializationError::Ok) {
    fail(tr(STR_AI_BAD_RESPONSE));
    return false;
  }
  const char* output = gemini ? answer["candidates"][0]["content"]["parts"][0]["text"].as<const char*>()
                              : answer["choices"][0]["message"]["content"].as<const char*>();
  if (!output || !extractLabeledTranslation(output, wordTranslation, sentenceTranslation)) {
    fail(tr(STR_AI_BAD_RESPONSE));
    return false;
  }
  return true;
}

void AiSentenceTranslationActivity::openResult(std::string wordTranslation, std::string sentenceTranslation) {
  std::string definition;
  definition.reserve(word_.size() + wordTranslation.size() + sentence_.size() +
                     sentenceTranslation.size() + 64);
  definition += word_;
  definition += " — ";
  definition += wordTranslation;
  definition += "\n\n";
  definition += tr(STR_AI_ORIGINAL);
  definition += "\n";
  definition += sentence_;
  definition += "\n\n";
  definition += tr(STR_AI_TRANSLATION);
  definition += "\n";
  definition += sentenceTranslation;

  auto activity = makeUniqueNoThrow<DictionaryDefinitionActivity>(
      renderer, mappedInput, std::string(tr(STR_AI_TRANSLATE)), std::move(definition), false);
  if (!activity) {
    fail(tr(STR_AI_LOW_MEMORY));
    return;
  }
  state_ = State::Showing;
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { finish(); });
}

void AiSentenceTranslationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (!requestPending_) return;
  requestPending_ = false;
  state_ = State::Translating;
  requestUpdateAndWait();
  std::string wordTranslation;
  std::string sentenceTranslation;
  if (translate(wordTranslation, sentenceTranslation)) {
    openResult(std::move(wordTranslation), std::move(sentenceTranslation));
  }
}

void AiSentenceTranslationActivity::onExit() {
  Activity::onExit();
  if (wifiOwned_) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
}

void AiSentenceTranslationActivity::render(RenderLock&&) {
  // Compact progress indicator rather than a full-screen loading activity.
  // Clear a previous manual network chooser if one was shown.
  if (state_ == State::Translating) {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_AI_TRANSLATING));
    return;
  }
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{area.x, area.y, area.width, metrics.headerHeight}, tr(STR_AI_TRANSLATE));
  const char* status = state_ == State::Connecting ? tr(STR_CONNECTING)
                     : state_ == State::Translating ? tr(STR_AI_TRANSLATING)
                     : error_.c_str();
  const Rect body{area.x + metrics.contentSidePadding, area.y + metrics.headerHeight + metrics.verticalSpacing,
                  area.width - 2 * metrics.contentSidePadding,
                  area.height - metrics.headerHeight - metrics.buttonHintsHeight - metrics.verticalSpacing};
  UITheme::drawCenteredWrappedText(renderer, body, UI_12_FONT_ID, status, 6);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
