#include "AiSentencePrefetchSession.h"

#include <ArduinoJson.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/blocks/TextBlock.h>
#include <HalMemory.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "AITranslationStore.h"
#include "RenderLock.h"
#include "clippings/ClippingText.h"
#include "util/AiApiEndpoint.h"
#include "util/AiSentenceUtils.h"
#include "util/AiTranslationRoots.h"

namespace {

constexpr size_t MAX_BATCH_BODY = 24 * 1024;
constexpr size_t MAX_RESPONSE = 48 * 1024;
constexpr int HTTP_TIMEOUT_MS = 30000;
constexpr uint32_t MIN_INTERNAL_FREE = 120 * 1024;
constexpr uint32_t MIN_INTERNAL_BLOCK = 44 * 1024;
constexpr uint32_t MIN_PSRAM_FREE = 144 * 1024;
constexpr uint32_t MIN_PSRAM_BLOCK = 64 * 1024;
constexpr uint32_t PREFETCH_STACK = 12288;
constexpr size_t TOKENS_PER_TICK = 48;
constexpr size_t MAX_PAGE_TOKENS = 320;

// A prior popup may close while its HTTPS task is finishing. Never allow a
// second popup to start another request until that worker releases TLS.
std::atomic<bool> gPrefetchHttpBusy{false};

// Never compete with the reader for its last heap blocks. TLS needs internal
// buffers even when JSON and the HTTP body are backed by Picco PSRAM.
bool memoryReady() {
#if !defined(FREEINK_DEVICE_PICCO)
  return false;  // This feature is deliberately disabled for all other boards.
#else
  const auto internal = HalMemory::getInternalHeap();
  const auto psram = HalMemory::getPsramHeap();
  return internal.freeBytes >= MIN_INTERNAL_FREE && internal.largestBlockBytes >= MIN_INTERNAL_BLOCK &&
         psram.freeBytes >= MIN_PSRAM_FREE && psram.largestBlockBytes >= MIN_PSRAM_BLOCK;
#endif
}

bool safeUrl(const std::string& url) {
  if (url.size() < 12 || url.size() > 255 || url.compare(0, 8, "https://") != 0 ||
      url.find_first_of("?#@\r\n\t ") != std::string::npos)
    return false;
  return url[8] != '/' && url[8] != ':';
}

bool buildUrl(std::string& url, const std::string& model, bool& gemini) {
  url = ai_endpoint::migrateOpenCodeGoUrl(url);
  if (!safeUrl(url)) return false;
  gemini = url == "https://generativelanguage.googleapis.com" ||
           url.rfind("https://generativelanguage.googleapis.com/", 0) == 0;
  if (gemini) {
    if (model.find('/') != std::string::npos || model.find(':') != std::string::npos) return false;
    const auto pos = url.find("/models/");
    if (pos != std::string::npos) url.resize(pos);
    if (url.back() == '/') url.pop_back();
    url += "/models/" + model + ":generateContent?fields=candidates.content.parts.text";
  } else {
    if (url.back() == '/') url.pop_back();
    if (!url.ends_with("/chat/completions")) url += "/chat/completions";
  }
  return true;
}

struct Response {
  char* data = nullptr;
  size_t size = 0;
  bool overflow = false;
  std::atomic<bool>* cancel = nullptr;

