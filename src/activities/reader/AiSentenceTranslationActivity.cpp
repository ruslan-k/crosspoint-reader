#include "AiSentenceTranslationActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <utility>

#include "AITranslationStore.h"
#include "AiSentencePrefetchSession.h"
#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/AiApiEndpoint.h"
#include "util/AiTranslationCache.h"
#include "util/AiTranslationRoots.h"

namespace {

// Picco frequently receives verbose completions from thinking models.
// Keep the HTTP body in PSRAM, not in the <=40 KiB contiguous internal heap.
constexpr size_t MAX_RESPONSE_BYTES = 48 * 1024;
constexpr size_t FALLBACK_RESPONSE_BYTES = 12 * 1024;
constexpr int AI_HTTP_TIMEOUT_MS = 90000;
constexpr size_t MAX_SENTENCE_BYTES = 2048;
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

struct AiPopupRect {
  int x, y, width, height;
};

AiPopupRect aiPopupRect(const GfxRenderer& renderer) {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int insetX = std::max(12, safe.width / 16);
  const int insetY = std::max(16, safe.height / 11);
  return {safe.x + insetX, safe.y + insetY, std::max(160, safe.width - insetX * 2),
          std::max(180, safe.height - insetY * 2)};
}

int aiPopupFont(const GfxRenderer& renderer) {
  // Ubuntu 10 includes Cyrillic; keep Noto 12 as a fallback on lean builds.
  return renderer.getFontMap().count(UI_10_FONT_ID)         ? UI_10_FONT_ID
         : renderer.getFontMap().count(NOTOSANS_12_FONT_ID) ? NOTOSANS_12_FONT_ID
                                                            : NOTOSANS_14_FONT_ID;
}

constexpr int AI_POPUP_TITLE_HEIGHT = 46;
constexpr int AI_POPUP_FOOTER_HEIGHT = 0;
constexpr int AI_POPUP_PADDING = 15;

// Do not send model output or the full provider error body to serial. Only
// machine-readable provider error identifiers are included in diagnostic logs.
std::string safeErrorId(const char* value) {
  if (!value) return "none";
  std::string safe;
  while (*value && safe.size() < 64) {
    const unsigned char c = static_cast<unsigned char>(*value++);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
        c == '.' || c == '/') {
      safe += static_cast<char>(c);
    }
  }
  return safe.empty() ? "none" : safe;
}

// A provider may echo parts of the user prompt in an error. Error messages
// appear only on the local E-Ink screen, never in logs. Explicitly hide any
// reflected API key or complete sentence, and strip terminal/control codes.
std::string safeScreenError(const char* raw, const std::string& key, const std::string& sentence) {
  if (!raw) return "";
  const std::string full(raw);
  if ((!key.empty() && full.find(key) != std::string::npos) ||
      (sentence.size() >= 6 && full.find(sentence) != std::string::npos)) {
    return "Provider error (details hidden)";
  }
  std::string result;
  for (const unsigned char c : full) {
    if (result.size() >= 180) break;
    if (c >= 32 && c != 127) result += static_cast<char>(c);
  }
  return result;
}

// Bounded response buffer: an unexpectedly large provider response must not
// exhaust internal RAM while ESP-IDF/mbedTLS is holding the TLS session.
struct AiHttpResponse {
  char* data = nullptr;
  size_t used = 0;
  size_t capacity = 0;
  bool overflow = false;

