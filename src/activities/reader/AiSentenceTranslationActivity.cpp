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
#include <esp_http_client.h>

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
#include "util/AiApiEndpoint.h"
#include "util/AiTranslationRoots.h"

namespace {

constexpr size_t MAX_RESPONSE_BYTES = 8 * 1024;
constexpr size_t MAX_SENTENCE_BYTES = 2048;
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

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
  std::string data;
  bool overflow = false;
};

esp_err_t onAiHttpEvent(esp_http_client_event_t* event) {
  if (!event || event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* response = static_cast<AiHttpResponse*>(event->user_data);
  if (!response || event->data_len < 0) return ESP_FAIL;
  const size_t bytes = static_cast<size_t>(event->data_len);
  if (bytes > MAX_RESPONSE_BYTES - response->data.size()) {
    response->overflow = true;
    return ESP_FAIL;
  }
  response->data.append(static_cast<const char*>(event->data), bytes);
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

}  // namespace

AiSentenceTranslationActivity::AiSentenceTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
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
      "Reply with exactly two lines, without markdown or commentary:\n"
      "WORD: <Russian translation of the word>\n"
      "SENTENCE: <Fluent Russian translation of the entire sentence>\n\n"
      "Selected word: " +
      word_ + "\nSource sentence: " + sentence_;

  JsonDocument req;
  if (gemini) {
    JsonObject item = req["contents"].to<JsonArray>().add<JsonObject>();
    item["parts"].to<JsonArray>().add<JsonObject>()["text"] = prompt;
    req["generationConfig"].to<JsonObject>()["maxOutputTokens"] = 384;
  } else {
    req["model"] = config.getModel();
    // Go's published Chat Completions example only requires model/messages;
    // do not send optional token fields to legacy API adapters.
    if (!ai_endpoint::isOpenCodeGoUrl(url)) req["max_tokens"] = 384;
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
  response.data.reserve(2048);
  esp_http_client_config_t httpConfig = {};
  httpConfig.url = url.c_str();
  httpConfig.method = HTTP_METHOD_POST;
  httpConfig.timeout_ms = 30000;
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
    LOG_ERR("AI", "Translation HTTP failed: status=%d response_bytes=%u", status,
            static_cast<unsigned>(response.data.size()));
    if (status > 0) {
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
  if (deserializeJson(answer, response.data) != DeserializationError::Ok) {
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
  definition.reserve(word_.size() + wordTranslation.size() + sentence_.size() + sentenceTranslation.size() + 64);
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
  const char* status = state_ == State::Connecting    ? tr(STR_CONNECTING)
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