  explicit Response(std::atomic<bool>* cancelToken) : cancel(cancelToken) {
    data = static_cast<char*>(heap_caps_malloc(MAX_RESPONSE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data) data[0] = '\0';
  }
  ~Response() { heap_caps_free(data); }
  Response(const Response&) = delete;
  Response& operator=(const Response&) = delete;
};

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
  if (!event) return ESP_FAIL;
  auto* result = static_cast<Response*>(event->user_data);
  if (!result || result->cancel->load(std::memory_order_relaxed)) return ESP_FAIL;
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  if (!result->data || event->data_len < 0 || static_cast<size_t>(event->data_len) > MAX_RESPONSE - result->size) {
    result->overflow = true;
    return ESP_FAIL;
  }
  const size_t bytes = static_cast<size_t>(event->data_len);
  memcpy(result->data + result->size, event->data, bytes);
  result->size += bytes;
  result->data[result->size] = '\0';
  return ESP_OK;
}

// Keep untrusted model output bounded, and never log book text, model output,
// credentials, or provider error payloads.
std::string getOutput(const Response& response, bool gemini) {
  JsonDocument answer;
  JsonDocument filter;
  filter["choices"][0]["message"]["content"] = true;
  filter["choices"][0]["text"] = true;
  filter["output_text"] = true;
  filter["candidates"][0]["content"]["parts"] = true;
  if (deserializeJson(answer, response.data, DeserializationOption::Filter(filter)) != DeserializationError::Ok)
    return {};

  std::string output;
  if (gemini) {
    for (JsonVariantConst part : answer["candidates"][0]["content"]["parts"].as<JsonArrayConst>()) {
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
  return output.size() <= 24 * 1024 ? output : std::string{};
}

bool buildRequest(const std::vector<ai_prefetch::Sentence>& batch, bool gemini, const std::string& model,
                  std::string& body) {
  JsonDocument payload;
  JsonArray sentences = payload["sentences"].to<JsonArray>();
  for (size_t i = 0; i < batch.size(); ++i) {
    JsonObject sentence = sentences.add<JsonObject>();
    sentence["id"] = static_cast<unsigned>(i);
    sentence["text"] = batch[i].source;
    JsonArray words = sentence["words"].to<JsonArray>();
    for (size_t w = 0; w < batch[i].words.size(); ++w) {
      JsonObject word = words.add<JsonObject>();
      word["i"] = static_cast<unsigned>(w);
      word["text"] = batch[i].words[w].source;
    }
  }

  std::string input;
  serializeJson(payload, input);
  if (payload.overflowed() || input.size() > 17000) return false;
  const std::string prompt =
      "Translate all numbered source sentences and EACH numbered source word into Russian in its own "
      "sentence context. Do not omit or reorder word ids. Each word translation should be brief "
      "(not an explanation). Return ONLY compact JSON with schema "
      "{\"items\":[{\"id\":0,\"translation\":\"...\","
      "\"words\":[{\"i\":0,\"translation\":\"...\"}]}]}. "
      "All input sentence IDs and word IDs must occur exactly once. "
      "No commentary, examples, markdown, or reasoning.\n" +
      input;
  JsonDocument request;
  if (gemini) {
    JsonObject item = request["contents"].to<JsonArray>().add<JsonObject>();
    item["parts"].to<JsonArray>().add<JsonObject>()["text"] = prompt;
    request["generationConfig"].to<JsonObject>()["maxOutputTokens"] = 6144;
  } else {
    request["model"] = model;
    request["max_tokens"] = 6144;
    JsonObject message = request["messages"].to<JsonArray>().add<JsonObject>();
    message["role"] = "user";
    message["content"] = prompt;
  }
  serializeJson(request, body);
  return !request.overflowed() && body.size() <= MAX_BATCH_BODY;
}

size_t processResponse(const std::string& raw, const ai_prefetch::Scope& scope,
                       const std::vector<ai_prefetch::Sentence>& input, std::atomic<bool>& cancel) {
  size_t start = raw.find('{');
  size_t end = raw.rfind('}');
  if (start == std::string::npos || end == std::string::npos || end < start || end - start > 24 * 1024) return 0;
  JsonDocument result;
  if (deserializeJson(result, raw.substr(start, end - start + 1)) != DeserializationError::Ok) return 0;
  JsonArrayConst items = result["items"].as<JsonArrayConst>();
  if (items.isNull()) return 0;
  bool ids[ai_prefetch::BATCH_SENTENCES] = {};
  size_t saved = 0;
  for (JsonVariantConst item : items) {
    if (cancel.load(std::memory_order_relaxed)) return saved;
    if (!item["id"].is<int>()) continue;
    const int id = item["id"].as<int>();
    if (id < 0 || id >= static_cast<int>(input.size()) || ids[id]) continue;
    ids[id] = true;
    const char* text = item["translation"].as<const char*>();
    if (!text || !*text || strlen(text) > 4096) continue;
    ai_prefetch::Sentence record = input[id];
    record.translation = text;
    JsonArrayConst words = item["words"].as<JsonArrayConst>();
    if (words.isNull()) continue;
    bool mapped[ai_prefetch::MAX_WORDS] = {};
    size_t count = 0;
    for (JsonVariantConst word : words) {
      // Numeric type guards: absent "i" is NOT interpreted as zero.
      if (!word["i"].is<int>()) continue;
      const int i = word["i"].as<int>();
      const char* value = word["translation"].as<const char*>();
      if (i < 0 || i >= static_cast<int>(record.words.size()) || mapped[i] || !value || !*value || strlen(value) > 256)
        continue;
      record.words[i].translation = value;
      mapped[i] = true;
      ++count;
    }
    // A partially translated batch cannot claim to be a complete word cache.
    // Missing mappings will be resolved by normal foreground Translate later.
    if (count != record.words.size() || !memoryReady() || cancel.load(std::memory_order_relaxed)) continue;
    if (ai_prefetch::save(scope, record)) ++saved;
  }
  return saved;
}

}  // namespace

struct AiSentencePrefetchSession::Job {
  ai_prefetch::Scope scope;
  ai_prefetch::Checkpoint checkpoint;
  std::vector<ai_prefetch::Sentence> batch;
  std::string apiKey;
  std::atomic<bool> cancel{false};
  std::atomic<bool> done{false};
  bool success = false;
};

namespace {
void batchTask(void* argument) {
  std::unique_ptr<std::shared_ptr<AiSentencePrefetchSession::Job>> owner(
      static_cast<std::shared_ptr<AiSentencePrefetchSession::Job>*>(argument));
  auto job = std::move(*owner);
  owner.reset();
  auto perform = [&]() -> bool {
    if (job->cancel.load() || !memoryReady() || !trustedtime::trustedNow()) return false;
    bool gemini = false;
    std::string url = job->scope.url;
    if (!buildUrl(url, job->scope.model, gemini)) return false;
    std::string body;
    if (!buildRequest(job->batch, gemini, job->scope.model, body)) return false;
    Response response(&job->cancel);
    if (!response.data) return false;

    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    // Non-blocking HTTPS allows the modal to cancel without another task
    // closing a live client handle (which is not thread-safe).
    cfg.timeout_ms = 5000;
    cfg.is_async = true;
    cfg.cert_pem = ai_translation::ROOTS;
    cfg.skip_cert_common_name_check = false;
    cfg.disable_auto_redirect = true;
    cfg.event_handler = onHttpEvent;
    cfg.user_data = &response;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t setup = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (setup == ESP_OK) setup = esp_http_client_set_header(client, "User-Agent", "CrossPoint-AI-Prefetch");
    std::string auth;
    if (setup == ESP_OK && gemini) {
      setup = esp_http_client_set_header(client, "x-goog-api-key", job->apiKey.c_str());
    } else if (setup == ESP_OK) {
      auth = "Bearer " + job->apiKey;
      setup = esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    char session[40] = {};
    if (setup == ESP_OK && ai_endpoint::isOpenCodeGoUrl(url)) {
      snprintf(session, sizeof(session), "cp-%08x%08x%08x%08x", static_cast<unsigned>(esp_random()),
               static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()),
               static_cast<unsigned>(esp_random()));
      setup = esp_http_client_set_header(client, "x-opencode-session", session);
    }
    if (setup == ESP_OK) setup = esp_http_client_set_post_field(client, body.data(), static_cast<int>(body.size()));
    if (job->cancel.load()) setup = ESP_FAIL;
    esp_err_t result = setup;
    if (setup == ESP_OK) {
      const uint32_t started = millis();
      do {
        if (job->cancel.load(std::memory_order_relaxed) ||
            static_cast<uint32_t>(millis() - started) >= HTTP_TIMEOUT_MS) {
          result = ESP_FAIL;
          break;
        }
        result = esp_http_client_perform(client);
        if (result == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(25));
      } while (result == ESP_ERR_HTTP_EAGAIN);
    }
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK || response.overflow || status != 200 || job->cancel.load()) {
      LOG_INF("AI_PRE", "Batch skipped: HTTP status=%d transport=%d", status, static_cast<int>(result));
      return false;
    }
    const std::string output = getOutput(response, gemini);
    if (output.empty() || job->cancel.load() || !memoryReady()) return false;
    const size_t saved = processResponse(output, job->scope, job->batch, job->cancel);
    if (job->cancel.load()) return false;
    // Hints are advisory; the scanner still validates EVERY record on resume.
    if (saved != 0) ai_prefetch::saveCheckpoint(job->scope, job->checkpoint);
    LOG_INF("AI_PRE", "Batch stored %u/%u sentences", static_cast<unsigned>(saved),
            static_cast<unsigned>(job->batch.size()));
    return saved == job->batch.size();
  };
  job->success = perform();
  job->done.store(true, std::memory_order_release);
  job.reset();
  gPrefetchHttpBusy.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}
}  // namespace

AiSentencePrefetchSession::AiSentencePrefetchSession(ai_prefetch::Scope scope, Section* section, uint32_t spine,
                                                     int firstPage, uint32_t selectedWordOffset)
    : scope_(std::move(scope)), section_(section), spine_(spine), anchor_(selectedWordOffset), nextPage_(firstPage) {
  batch_.reserve(ai_prefetch::BATCH_SENTENCES);
  current_.spine = spine_;
  ai_prefetch::Checkpoint saved;
  if (ai_prefetch::loadCheckpoint(scope_, saved) && saved.spine == spine_ && saved.anchor == anchor_) {
    LOG_INF("AI_PRE", "Resume checkpoint: scanned=%u", static_cast<unsigned>(saved.scanned));
  }
}

AiSentencePrefetchSession::~AiSentencePrefetchSession() { cancel(); }

void AiSentencePrefetchSession::cancel() {
  stopped_ = true;
  if (worker_) worker_->cancel.store(true, std::memory_order_release);
  worker_.reset();  // Worker owns a separate shared copy; never dereferences this activity.
  pageTokens_.clear();
  page_.reset();
  batch_.clear();
}

bool AiSentencePrefetchSession::openPage() {
  if (!section_ || nextPage_ >= static_cast<int>(section_->pageCount)) {
    // Reuse pages already laid out by the reader (including a partial build).
    // Never call buildSomeMore() or inflate another chapter just for AI.
    scanEnded_ = true;
    return false;
  }
  const auto mem = HalMemory::getInternalHeap();
  if (mem.freeBytes < MIN_INTERNAL_FREE || mem.largestBlockBytes < MIN_INTERNAL_BLOCK) return false;
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return false;
  page_ = section_->loadPage(nextPage_);
  ++nextPage_;
  if (!page_) {
    scanEnded_ = true;
    return false;
  }
  pageTokens_.clear();
  for (const auto& element : page_->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine&>(*element).getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const auto r = block->wordSourceRange(i);
      if (r.start == UINT32_MAX || r.end == UINT32_MAX || r.end <= r.start ||
          !clippingText::hasVisibleText(block->wordText(i)))
        continue;
      if (pageTokens_.size() >= MAX_PAGE_TOKENS) {
        disabled_ = true;
        page_.reset();
        pageTokens_.clear();
        return false;
      }
      pageTokens_.push_back({block, i, r.start, r.end, block->wordStartsParagraph(i)});
    }
  }
  std::stable_sort(pageTokens_.begin(), pageTokens_.end(),
                   [](const Token& a, const Token& b) { return a.start < b.start; });
  tokenIndex_ = 0;
  return true;
}

