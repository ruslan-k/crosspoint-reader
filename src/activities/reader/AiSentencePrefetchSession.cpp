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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AITranslationStore.h"
#include "activities/RenderLock.h"
#include "clippings/ClippingText.h"
#include "util/AiApiEndpoint.h"
#include "util/AiSentenceUtils.h"
#include "util/AiPrefetchStream.h"
#include "util/AiTranslationRoots.h"

namespace {

constexpr size_t MAX_BATCH_BODY = 24 * 1024;
constexpr size_t MAX_RESPONSE = 48 * 1024;
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr int HTTP_STEP_TIMEOUT_MS = 100;
constexpr uint32_t MIN_INTERNAL_IN_FLIGHT = 15 * 1024;
// Cooperative client uses the existing Translate task stack, unlike the old worker.
constexpr uint32_t MIN_INTERNAL_FREE = 94 * 1024;
constexpr uint32_t MIN_INTERNAL_BLOCK = 36 * 1024;
constexpr uint32_t MIN_PSRAM_FREE = 144 * 1024;
constexpr uint32_t MIN_PSRAM_BLOCK = 64 * 1024;
constexpr size_t TOKENS_PER_TICK = 48;
constexpr size_t MAX_PAGE_TOKENS = 320;
constexpr size_t SSE_LINE_CAP = 8 * 1024;
constexpr size_t JSON_OBJECT_CAP = 24 * 1024;

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
    url += "/models/" + model + ":streamGenerateContent?alt=sse";
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
  // Send only missing word indices (the durable cache supplies the rest).
  JsonDocument payload;
  JsonArray sentences = payload["sentences"].to<JsonArray>();
  for (size_t i = 0; i < batch.size(); ++i) {
    JsonObject sentence = sentences.add<JsonObject>();
    sentence["id"] = static_cast<unsigned>(i);
    sentence["text"] = batch[i].source;
    sentence["need_sentence"] = batch[i].translation.empty();
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
      "Translate the numbered text into Russian. Respond with exactly one standalone, compact JSON object "
      "per sentence, each on its own line (NDJSON). Emit id=0 first, then in increasing id order. "
      "For each id include {\"id\":0,\"translation\":\"...\",\"words\":[{\"i\":0,\"translation\":\"...\"}]}. "
      "If need_sentence=false omit translation (already cached). The numbered words list contains "
      "ONLY missing contextual word translations; return as many valid word entries as possible, "
      "never invent indices or fill missing words with placeholders. Each word translation is brief. "
      "Do not wait for all sentences to write the first object. No markdown fences, explanations, "
      "or a surrounding items array. Source:\n" +
      input;
  JsonDocument request;
  if (gemini) {
    JsonObject item = request["contents"].to<JsonArray>().add<JsonObject>();
    item["parts"].to<JsonArray>().add<JsonObject>()["text"] = prompt;
    request["generationConfig"].to<JsonObject>()["maxOutputTokens"] = 6144;
  } else {
    request["model"] = model;
    request["max_tokens"] = 6144;
    request["stream"] = true;
    JsonObject message = request["messages"].to<JsonArray>().add<JsonObject>();
    message["role"] = "user";
    message["content"] = prompt;
  }
  serializeJson(request, body);
  return !request.overflowed() && body.size() <= MAX_BATCH_BODY;
}

// A model can return only a sentence, only a subset of requested words, or
// both. Persist every validated fragment; the cache merges it with old data.
bool saveItem(JsonVariantConst item, const ai_prefetch::Scope& scope,
              const std::vector<ai_prefetch::Sentence>& batch, bool* stored) {
  if (!item["id"].is<int>()) return false;
  const int id = item["id"].as<int>();
  if (id < 0 || id >= static_cast<int>(batch.size())) return false;
  ai_prefetch::Sentence record = batch[id];
  record.translation.clear();
  record.words.clear();
  const char* translated = item["translation"].as<const char*>();
  if (translated && *translated && strlen(translated) <= 4096) record.translation = translated;
  JsonArrayConst words = item["words"].as<JsonArrayConst>();
  bool used[ai_prefetch::MAX_WORDS] = {};
  if (!words.isNull()) {
    for (JsonVariantConst word : words) {
      if (!word["i"].is<int>()) continue;
      const int i = word["i"].as<int>();
      const char* value = word["translation"].as<const char*>();
      if (i < 0 || i >= static_cast<int>(batch[id].words.size()) || used[i] || !value || !*value ||
          strlen(value) > 256)
        continue;
      ai_prefetch::Word translatedWord = batch[id].words[i];
      translatedWord.translation = value;
      record.words.push_back(std::move(translatedWord));
      used[i] = true;
    }
  }
  if (record.translation.empty() && record.words.empty()) return false;
  std::sort(record.words.begin(), record.words.end(),
            [](const ai_prefetch::Word& lhs, const ai_prefetch::Word& rhs) { return lhs.offset < rhs.offset; });
  if (!ai_prefetch::save(scope, record)) return false;
  stored[id] = true;
  return true;
}

}  // namespace