  AiHttpResponse() {
    data = static_cast<char*>(heap_caps_malloc(MAX_RESPONSE_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    capacity = data ? MAX_RESPONSE_BYTES : 0;
    if (!data && HalMemory::getDefaultHeap().largestBlockBytes > FALLBACK_RESPONSE_BYTES + 8192) {
      data = static_cast<char*>(heap_caps_malloc(FALLBACK_RESPONSE_BYTES + 1, MALLOC_CAP_8BIT));
      capacity = data ? FALLBACK_RESPONSE_BYTES : 0;
    }
    if (data) data[0] = '\0';
  }
  ~AiHttpResponse() { heap_caps_free(data); }
  AiHttpResponse(const AiHttpResponse&) = delete;
  AiHttpResponse& operator=(const AiHttpResponse&) = delete;
};

esp_err_t onAiHttpEvent(esp_http_client_event_t* event) {
  if (!event || event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* response = static_cast<AiHttpResponse*>(event->user_data);
  if (!response || !response->data || event->data_len < 0) return ESP_FAIL;
  const size_t bytes = static_cast<size_t>(event->data_len);
  if (bytes > response->capacity - response->used) {
    response->overflow = true;
    return ESP_FAIL;
  }
  memcpy(response->data + response->used, event->data, bytes);
  response->used += bytes;
  response->data[response->used] = '\0';
  return ESP_OK;
}

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
      url.find_first_of("?#@\r\n\t ") != std::string::npos)
    return false;
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

// Backward-compatible response decoding for providers that return either the
// new compact JSON payload or the old WORD:/SENTENCE: two-line form.
bool extractTranslationText(const std::string& raw, std::string& word, std::string& sentence) {
  const std::string output = trim(raw);
  const size_t start = output.find('{');
  const size_t end = output.rfind('}');
  if (start != std::string::npos && end != std::string::npos && end >= start && end - start < 8192) {
    JsonDocument translated;
    if (deserializeJson(translated, output.substr(start, end - start + 1)) == DeserializationError::Ok) {
      const char* w = translated["word_translation"].as<const char*>();
      const char* t = translated["sentence_translation"].as<const char*>();
      if (w && t) {
        word = trim(w);
        sentence = trim(t);
        if (!word.empty() && !sentence.empty() && word.size() <= 256 && sentence.size() <= 4096) return true;
      }
    }
  }
  return extractLabeledTranslation(output, word, sentence);
}

}  // namespace

AiSentenceTranslationActivity::AiSentenceTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::string word, std::string sentence,
                                                             std::string bookPath, AiPrefetchOrigin origin)
    : Activity("AiSentenceTranslation", renderer, mappedInput),
      word_(std::move(word)),
      sentence_(std::move(sentence)),
      bookPath_(std::move(bookPath)),
      origin_(origin) {}

AiSentenceTranslationActivity::~AiSentenceTranslationActivity() { releaseBackground(); }