void AiSentencePrefetchSession::completeSentence() {
  if (current_.source.empty()) {
    current_ = {};
    current_.spine = spine_;
    droppedSentence_ = false;
    return;
  }
  if (current_.start > anchor_ && considered_ < ai_prefetch::WINDOW_SENTENCES) {
    ++considered_;
    if (!droppedSentence_ && !ai_prefetch::has(scope_, current_)) {
      batch_.push_back(std::move(current_));
    }
  }
  current_ = {};
  current_.spine = spine_;
  droppedSentence_ = false;
}

void AiSentencePrefetchSession::acceptToken(const Token& token) {
  const char* raw = token.block->wordText(token.index);
  if (!raw) return;
  const char* clean = raw;
  if (clippingText::hasEmSpacePrefix(clean)) clean += 3;
  while (*clean == ' ' || *clean == '\t' || *clean == '\n' || *clean == '\r' ||
         (static_cast<uint8_t>(clean[0]) == 0xC2 && static_cast<uint8_t>(clean[1]) == 0xA0)) {
    clean += static_cast<uint8_t>(clean[0]) == 0xC2 ? 2 : 1;
  }
  if (!*clean) return;
  if (token.paragraph && !current_.source.empty()) completeSentence();
  if (current_.source.empty()) {
    current_.spine = spine_;
    current_.start = token.start;
  }
  current_.end = token.end;
  const char separator = !current_.source.empty() && token.start > lastWordEnd_ ? ' ' : '\0';
  if (!clippingText::append(current_.source, clean, separator, ai_prefetch::MAX_SENTENCE_BYTES,
                            token.block->wordHasDiscretionaryHyphen(token.index))) {
    droppedSentence_ = true;
  }
  if (current_.words.size() >= ai_prefetch::MAX_WORDS || strlen(clean) > 160) {
    droppedSentence_ = true;
  } else if (!droppedSentence_) {
    ai_prefetch::Word word;
    word.offset = token.start;
    word.source = clean;
    current_.words.push_back(std::move(word));
  }
  lastWordEnd_ = token.end;
  if (ai_sentence::isEnd(raw)) completeSentence();
}