// One nonblocking HTTPS step per Translate UI loop. No extra FreeRTOS stack.
struct AiSentencePrefetchSession::Job {
  ai_prefetch::Scope scope;
  ai_prefetch::Checkpoint checkpoint;
  std::vector<ai_prefetch::Sentence> batch;
  std::string url;
  std::string body;
  std::string auth;
  bool gemini = false;
  std::atomic<bool> cancel{false};
  Response response{&cancel};
  esp_http_client_handle_t client = nullptr;
  uint32_t startedAt = 0;
  bool classified = false;
  bool rawNonStreaming = false;
  bool parseError = false;
  bool stored[ai_prefetch::BATCH_SENTENCES] = {};
  uint8_t accepted[ai_prefetch::BATCH_SENTENCES] = {};
  char* sseLine = static_cast<char*>(heap_caps_malloc(SSE_LINE_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  char* jsonObject = static_cast<char*>(heap_caps_malloc(JSON_OBJECT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  std::unique_ptr<ai_prefetch::StreamFramer> framer;
  std::array<char*, ai_prefetch::BATCH_SENTENCES> deferred{};
  std::array<size_t, ai_prefetch::BATCH_SENTENCES> deferredBytes{};

  Job() {
    if (sseLine && jsonObject) {
      framer = std::make_unique<ai_prefetch::StreamFramer>(sseLine, SSE_LINE_CAP, jsonObject, JSON_OBJECT_CAP);
    }
  }
  ~Job() {
    if (client) esp_http_client_cleanup(client);
    for (char* ptr : deferred) heap_caps_free(ptr);
    heap_caps_free(sseLine);
    heap_caps_free(jsonObject);
  }
};

void AiSentencePrefetchSession::acceptStreamObject(std::string_view json) {
  if (!worker_) return;
  Job& job = *worker_;
  JsonDocument parsed;
  if (deserializeJson(parsed, json.data(), json.size()) != DeserializationError::Ok) return;
  const auto accept = [&](JsonVariantConst item) {
    if (!item["id"].is<int>()) return;
    const int id = item["id"].as<int>();
    if (id < 0 || id >= static_cast<int>(job.batch.size()) || job.accepted[id] >= 4) return;
    if (saveItem(item, job.scope, job.batch, job.stored)) {
      ++job.accepted[id];
      // The checkpoint is advisory only. Never skip a sentence merely
      // because this counter claims that a later offset was visited.
      if (ai_prefetch::saveCheckpoint(job.scope, job.checkpoint)) {
        LOG_INF("AI_PRE", "Stream item saved: id=%d", id);
      }
    } else if (json.size() <= JSON_OBJECT_CAP && !job.deferred[id]) {
      // TLS may leave too little internal RAM for the atomic file write.
      // Copy the complete object to PSRAM and retry AFTER disconnecting TLS.
      char* pending = static_cast<char*>(heap_caps_malloc(json.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (pending) {
        memcpy(pending, json.data(), json.size());
        job.deferred[id] = pending;
        job.deferredBytes[id] = json.size();
      }
    }
  };
  JsonArrayConst items = parsed["items"].as<JsonArrayConst>();
  if (!items.isNull()) {
    for (JsonVariantConst item : items) accept(item);
  } else {
    accept(parsed.as<JsonVariantConst>());
  }
}

void AiSentencePrefetchSession::consumeStream() {
  if (!worker_ || !worker_->response.size || !worker_->framer) return;
  Job& job = *worker_;
  Response& buffer = job.response;
  if (!job.classified) {
    size_t i = 0;
    while (i < buffer.size && (buffer.data[i] == ' ' || buffer.data[i] == '\r' ||
                               buffer.data[i] == '\n' || buffer.data[i] == '\t'))
      ++i;
    if (i == buffer.size) return;
    job.rawNonStreaming = buffer.data[i] == '{' || buffer.data[i] == '[';
    job.classified = true;
  }
  if (job.rawNonStreaming) return;  // Buffer full ordinary JSON until HTTP completes.
  const bool ok = job.framer->feedSse(buffer.data, buffer.size, [&](std::string_view frame) {
    if (job.parseError) return;
    JsonDocument chunk;
    if (deserializeJson(chunk, frame.data(), frame.size()) != DeserializationError::Ok) return;
    const auto feedText = [&](const char* text) {
      if (!text || job.parseError) return;
      if (!job.framer->feedModel(text, strlen(text),
                                 [&](std::string_view record) { acceptStreamObject(record); })) {
        job.parseError = true;
      }
    };
    if (job.gemini) {
      for (JsonVariantConst part : chunk["candidates"][0]["content"]["parts"].as<JsonArrayConst>()) {
        feedText(part["text"].as<const char*>());
      }
    } else {
      feedText(chunk["choices"][0]["delta"]["content"].as<const char*>());
    }
  });
  if (!ok) job.parseError = true;
  buffer.size = 0;  // Bounded reusable PSRAM transport buffer.
  if (job.parseError) LOG_INF("AI_PRE", "SSE stream exceeds bounded parser capacity");
}

void AiSentencePrefetchSession::flushDeferred() {
  if (!worker_) return;
  Job& job = *worker_;
  for (size_t id = 0; id < job.batch.size(); ++id) {
    if (!job.deferred[id]) continue;
    // Retry completed JSON records only; never replay an incomplete fragment.
    JsonDocument parsed;
    const bool valid =
        deserializeJson(parsed, job.deferred[id], job.deferredBytes[id]) == DeserializationError::Ok;
    if (valid && saveItem(parsed.as<JsonVariantConst>(), job.scope, job.batch, job.stored)) {
      ai_prefetch::saveCheckpoint(job.scope, job.checkpoint);
      LOG_INF("AI_PRE", "Deferred stream item saved: id=%u", static_cast<unsigned>(id));
    }
    heap_caps_free(job.deferred[id]);
    job.deferred[id] = nullptr;
    job.deferredBytes[id] = 0;
  }
}

void AiSentencePrefetchSession::pollBatch() {
  if (!worker_) return;
  Job& job = *worker_;
  const auto mem = HalMemory::getInternalHeap();
  const bool expired = static_cast<uint32_t>(millis() - job.startedAt) >= HTTP_TIMEOUT_MS;
  if (WiFi.status() != WL_CONNECTED || expired || mem.freeBytes < MIN_INTERNAL_IN_FLIGHT) {
    // Salvage completed events already received, without any further TLS.
    if (esp_http_client_get_status_code(job.client) == 200) consumeStream();
    esp_http_client_cleanup(job.client);
    job.client = nullptr;
    flushDeferred();
    LOG_INF("AI_PRE", "Cancelled batch: timeout/Wi-Fi/RAM (free=%u)", static_cast<unsigned>(mem.freeBytes));
    worker_.reset();
    disabled_ = true;
    return;
  }

  const esp_err_t result = esp_http_client_perform(job.client);
  const int status = esp_http_client_get_status_code(job.client);
  if (status == 200) consumeStream();
  if (result == ESP_ERR_HTTP_EAGAIN) return;

  // Release TLS BEFORE any delayed JSON parse or SD write.
  esp_http_client_cleanup(job.client);
  job.client = nullptr;
  if (result == ESP_OK && status == 200 && job.rawNonStreaming && !job.parseError) {
    // Some OpenAI-compatible gateways ignore stream=true. Accept the complete
    // legacy body as a fallback without requiring a second paid API request.
    const std::string text = getOutput(job.response, job.gemini);
    if (!text.empty() && job.framer) {
      if (!job.framer->feedModel(text.data(), text.size(),
                                 [&](std::string_view record) { acceptStreamObject(record); })) {
        job.parseError = true;
      }
    }
  }
  flushDeferred();
  size_t savedCount = 0;
  for (size_t i = 0; i < job.batch.size(); ++i) {
    if (job.stored[i]) ++savedCount;
  }
  LOG_INF("AI_PRE", "Stream stored %u/%u sentences, HTTP=%d transport=%d%s",
          static_cast<unsigned>(savedCount), static_cast<unsigned>(job.batch.size()), status,
          static_cast<int>(result), job.parseError ? " parse-limit" : "");
  const bool complete = result == ESP_OK && status == 200 && savedCount == job.batch.size();
  worker_.reset();
  if (!complete) disabled_ = true;  // Retry missing pieces on next Translate.
}

AiSentencePrefetchSession::AiSentencePrefetchSession(ai_prefetch::Scope scope, Section* section, uint32_t spine,
                                                     int firstPage, uint32_t selectedWordOffset)
    : scope_(std::move(scope)), section_(section), spine_(spine), anchor_(selectedWordOffset), nextPage_(firstPage) {
  batch_.reserve(ai_prefetch::BATCH_SENTENCES);
  current_.spine = spine_;
  ai_prefetch::Checkpoint saved;
  LOG_INF("AI_PRE", "Translate lookahead initialized: spine=%u page=%d offset=%u", static_cast<unsigned>(spine_),
          nextPage_, static_cast<unsigned>(anchor_));
  if (ai_prefetch::loadCheckpoint(scope_, saved) && saved.spine == spine_ && saved.anchor == anchor_) {
    LOG_INF("AI_PRE", "Resume checkpoint: scanned=%u", static_cast<unsigned>(saved.scanned));
  }
}

AiSentencePrefetchSession::~AiSentencePrefetchSession() { cancel(); }

void AiSentencePrefetchSession::cancel() {
  stopped_ = true;
  if (worker_) {
    if (worker_->client && esp_http_client_get_status_code(worker_->client) == 200) consumeStream();
    if (worker_->client) {
      esp_http_client_cleanup(worker_->client);
      worker_->client = nullptr;
    }
    flushDeferred();  // Salvage complete items, never wait for another network packet.
  }
  worker_.reset();
  pageTokens_.clear();
  batch_.clear();
}

bool AiSentencePrefetchSession::openPage() {
  if (!section_ || nextPage_ >= static_cast<int>(section_->pageCount)) {
    scanEnded_ = true;  // Never build EPUB pages merely for AI.
    LOG_INF("AI_PRE", "Lookahead ended: available pages exhausted, scanned=%u", static_cast<unsigned>(considered_));
    return false;
  }
  if (!memoryReady()) return false;
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return false;
  auto page = section_->loadPage(nextPage_);
  if (!page) {
    scanEnded_ = true;
    return false;
  }
  ++nextPage_;
  pageTokens_.clear();
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine&>(*element).getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const auto range = block->wordSourceRange(i);
      const char* word = block->wordText(i);
      if (range.start == UINT32_MAX || range.end == UINT32_MAX || range.end <= range.start ||
          range.end <= lastWordEnd_ || !clippingText::hasVisibleText(word))
        continue;
      if (pageTokens_.size() >= MAX_PAGE_TOKENS) {
        disabled_ = true;
        pageTokens_.clear();
        LOG_INF("AI_PRE", "Page token cap reached; prefetch paused");
        return false;
      }
      Token token;
      token.text = word;
      token.start = range.start;
      token.end = range.end;
      token.paragraph = block->wordStartsParagraph(i);
      token.discretionaryHyphen = block->wordHasDiscretionaryHyphen(i);
      pageTokens_.push_back(std::move(token));
    }
  }
  std::stable_sort(pageTokens_.begin(), pageTokens_.end(),
                   [](const Token& lhs, const Token& rhs) { return lhs.start < rhs.start; });
  tokenIndex_ = 0;
  // The Page is released immediately, before any HTTPS request.
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
    if (!droppedSentence_) {
      ai_prefetch::Sentence delta;
      if (ai_prefetch::pending(scope_, current_, delta)) batch_.push_back(std::move(delta));
    }
  }
  current_ = {};
  current_.spine = spine_;
  droppedSentence_ = false;
}

void AiSentencePrefetchSession::acceptToken(const Token& token) {
  const char* raw = token.text.c_str();
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
                            token.discretionaryHyphen)) {
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
  if (batch_.empty()) return false;
  // Do not retain a full EPUB page token list while mbedTLS is allocating its
  // internal buffers. The next Translate tick may safely reload this SAME
  // prepared page and skip already-consumed source offsets after the batch.
  if (tokenIndex_ < pageTokens_.size() && nextPage_ > 0) --nextPage_;
  pageTokens_.clear();
  std::vector<Token>().swap(pageTokens_);
  tokenIndex_ = 0;
  if (!memoryReady()) {
    if (!reportedMemoryPause_) {
      const auto mem = HalMemory::getInternalHeap();
      LOG_INF("AI_PRE", "Batch waiting for TLS RAM: free=%u largest=%u need=%u/%u",
              static_cast<unsigned>(mem.freeBytes), static_cast<unsigned>(mem.largestBlockBytes),
              static_cast<unsigned>(MIN_INTERNAL_FREE), static_cast<unsigned>(MIN_INTERNAL_BLOCK));
      reportedMemoryPause_ = true;
    }
    return false;
  }
  reportedMemoryPause_ = false;
  auto job = std::make_unique<Job>();
  job->scope = scope_;
  job->batch = std::move(batch_);
  job->checkpoint = {spine_, anchor_, job->batch.back().end, considered_};
  batch_.clear();
  batch_.reserve(ai_prefetch::BATCH_SENTENCES);
  job->url = job->scope.url;
  if (!buildUrl(job->url, job->scope.model, job->gemini) ||
      !buildRequest(job->batch, job->gemini, job->scope.model, job->body) || !job->response.data || !job->framer ||
      !trustedtime::trustedNow()) {
    // Do not reapply the 94 KiB preflight AFTER allocating JSON and response
    // buffers. TLS gets its own contiguous-block check just below.
    LOG_INF("AI_PRE", "Batch skipped: endpoint, clock, parser or bounded PSRAM buffers");
    disabled_ = true;
    return false;
  }
  const auto heap = HalMemory::getInternalHeap();
  if (heap.freeBytes < 48 * 1024 || heap.largestBlockBytes < 20 * 1024) {
    LOG_INF("AI_PRE", "Batch TLS reserve low: free=%u block=%u", static_cast<unsigned>(heap.freeBytes),
            static_cast<unsigned>(heap.largestBlockBytes));
    disabled_ = true;
    return false;
  }
  esp_http_client_config_t cfg{};
  cfg.url = job->url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.is_async = true;
  cfg.timeout_ms = HTTP_STEP_TIMEOUT_MS;
  cfg.cert_pem = ai_translation::ROOTS;
  cfg.skip_cert_common_name_check = false;
  cfg.disable_auto_redirect = true;
  cfg.event_handler = onHttpEvent;
  cfg.user_data = &job->response;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 2048;
  job->client = esp_http_client_init(&cfg);
  if (!job->client) {
    disabled_ = true;
    LOG_INF("AI_PRE", "Batch skipped: HTTPS client initialization failed");
    return false;
  }
  esp_err_t result = esp_http_client_set_header(job->client, "Content-Type", "application/json");
  if (result == ESP_OK) result = esp_http_client_set_header(job->client, "User-Agent", "CrossPoint-AI-Prefetch");
  if (result == ESP_OK && job->gemini) {
    result = esp_http_client_set_header(job->client, "x-goog-api-key", AI_TRANSLATION_STORE.getKey().c_str());
  } else if (result == ESP_OK) {
    job->auth = "Bearer " + AI_TRANSLATION_STORE.getKey();
    result = esp_http_client_set_header(job->client, "Authorization", job->auth.c_str());
  }
  char session[40] = {};
  if (result == ESP_OK && ai_endpoint::isOpenCodeGoUrl(job->url)) {
    snprintf(session, sizeof(session), "cp-%08x%08x%08x%08x", static_cast<unsigned>(esp_random()),
             static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()),
             static_cast<unsigned>(esp_random()));
    result = esp_http_client_set_header(job->client, "x-opencode-session", session);
  }
  if (result == ESP_OK) {
    result = esp_http_client_set_post_field(job->client, job->body.data(), static_cast<int>(job->body.size()));
  }
  if (result != ESP_OK) {
    LOG_INF("AI_PRE", "Batch skipped: HTTPS setup error=%d", static_cast<int>(result));
    disabled_ = true;
    return false;
  }
  job->startedAt = millis();
  LOG_INF("AI_PRE", "Batch started: count=%u internal=%u largest=%u", static_cast<unsigned>(job->batch.size()),
          static_cast<unsigned>(HalMemory::getInternalHeap().freeBytes),
          static_cast<unsigned>(HalMemory::getInternalHeap().largestBlockBytes));
  worker_ = std::move(job);
  return true;
}

void AiSentencePrefetchSession::tick() {
  if (stopped_ || disabled_ || !section_) return;
  if (worker_) {
    pollBatch();  // At most one bounded asynchronous HTTPS step per UI tick.
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;
  // Finish a small, already-loaded token buffer even if it temporarily
  // occupies enough internal RAM to fail the stricter TLS preflight.
  // The batch launcher frees that buffer BEFORE checking TLS headroom.
  if (batch_.size() >= ai_prefetch::BATCH_SENTENCES) {
    launchBatch();
    return;
  }
  if (tokenIndex_ >= pageTokens_.size() && !memoryReady()) {
    if (!reportedMemoryPause_) {
      const auto mem = HalMemory::getInternalHeap();
      LOG_INF("AI_PRE", "Waiting for RAM: free=%u largest=%u need=%u/%u", static_cast<unsigned>(mem.freeBytes),
              static_cast<unsigned>(mem.largestBlockBytes), static_cast<unsigned>(MIN_INTERNAL_FREE),
              static_cast<unsigned>(MIN_INTERNAL_BLOCK));
      reportedMemoryPause_ = true;
    }
    return;
  }
  reportedMemoryPause_ = false;
  if (tokenIndex_ < pageTokens_.size() && HalMemory::getInternalHeap().freeBytes < 32 * 1024) {
    LOG_INF("AI_PRE", "Token scan paused: internal RAM low");
    disabled_ = true;
    return;
  }
  if (scanEnded_ || considered_ >= ai_prefetch::WINDOW_SENTENCES) {
    finishScan();
    if (!batch_.empty()) launchBatch();
    return;
  }
  if (tokenIndex_ >= pageTokens_.size()) {
    pageTokens_.clear();
    if (!openPage()) {
      if (scanEnded_) finishScan();
      if (scanEnded_ && !batch_.empty()) launchBatch();
      return;
    }
  }
  size_t stepped = 0;
  while (tokenIndex_ < pageTokens_.size() && stepped++ < TOKENS_PER_TICK &&
         batch_.size() < ai_prefetch::BATCH_SENTENCES && considered_ < ai_prefetch::WINDOW_SENTENCES) {
    const Token& token = pageTokens_[tokenIndex_++];
    acceptToken(token);
  }
  if (tokenIndex_ >= pageTokens_.size()) {
    pageTokens_.clear();
    std::vector<Token>().swap(pageTokens_);  // Release token RAM between pages.
    tokenIndex_ = 0;
  }
  if (batch_.size() == ai_prefetch::BATCH_SENTENCES) launchBatch();
}