void AiSentenceTranslationActivity::captureUnderlyingPage() {
  releaseBackground();
  // Capture the actual EPUB/selection framebuffer before Wi-Fi screens replace
  // it, so every redraw can paint an opaque modal over the retained page.
  background_ = static_cast<uint8_t*>(heap_caps_malloc(HalDisplay::BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!background_ && HalMemory::getDefaultHeap().largestBlockBytes > HalDisplay::BUFFER_SIZE + 8192) {
    background_ = static_cast<uint8_t*>(heap_caps_malloc(HalDisplay::BUFFER_SIZE, MALLOC_CAP_8BIT));
  }
  if (background_ && renderer.getWriteTarget()) {
    memcpy(background_, renderer.getWriteTarget(), HalDisplay::BUFFER_SIZE);
  } else {
    releaseBackground();  // White fallback for memory-limited boards.
  }
}

void AiSentenceTranslationActivity::releaseBackground() {
  if (background_) heap_caps_free(background_);
  background_ = nullptr;
}

void AiSentenceTranslationActivity::preparePopupLines() {
  popupLines_.clear();
  popupScroll_ = 0;
  const AiPopupRect rect = aiPopupRect(renderer);
  const int fontId = aiPopupFont(renderer);
  const int bodyWidth = std::max(40, rect.width - 2 * AI_POPUP_PADDING - 10);
  const int bodyHeight =
      std::max(20, rect.height - AI_POPUP_TITLE_HEIGHT - AI_POPUP_FOOTER_HEIGHT - 2 * AI_POPUP_PADDING);
  popupVisibleLines_ = std::max(1, bodyHeight / std::max(1, renderer.getLineHeight(fontId) + 2));

  // Pre-wrap only once; redraw and scroll operate on the same lines without
  // repeatedly allocating strings or dropping the tail of a long sentence.
  auto append = [this, fontId, bodyWidth](const std::string& text, const bool bold) {
    size_t begin = 0;
    while (begin <= text.size() && popupLines_.size() < 240) {
      const size_t end = text.find('\n', begin);
      const std::string segment = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
      if (segment.empty()) {
        popupLines_.push_back({"", false});
      } else {
        auto wrapped = renderer.wrappedText(fontId, segment.c_str(), bodyWidth, 192,
                                            bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
        for (auto& line : wrapped) {
          if (popupLines_.size() >= 240) break;
          popupLines_.push_back({std::move(line), bold});
        }
      }
      if (end == std::string::npos) break;
      begin = end + 1;
    }
  };

  append(word_ + " — " + wordTranslation_, true);
  append("", false);
  append(tr(STR_AI_ORIGINAL), true);
  append(sentence_, false);
  append("", false);
  append(tr(STR_AI_TRANSLATION), true);
  append(sentenceTranslation_, false);
}

void AiSentenceTranslationActivity::scrollPopup(const int delta) {
  RenderLock lock;
  const int limit = std::max(0, static_cast<int>(popupLines_.size()) - popupVisibleLines_);
  const int next = std::clamp(popupScroll_ + delta, 0, limit);
  if (next != popupScroll_) {
    popupScroll_ = next;
    requestUpdate();
  }
}

void AiSentenceTranslationActivity::drawPopup() const {
  const AiPopupRect rect = aiPopupRect(renderer);
  const int fontId = aiPopupFont(renderer);
  const int lineHeight = std::max(1, renderer.getLineHeight(fontId) + 2);
  const int bodyTop = rect.y + AI_POPUP_TITLE_HEIGHT + AI_POPUP_PADDING;
  const int bodyHeight =
      std::max(20, rect.height - AI_POPUP_TITLE_HEIGHT - AI_POPUP_FOOTER_HEIGHT - 2 * AI_POPUP_PADDING);

  // Inset white panel with a black border like the dictionary dialog.
  renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, 5, Color::White);
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, 5, true);
  renderer.drawText(NOTOSANS_14_FONT_ID, rect.x + AI_POPUP_PADDING, rect.y + 11, tr(STR_AI_TRANSLATE), true,
                    EpdFontFamily::BOLD);
  renderer.drawLine(rect.x + 12, rect.y + AI_POPUP_TITLE_HEIGHT, rect.x + rect.width - 12,
                    rect.y + AI_POPUP_TITLE_HEIGHT, true);

  const auto oldClip = renderer.getClipRect();
  renderer.setClipRect(rect.x + AI_POPUP_PADDING, bodyTop, rect.width - 2 * AI_POPUP_PADDING - 6, bodyHeight);
  const int last = std::min<int>(popupLines_.size(), popupScroll_ + popupVisibleLines_);
  for (int index = popupScroll_; index < last; ++index) {
    const PopupLine& line = popupLines_[index];
    if (!line.text.empty()) {
      renderer.drawText(fontId, rect.x + AI_POPUP_PADDING, bodyTop + (index - popupScroll_) * lineHeight,
                        line.text.c_str(), true, line.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }
  }
  renderer.setClipRect(oldClip[0], oldClip[1], oldClip[2], oldClip[3]);

  // Thin scroll thumb signals that swipe / page buttons can show more text.
  const int extra = std::max(0, static_cast<int>(popupLines_.size()) - popupVisibleLines_);
  if (extra > 0) {
    const int thumb = std::max(18, bodyHeight * popupVisibleLines_ / static_cast<int>(popupLines_.size()));
    const int offset = (bodyHeight - thumb) * popupScroll_ / extra;
    renderer.fillRect(rect.x + rect.width - 9, bodyTop + offset, 3, thumb);
  }
}

void AiSentenceTranslationActivity::fail(std::string reason) {
  error_ = std::move(reason);
  state_ = State::Error;
  requestUpdate();
}

void AiSentenceTranslationActivity::onEnter() {
  Activity::onEnter();
  captureUnderlyingPage();
  if (!AI_TRANSLATION_STORE.loadFromFile() || !AI_TRANSLATION_STORE.configured()) {
    fail(tr(STR_AI_NOT_CONFIGURED));
    return;
  }
  if (sentence_.empty() || sentence_.size() > MAX_SENTENCE_BYTES || word_.empty()) {
    fail(tr(STR_AI_BAD_RESPONSE));
    return;
  }

  // No radio, TLS, or renderer work for a cached exact word+sentence.
  // A book/provider/model change has a separate on-disk namespace.
  std::string cachedWord, cachedSentence;
  if (ai_translation_cache::load(bookPath_, AI_TRANSLATION_STORE.getUrl(), AI_TRANSLATION_STORE.getModel(), word_,
                                 sentence_, cachedWord, cachedSentence)) {
    openResult(std::move(cachedWord), std::move(cachedSentence));
    return;
  }

  // Picco lookahead records include a position-specific word mapping and
  // allow an instant offline result even when the foreground exact cache misses.
#if defined(FREEINK_DEVICE_PICCO)
  if (origin_.sentenceStartOffset != UINT32_MAX && origin_.selectedWordOffset != UINT32_MAX) {
    const ai_prefetch::Scope scope{bookPath_, AI_TRANSLATION_STORE.getUrl(), AI_TRANSLATION_STORE.getModel()};
    ai_prefetch::Sentence needle;
    needle.spine = origin_.spine;
    needle.start = origin_.sentenceStartOffset;
    needle.end = UINT32_MAX;
    needle.source = sentence_;
    if (ai_prefetch::lookup(scope, needle, origin_.selectedWordOffset, word_, cachedWord, cachedSentence)) {
      openResult(std::move(cachedWord), std::move(cachedSentence));
      return;
    }
  }
#endif

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
  std::string url = ai_endpoint::migrateOpenCodeGoUrl(config.getUrl());
  if (url != config.getUrl()) {
    LOG_INF("AI", "Using current OpenCode Go API URL (legacy URL migrated in memory)");
  }
  if (!safeEndpoint(url)) {
    fail(tr(STR_AI_NOT_CONFIGURED));
    return false;
  }
  const bool gemini = url == "https://generativelanguage.googleapis.com" ||
                      url.rfind("https://generativelanguage.googleapis.com/", 0) == 0;
  if (gemini) {
    if (config.getModel().find('/') != std::string::npos || config.getModel().find(':') != std::string::npos) {
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
      "Output a single compact JSON object with exactly two string keys: "
      "word_translation and sentence_translation. No reasoning, examples, alternatives, markdown, or explanations. "
      "The sentence_translation must cover the whole source sentence.\n\n"
      "Selected word: " +
      word_ + "\nSource sentence: " + sentence_;

  JsonDocument req;
  if (gemini) {
    JsonObject item = req["contents"].to<JsonArray>().add<JsonObject>();
    item["parts"].to<JsonArray>().add<JsonObject>()["text"] = prompt;
    req["generationConfig"].to<JsonObject>()["maxOutputTokens"] = 1024;
  } else {
    req["model"] = config.getModel();
    // A bounded completion keeps thinking-model replies from growing without
    // limit; 1024 output tokens still covers a lengthy translated sentence.
    req["max_tokens"] = 1024;
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

  // Print only the connection hostname, never the URL path, API key, book
  // text, or request headers. We need the hostname to diagnose ASN_NO_SIGNER_E
  // (-188) against the actual provider's public certificate chain.
  const size_t hostStart = sizeof("https://") - 1;
  const size_t hostEnd = url.find('/', hostStart);
  const std::string host =
      url.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
  LOG_INF("AI", "TLS connecting: host=%s free=%u largest=%u ca_bytes=%u", host.c_str(),
          static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlockBytes),
          static_cast<unsigned>(sizeof(ai_translation::ROOTS) - 1));

  // Use the ESP-IDF HTTPS client (mbedTLS), not FreeInk's wolfSSL client:
  // the latter consistently fails with ASN_NO_SIGNER_E (-188) on opencode.ai
  // despite successfully loading both GTS Root R4 and its WE1 intermediate.
  // cert_pem and default hostname verification are mandatory for BYOK keys.
  AiHttpResponse response;
  if (!response.data) {
    fail(tr(STR_AI_LOW_MEMORY));
    return false;
  }
  LOG_INF("AI", "HTTP response buffer: %u bytes", static_cast<unsigned>(response.capacity));
  esp_http_client_config_t httpConfig = {};
  httpConfig.url = url.c_str();
  httpConfig.method = HTTP_METHOD_POST;
  httpConfig.timeout_ms = AI_HTTP_TIMEOUT_MS;
  httpConfig.cert_pem = ai_translation::ROOTS;
  httpConfig.skip_cert_common_name_check = false;
  httpConfig.disable_auto_redirect = true;  // Never forward API keys across redirects.
  httpConfig.event_handler = onAiHttpEvent;
  httpConfig.user_data = &response;
  httpConfig.buffer_size = 2048;
  httpConfig.buffer_size_tx = 2048;

  esp_http_client_handle_t client = esp_http_client_init(&httpConfig);
  if (!client) {
    LOG_ERR("AI", "esp_http_client_init failed for host=%s", host.c_str());
    fail(tr(STR_AI_LOW_MEMORY));
    return false;
  }

  esp_err_t setup = esp_http_client_set_header(client, "Content-Type", "application/json");
  if (setup == ESP_OK) setup = esp_http_client_set_header(client, "User-Agent", "CrossPoint-AI-Translation");
  std::string authHeader;
  if (setup == ESP_OK && gemini) {
    setup = esp_http_client_set_header(client, "x-goog-api-key", config.getKey().c_str());
  } else if (setup == ESP_OK) {
    authHeader = "Bearer " + config.getKey();
    setup = esp_http_client_set_header(client, "Authorization", authHeader.c_str());
  }
  // Go requires x-opencode-session for routing and prompt-cache affinity.
  // Every sentence translation is its own one-request conversation; generate
  // a fresh random ID per request and never reuse it across books.
  char goSession[40] = {};
  if (setup == ESP_OK && ai_endpoint::isOpenCodeGoUrl(url)) {
    snprintf(goSession, sizeof(goSession), "cp-%08x%08x%08x%08x", static_cast<unsigned>(esp_random()),
             static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()),
             static_cast<unsigned>(esp_random()));
    setup = esp_http_client_set_header(client, "x-opencode-session", goSession);
    LOG_INF("AI", "OpenCode Go request includes session routing header");
  }
  if (setup == ESP_OK) {
    setup = esp_http_client_set_post_field(client, body.data(), static_cast<int>(body.size()));
  }
  if (setup != ESP_OK) {
    LOG_ERR("AI", "ESP-IDF HTTPS setup failed: %s", esp_err_to_name(setup));
    esp_http_client_cleanup(client);
    fail(tr(STR_AI_NETWORK_FAILED));
    return false;
  }

  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  if (result != ESP_OK) {
    int tlsError = 0;
    int tlsFlags = 0;
    esp_http_client_get_and_clear_last_tls_error(client, &tlsError, &tlsFlags);
    LOG_ERR("AI", "mbedTLS HTTPS failed: host=%s result=%s tls=%d verify_flags=%d overflow=%d", host.c_str(),
            esp_err_to_name(result), tlsError, tlsFlags, response.overflow);
  }
  esp_http_client_cleanup(client);

  if (result != ESP_OK || response.overflow || status != 200) {
    JsonDocument providerError;
    const bool jsonError =
        !response.overflow && deserializeJson(providerError, response.data) == DeserializationError::Ok;
    const char* providerType = jsonError ? providerError["error"]["type"].as<const char*>() : nullptr;
    const char* providerCode = jsonError ? providerError["error"]["code"].as<const char*>() : nullptr;
    const char* providerMessage = jsonError ? providerError["error"]["message"].as<const char*>() : nullptr;
    if (!providerMessage && jsonError) providerMessage = providerError["message"].as<const char*>();
    if (!providerMessage && jsonError) providerMessage = providerError["error"].as<const char*>();

    LOG_ERR("AI", "Provider request failed: model=%s status=%d type=%s code=%s", config.getModel().c_str(), status,
            safeErrorId(providerType).c_str(), safeErrorId(providerCode).c_str());
    LOG_ERR("AI", "Translation HTTP failed: status=%d response_bytes=%u capacity=%u overflow=%d", status,
            static_cast<unsigned>(response.used), static_cast<unsigned>(response.capacity), response.overflow);
    if (response.overflow) {
      LOG_ERR("AI", "Provider body exceeded the bounded buffer; reduce model output size");
      fail("AI response too long");
    } else if (result != ESP_OK && status <= 0) {
      fail(tr(STR_AI_NETWORK_FAILED));
    } else if (status > 0 && status != 200) {
      char statusText[32];
      snprintf(statusText, sizeof(statusText), "HTTP %d", status);
      const std::string explanation = safeScreenError(providerMessage, config.getKey(), sentence_);
      fail(explanation.empty() ? std::string(statusText) : std::string(statusText) + ": " + explanation);
    } else {
      fail(tr(STR_AI_NETWORK_FAILED));
    }
    return false;
  }

  JsonDocument answer;
  // Large reasoning/meta fields are irrelevant to a translation. ArduinoJson
  // skips them during parsing, keeping the JSON document bounded.
  JsonDocument filter;
  filter["choices"][0]["message"]["content"] = true;
  filter["choices"][0]["text"] = true;
  filter["choices"][0]["finish_reason"] = true;
  filter["output_text"] = true;
  filter["candidates"][0]["content"]["parts"] = true;
  if (deserializeJson(answer, response.data, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    fail(tr(STR_AI_BAD_RESPONSE));
    return false;
  }
  std::string output;
  if (gemini) {
    JsonArrayConst parts = answer["candidates"][0]["content"]["parts"].as<JsonArrayConst>();
    for (JsonVariantConst part : parts) {
      if (const char* text = part["text"].as<const char*>()) output += text;
    }
  } else {
    JsonVariantConst content = answer["choices"][0]["message"]["content"];
    if (const char* text = content.as<const char*>()) {
      output = text;
    } else if (JsonArrayConst parts = content.as<JsonArrayConst>(); !parts.isNull()) {
      for (JsonVariantConst part : parts) {
        if (const char* text = part["text"].as<const char*>()) output += text;
      }
    }
    if (output.empty()) {
      if (const char* text = answer["choices"][0]["text"].as<const char*>()) output = text;
    }
    if (output.empty()) {
      if (const char* text = answer["output_text"].as<const char*>()) output = text;
    }
  }
  if (!extractTranslationText(output, wordTranslation, sentenceTranslation)) {
    const char* finish = answer["choices"][0]["finish_reason"].as<const char*>();
    LOG_ERR("AI", "HTTP 200 with unrecognized translation payload: bytes=%u text_bytes=%u finish=%s",
            static_cast<unsigned>(response.used), static_cast<unsigned>(output.size()), safeErrorId(finish).c_str());
    fail(tr(STR_AI_BAD_RESPONSE));
    return false;
  }
  return true;
}

void AiSentenceTranslationActivity::openResult(std::string wordTranslation, std::string sentenceTranslation) {
  {
    RenderLock lock;
    wordTranslation_ = std::move(wordTranslation);
    sentenceTranslation_ = std::move(sentenceTranslation);
    preparePopupLines();
    state_ = State::Showing;
  }
  requestUpdate();
#if defined(FREEINK_DEVICE_PICCO)
  // Do not scan or do TLS here: one small page/word scan is scheduled per
  // later UI loop tick. The session exists ONLY while this modal is active.
  if (!prefetch_ && origin_.section && origin_.selectedWordOffset != UINT32_MAX) {
    ai_prefetch::Scope scope{bookPath_, AI_TRANSLATION_STORE.getUrl(), AI_TRANSLATION_STORE.getModel()};
    prefetch_ = makeUniqueNoThrow<AiSentencePrefetchSession>(std::move(scope), origin_.section, origin_.spine,
                                                             origin_.firstPage, origin_.selectedWordOffset);
  }
#endif
}

void AiSentenceTranslationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (state_ == State::Showing) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finish();
      return;
    }
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Left) {
      scrollPopup(std::max(1, popupVisibleLines_ - 3));
    } else if (swipe == MappedInputManager::SwipeDir::Down || swipe == MappedInputManager::SwipeDir::Right) {
      scrollPopup(-std::max(1, popupVisibleLines_ - 3));
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
        mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
        mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      scrollPopup(std::max(1, popupVisibleLines_ - 3));
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
        mappedInput.wasPressed(MappedInputManager::Button::NavPrevious) ||
        mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      scrollPopup(-std::max(1, popupVisibleLines_ - 3));
    }
    int x = -1;
    int y = -1;
    if (mappedInput.wasScreenTapped(x, y)) {
      const AiPopupRect rect = aiPopupRect(renderer);
      if (x < rect.x || x >= rect.x + rect.width || y < rect.y || y >= rect.y + rect.height) {
        finish();
        return;  // No new work after requesting dismissal.
      } else if (y >= rect.y + AI_POPUP_TITLE_HEIGHT && y < rect.y + rect.height - AI_POPUP_FOOTER_HEIGHT) {
        // Taps on the upper/lower half scroll a few lines; swipes scroll a page.
        const int middle = rect.y + rect.height / 2;
        scrollPopup(y < middle ? -4 : 4);
      }
    }
#if defined(FREEINK_DEVICE_PICCO)
    if (prefetch_) prefetch_->tick();
#endif
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
    // Save only validated provider output; failed/partial results never enter the cache.
    // This happens after queuing the popup render, so the first result is not
    // delayed by filesystem housekeeping.
    ai_translation_cache::save(bookPath_, AI_TRANSLATION_STORE.getUrl(), AI_TRANSLATION_STORE.getModel(), word_,
                               sentence_, wordTranslation_, sentenceTranslation_);
  }
}