void AiSentencePrefetchSession::finishScan() {
  if (!scanEnded_) scanEnded_ = true;
  completeSentence();
}

bool AiSentencePrefetchSession::launchBatch() {
  if (batch_.empty() || !memoryReady()) return false;
  bool idle = false;
  if (!gPrefetchHttpBusy.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) return false;
  auto job = std::make_shared<Job>();
  job->scope = scope_;
  job->batch = std::move(batch_);
  job->apiKey = AI_TRANSLATION_STORE.getKey();
  job->checkpoint = {spine_, anchor_, job->batch.back().end, considered_};
  batch_.clear();
  batch_.reserve(ai_prefetch::BATCH_SENTENCES);
  auto* holder = new (std::nothrow) std::shared_ptr<Job>(job);
  if (!holder) {
    gPrefetchHttpBusy.store(false, std::memory_order_release);
    return false;
  }
  TaskHandle_t task = nullptr;
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t core = 0;
#else
  constexpr BaseType_t core = 0;
#endif
  const BaseType_t ok =
      xTaskCreatePinnedToCore(batchTask, "AiPrefetch", PREFETCH_STACK, holder, tskIDLE_PRIORITY + 1, &task, core);
  if (ok != pdPASS) {
    delete holder;
    LOG_INF("AI_PRE", "Skipping batch: low memory for task stack");
    disabled_ = true;
    gPrefetchHttpBusy.store(false, std::memory_order_release);
    return false;
  }
  worker_ = std::move(job);
  return true;
}

void AiSentencePrefetchSession::tick() {
  if (stopped_ || disabled_ || !section_ || WiFi.status() != WL_CONNECTED) return;
  if (worker_) {
    if (!worker_->done.load(std::memory_order_acquire)) return;
    const bool success = worker_->success;
    worker_.reset();
    if (!success) {
      disabled_ = true;  // No retry storm; resume on a later Translate popup.
      return;
    }
  }
  if (!memoryReady()) return;
  if (scanEnded_ || considered_ >= ai_prefetch::WINDOW_SENTENCES) {
    finishScan();
    if (!batch_.empty()) launchBatch();
    return;
  }
  // Do at most one page read or 48 token visits per loop tick.
  if (!page_ && !openPage()) {
    if (scanEnded_) finishScan();
    if (scanEnded_ && !batch_.empty()) launchBatch();
    return;
  }
  size_t stepped = 0;
  while (page_ && tokenIndex_ < pageTokens_.size() && stepped++ < TOKENS_PER_TICK &&
         batch_.size() < ai_prefetch::BATCH_SENTENCES && considered_ < ai_prefetch::WINDOW_SENTENCES) {
    const Token token = pageTokens_[tokenIndex_++];
    acceptToken(token);
  }
  if (tokenIndex_ >= pageTokens_.size()) {
    pageTokens_.clear();
    page_.reset();
  }
  if (batch_.size() == ai_prefetch::BATCH_SENTENCES) launchBatch();
}