void AiSentenceTranslationActivity::onExit() {
  Activity::onExit();
  if (prefetch_) {
    prefetch_->cancel();
    prefetch_.reset();
  }
  releaseBackground();
  if (wifiOwned_) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
}

void AiSentenceTranslationActivity::render(RenderLock&&) {
  // Repaint the captured EPUB/selection page rather than replacing it with
  // an opaque full-screen translation activity. The dialog itself is opaque,
  // so the page under its bounds never bleeds through.
  if (background_ && renderer.getWriteTarget()) {
    memcpy(renderer.getWriteTarget(), background_, HalDisplay::BUFFER_SIZE);
  } else {
    renderer.clearScreen();
  }

  if (state_ == State::Showing) {
    drawPopup();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
  if (state_ == State::Translating) {
    GUI.drawPopup(renderer, tr(STR_AI_TRANSLATING));
    return;
  }
  // Connection errors remain readable without displacing the EPUB page.
  const AiPopupRect rect = aiPopupRect(renderer);
  renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, 5, Color::White);
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, 5, true);
  UITheme::drawCenteredWrappedText(renderer, Rect{rect.x + 16, rect.y + 24, rect.width - 32, rect.height - 48},
                                   NOTOSANS_14_FONT_ID, error_.c_str(), 9);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
