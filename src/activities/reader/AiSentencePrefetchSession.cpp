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
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <iterator>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "AITranslationStore.h"
#include "activities/RenderLock.h"
#include "clippings/ClippingText.h"
#include "util/AiApiEndpoint.h"
#include "util/AiBoundedJsonWriter.h"
#include "util/AiCompletionPolicy.h"
#include "util/AiHttpWorkerGate.h"
#include "util/AiPrefetchStream.h"
#include "util/AiSentenceUtils.h"
#include "util/AiTranslationRoots.h"

namespace {

constexpr size_t MAX_BATCH_BODY = 24 * 1024;
constexpr size_t MAX_RESPONSE = 48 * 1024;
constexpr int HTTP_TIMEOUT_MS = 60000;
// A single 60s socket timeout permits slow TLS/headers and bounds a stalled
// read. Never reconfigure the transport from inside its event callback;
// cancellation is cooperative and WiFi/BLE teardown waits for TLS cleanup.
constexpr int HTTP_READ_IDLE_TIMEOUT_MS = 60000;  // Single blocking socket timeout.
constexpr uint32_t MIN_INTERNAL_IN_FLIGHT = 15 * 1024;
// Strict preflight before allocating a dedicated HTTPS worker.
constexpr uint32_t MIN_INTERNAL_FREE = 94 * 1024;
constexpr uint32_t MIN_INTERNAL_BLOCK = 36 * 1024;
constexpr uint32_t MIN_PSRAM_FREE = 144 * 1024;
constexpr uint32_t MIN_PSRAM_BLOCK = 64 * 1024;
constexpr size_t TOKENS_PER_TICK = 48;
constexpr uint32_t BACKGROUND_MIN_BATCH_INTERVAL_MS = 25000;
constexpr size_t MAX_PAGE_TOKENS = 320;
constexpr size_t SSE_LINE_CAP = 8 * 1024;
constexpr size_t JSON_OBJECT_CAP = 24 * 1024;

// Temporary per-endpoint fallback: if chunked SSE transport fails, request
// ordinary completion JSON on later Translate sessions, without retrying a
// potentially billable request immediately.
std::string gNonStreamingEndpoint;
std::atomic<bool> gPrefetchTaskBusy{false};
constexpr uint32_t PREFETCH_TASK_STACK_BYTES = 12 * 1024;

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

bool buildUrl(std::string& url, const std::string& model, bool& gemini, bool streaming) {
  url = ai_endpoint::migrateOpenCodeGoUrl(url);
  if (!safeUrl(url)) return false;
  gemini = url == "https://generativelanguage.googleapis.com" ||
           url.rfind("https://generativelanguage.googleapis.com/", 0) == 0;
  if (gemini) {
    if (model.find('/') != std::string::npos || model.find(':') != std::string::npos) return false;
    const auto pos = url.find("/models/");
    if (pos != std::string::npos) url.resize(pos);
    if (url.back() == '/') url.pop_back();
    if (streaming) {
      url += "/models/" + model + ":streamGenerateContent?alt=sse";
    } else {
      url += "/models/" + model + ":generateContent?fields=candidates.content.parts.text";
    }
  } else {
    if (url.back() == '/') url.pop_back();
    if (!url.ends_with("/chat/completions")) url += "/chat/completions";
  }
  return true;
}

// Put JSON parsing allocations in Picco PSRAM: TLS can leave less than 26 KiB
// internal RAM, causing otherwise complete SSE items to be discarded.
class PsramJsonAllocator final : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t bytes) override { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void* ptr) override { heap_caps_free(ptr); }
  void* reallocate(void* ptr, size_t bytes) override {
    return heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};
PsramJsonAllocator gPsramJsonAllocator;

struct Response {
  char* data = nullptr;
  size_t size = 0;
  bool overflow = false;
  size_t totalBytes = 0;
  uint32_t dataEvents = 0;
  uint32_t startedAt = 0;
  bool headersReceived = false;
  bool firstByte = false;
  bool firstFrame = false;
  const char* phase = "connect";
  std::atomic<bool>* cancel = nullptr;
  void (*onDataReady)(void*) = nullptr;
  void* onDataContext = nullptr;

  explicit Response(std::atomic<bool>* cancelToken) : cancel(cancelToken) {
    data = static_cast<char*>(heap_caps_malloc(MAX_RESPONSE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data) data[0] = '\0';
  }
  ~Response() { heap_caps_free(data); }
  Response(const Response&) = delete;
  Response& operator=(const Response&) = delete;
};

const char* httpResultName(esp_err_t result) {
  switch (result) {
    case ESP_OK:
      return "ESP_OK";
    case ESP_ERR_HTTP_EAGAIN:
      return "ESP_ERR_HTTP_EAGAIN";
    case ESP_ERR_HTTP_FETCH_HEADER:
      return "ESP_ERR_HTTP_FETCH_HEADER";
    case ESP_ERR_HTTP_CONNECT:
      return "ESP_ERR_HTTP_CONNECT";
    case ESP_ERR_HTTP_WRITE_DATA:
      return "ESP_ERR_HTTP_WRITE_DATA";
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
      return "ESP_ERR_HTTP_CONNECTION_CLOSED";
    case ESP_ERR_HTTP_BASE + 11:
      return "ESP_ERR_HTTP_READ_TIMEOUT";
    case ESP_ERR_HTTP_BASE + 12:
      return "ESP_ERR_HTTP_INCOMPLETE_DATA";
    default:
      return esp_err_to_name(result);
  }
}

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
  if (!event) return ESP_FAIL;
  auto* result = static_cast<Response*>(event->user_data);
  if (!result) return ESP_FAIL;
  // Only the worker owns the handle. Never close or mutate it in a callback.
  if (result->cancel->load(std::memory_order_acquire)) return ESP_FAIL;
  // Bound a stream whose provider keeps sending bytes indefinitely. Socket
  // timeout handles a stalled read; no reentrant timeout setter is needed.
  if (result->startedAt && millis() - result->startedAt >= HTTP_TIMEOUT_MS) return ESP_FAIL;
  switch (event->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
      result->phase = "fetch_headers";
      LOG_INF("AI_PRE", "Phase: connected elapsed_ms=%u", static_cast<unsigned>(millis() - result->startedAt));
      break;
    case HTTP_EVENT_HEADERS_SENT:
      LOG_INF("AI_PRE", "Phase: request_sent elapsed_ms=%u", static_cast<unsigned>(millis() - result->startedAt));
      break;
    case HTTP_EVENT_ON_HEADER:
      if (!result->headersReceived) {
        result->headersReceived = true;
        result->phase = "first_byte";
        LOG_INF("AI_PRE", "Phase: response_headers elapsed_ms=%u",
                static_cast<unsigned>(millis() - result->startedAt));
      }
      break;
    case HTTP_EVENT_ON_DATA:
      if (!result->firstByte) {
        result->firstByte = true;
        result->phase = "body";
        LOG_INF("AI_PRE", "Phase: first_byte elapsed_ms=%u", static_cast<unsigned>(millis() - result->startedAt));
      }
      break;
    default: break;
  }
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  if (!result->data || event->data_len < 0 || static_cast<size_t>(event->data_len) > MAX_RESPONSE - result->size) {
    result->overflow = true;
    return ESP_FAIL;
  }
  const size_t bytes = static_cast<size_t>(event->data_len);
  memcpy(result->data + result->size, event->data, bytes);
  result->size += bytes;
  result->totalBytes += bytes;
  ++result->dataEvents;
  result->data[result->size] = '\0';
  // Decode the current SSE block before the next HTTP body callback. The
  // bounded 48-KiB PSRAM buffer is reused, so streamed completions can exceed
  // 48 KiB in total without overflowing. Never call this for ordinary JSON.
  if (result->onDataReady) result->onDataReady(result->onDataContext);
  return ESP_OK;
}

// Keep untrusted model output bounded, and never log book text, model output,
// credentials, or provider error payloads.
std::string getOutput(const Response& response, bool gemini) {
  JsonDocument answer(&gPsramJsonAllocator);
  JsonDocument filter(&gPsramJsonAllocator);
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

bool buildRequest(const std::vector<ai_prefetch::Sentence>& batch, bool gemini, bool streaming,
                  const std::string& model, std::string_view reasoningEffort, unsigned maxTokens,
                   char* body, size_t capacity, size_t& bodyBytes) {
  // All large serialization buffers are in PSRAM. No intermediate ArduinoJson
  // documents, copied prompt strings or payload strings occupy internal heap.
  auto promptBuffer = HalMemory::allocatePsram(17 * 1024);
  if (!promptBuffer || !body) return false;
  ai_prefetch::BoundedJsonWriter prompt(reinterpret_cast<char*>(promptBuffer.get()), 17 * 1024);
  if (!prompt.append("Translate each numbered sentence into natural, faithful Russian. "
                     "Return one standalone JSON object per line (NDJSON) in id order, "
                     "as soon as it is ready: {\"id\":0,\"translation\":\"...\"}. "
                     "Translate the whole sentence, preserving tone and punctuation. "
                     "No analysis, word mappings, explanations, markdown or enclosing array. "
                     "Source: {\"sentences\":["))
    return false;
  for (size_t i = 0; i < batch.size(); ++i) {
    if (i && !prompt.append(",")) return false;
    if (!prompt.append("{\"id\":") || !prompt.integer(static_cast<unsigned>(i)) ||
        !prompt.append(",\"text\":") || !prompt.quoted(batch[i].source) || !prompt.append("}"))
      return false;
  }
  if (!prompt.append("]}")) return false;

  ai_prefetch::BoundedJsonWriter request(body, capacity);
  if (gemini) {
    if (!request.append("{\"contents\":[{\"parts\":[{\"text\":") || !request.quoted(prompt.view()) ||
        !request.append("}]}],\"generationConfig\":{\"maxOutputTokens\":2048}}"))
      return false;
  } else {
    if (!request.append("{\"model\":") || !request.quoted(model)) return false;
    if (reasoningEffort != "default" &&
        (!request.append(",\"reasoning_effort\":") || !request.quoted(reasoningEffort)))
      return false;
    if (!request.append(",\"max_tokens\":") || !request.integer(maxTokens) ||
        !request.append(streaming
                            ? ",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":"
                            : ",\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":") ||
        !request.quoted(prompt.view()) || !request.append("}]}"))
      return false;
  }
  bodyBytes = request.size();
  return bodyBytes > 0;
}

// Only complete, validated sentence translations are committed. Word mappings
// are neither sent to the model nor required for the popup.
bool saveItem(JsonVariantConst item, const ai_prefetch::Scope& scope, const std::vector<ai_prefetch::Sentence>& batch,
              bool* stored) {
  if (!item["id"].is<int>()) return false;
  const int id = item["id"].as<int>();
  if (id < 0 || id >= static_cast<int>(batch.size())) return false;
  const char* translated = item["translation"].as<const char*>();
  if (!translated || !*translated || strlen(translated) > 4096) return false;
  ai_prefetch::Sentence record = batch[id];
  record.translation = translated;
  record.words.clear();
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
  HalMemory::PsramBuffer body = HalMemory::allocatePsram(MAX_BATCH_BODY);
  size_t bodyBytes = 0;
  std::string auth;
  bool gemini = false;
  bool streaming = true;
  std::atomic<bool> cancel{false};
  std::atomic<bool> done{false};
  size_t savedCount = 0;
  uint32_t completedObjects = 0;
  uint32_t recognizedItems = 0;
  uint32_t discardedObjects = 0;
  uint32_t objectsParseFail = 0;
  uint32_t objectsBadId = 0;
  Response response{&cancel};
  esp_http_client_handle_t client = nullptr;
  uint32_t startedAt = 0;
  bool classified = false;
  bool rawNonStreaming = false;
  bool parseError = false;
  uint32_t sseFrames = 0;
  uint32_t textParts = 0;
  uint32_t framesParseFail = 0;
  uint32_t framesWithText = 0;
  uint32_t modelChars = 0;
  uint32_t reasoningFrames = 0;
  uint32_t finishLength = 0;
  bool firstFrameShapeLogged = false;
  std::atomic<bool> shutdownWifiOnExit{false};
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

void AiSentencePrefetchSession::acceptStreamObject(Job& job, std::string_view json) {
  ++job.completedObjects;
  JsonDocument parsed(&gPsramJsonAllocator);
  if (deserializeJson(parsed, json.data(), json.size()) != DeserializationError::Ok) {
    ++job.objectsParseFail;
    ++job.discardedObjects;
    return;
  }
  const auto accept = [&](JsonVariantConst item) {
    if (!item["id"].is<int>()) {
      ++job.objectsBadId;
      ++job.discardedObjects;
      return;
    }
    const int id = item["id"].as<int>();
    if (id < 0 || id >= static_cast<int>(job.batch.size())) {
      ++job.objectsBadId;
      ++job.discardedObjects;
      return;
    }
    if (job.accepted[id] >= 4) return;
    ++job.recognizedItems;
    if (saveItem(item, job.scope, job.batch, job.stored)) {
      ++job.accepted[id];
      if (job.accepted[id] == 1) LOG_INF("AI_PRE", "Phase: stored id=%d", id);
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
  } else if (JsonArrayConst array = parsed.as<JsonArrayConst>(); !array.isNull()) {
    for (JsonVariantConst item : array) accept(item);
  } else {
    accept(parsed.as<JsonVariantConst>());
  }
}

void AiSentencePrefetchSession::consumeStream(Job& job) {
  if (!job.response.size || !job.framer) return;
  Response& buffer = job.response;
  if (!job.classified) {
    size_t i = 0;
    while (i < buffer.size &&
           (buffer.data[i] == ' ' || buffer.data[i] == '\r' || buffer.data[i] == '\n' || buffer.data[i] == '\t'))
      ++i;
    if (i == buffer.size) return;
    job.rawNonStreaming = buffer.data[i] == '{' || buffer.data[i] == '[';
    job.classified = true;
  }
  if (job.rawNonStreaming) return;  // Buffer ordinary JSON until HTTP completes.
  const bool ok = job.framer->feedSse(buffer.data, buffer.size, [&](std::string_view frame) {
    if (job.parseError || job.cancel.load(std::memory_order_acquire)) return;
    ++job.sseFrames;
    if (!job.response.firstFrame) {
      job.response.firstFrame = true;
      job.response.phase = "sse";
      LOG_INF("AI_PRE", "Phase: first_sse_frame elapsed_ms=%u",
              static_cast<unsigned>(millis() - job.startedAt));
    }
    JsonDocument chunk(&gPsramJsonAllocator);
    const bool parsed = deserializeJson(chunk, frame.data(), frame.size()) == DeserializationError::Ok;
    if (!job.firstFrameShapeLogged) {
      job.firstFrameShapeLogged = true;
      // Whitelist JSON punctuation only. Mask all letters, digits, UTF-8
      // bytes and controls: no book/model/API text can appear in this log.
      char shape[81] = {};
      const size_t count = std::min(frame.size(), sizeof(shape) - 1);
      for (size_t i = 0; i < count; ++i) {
        const char ch = frame[i];
        shape[i] = ch == '{' || ch == '}' || ch == '[' || ch == ']' || ch == ':' ||
                           ch == ',' || ch == '"' || ch == '\\'
                       ? ch
                       : 'x';
      }
      const JsonVariantConst content = chunk["choices"][0]["delta"]["content"];
      const char* kind = !parsed ? "parse_fail" : content.is<const char*>() ? "string"
                                                : content.is<JsonArrayConst>() ? "array"
                                                : content.isNull() ? "missing" : "other";
      const bool hasReasoning = !chunk["choices"][0]["delta"]["reasoning_content"].isNull() ||
                                !chunk["choices"][0]["delta"]["reasoning"].isNull();
      LOG_INF("AI_PRE", "SSE first frame: delta_content=%s reasoning=%u bytes=%u shape=%s", kind,
              static_cast<unsigned>(hasReasoning), static_cast<unsigned>(frame.size()), shape);
    }
    if (!parsed) {
      ++job.framesParseFail;
      return;
    }
    if (!chunk["choices"][0]["delta"]["reasoning_content"].isNull() ||
        !chunk["choices"][0]["delta"]["reasoning"].isNull()) {
      ++job.reasoningFrames;  // Reasoning is never cached as a translation.
    }
    const char* finish = chunk["choices"][0]["finish_reason"].as<const char*>();
    if (finish && strcmp(finish, "length") == 0) ++job.finishLength;
    const char* geminiFinish = chunk["candidates"][0]["finishReason"].as<const char*>();
    if (geminiFinish && strcmp(geminiFinish, "MAX_TOKENS") == 0) ++job.finishLength;

    bool frameHadText = false;
    const auto feedText = [&](const char* text) {
      if (!text || !*text || job.parseError || job.cancel.load(std::memory_order_acquire)) return;
      frameHadText = true;
      ++job.textParts;
      const size_t len = strlen(text);
      job.modelChars += static_cast<uint32_t>(len);
      if (!job.framer->feedModel(text, len, [&](std::string_view record) { acceptStreamObject(job, record); })) {
        job.parseError = true;
      }
    };
    const auto feedContent = [&](JsonVariantConst content) {
      if (const char* text = content.as<const char*>()) {
        feedText(text);
      } else if (JsonArrayConst parts = content.as<JsonArrayConst>(); !parts.isNull()) {
        for (JsonVariantConst part : parts) {
          if (const char* text = part.as<const char*>()) {
            feedText(text);
          } else {
            feedText(part["text"].as<const char*>());
          }
        }
      } else {
        feedText(content["text"].as<const char*>());
      }
    };
    if (job.gemini) {
      feedContent(chunk["candidates"][0]["content"]["parts"]);
    } else {
      feedContent(chunk["choices"][0]["delta"]["content"]);
      if (!frameHadText) feedContent(chunk["choices"][0]["message"]["content"]);
      if (!frameHadText) feedContent(chunk["choices"][0]["text"]);
      if (!frameHadText) feedContent(chunk["output_text"]);
      const char* type = chunk["type"].as<const char*>();
      if (!frameHadText && type && strcmp(type, "response.output_text.delta") == 0) {
        feedContent(chunk["delta"]);
      }
    }
    if (frameHadText) ++job.framesWithText;
  });
  if (!ok) job.parseError = true;
  buffer.size = 0;  // Bounded reusable PSRAM transport buffer.
  if (job.parseError) LOG_INF("AI_PRE", "SSE stream exceeds bounded parser capacity");
}

void AiSentencePrefetchSession::flushDeferred(Job& job) {
  for (size_t id = 0; id < job.batch.size(); ++id) {
    if (!job.deferred[id]) continue;
    // Retry completed JSON records only; never replay an incomplete fragment.
    JsonDocument parsed(&gPsramJsonAllocator);
    const bool valid = deserializeJson(parsed, job.deferred[id], job.deferredBytes[id]) == DeserializationError::Ok;
    bool saved = false;
    if (valid) {
      JsonArrayConst items = parsed["items"].as<JsonArrayConst>();
      if (items.isNull()) {
        saved = saveItem(parsed.as<JsonVariantConst>(), job.scope, job.batch, job.stored);
      } else {
        for (JsonVariantConst item : items) {
          if (item["id"].is<int>() && item["id"].as<int>() == static_cast<int>(id)) {
            saved |= saveItem(item, job.scope, job.batch, job.stored);
          }
        }
      }
    }
    if (saved) {
      ai_prefetch::saveCheckpoint(job.scope, job.checkpoint);
      LOG_INF("AI_PRE", "Deferred stream item saved: id=%u", static_cast<unsigned>(id));
    }
    heap_caps_free(job.deferred[id]);
    job.deferred[id] = nullptr;
    job.deferredBytes[id] = 0;
  }
}

void AiSentencePrefetchSession::runBatch(void* data) {
  // The task owns its own shared job reference. It does not touch the popup,
  // EPUB pages, renderer, or anything freed when Translate closes.
  std::unique_ptr<std::shared_ptr<Job>> argument(static_cast<std::shared_ptr<Job>*>(data));
  std::shared_ptr<Job> holder = std::move(*argument);
  argument.reset();
  Job& job = *holder;

  esp_err_t result = ESP_FAIL;
  bool performed = false;
  int status = -1;
  int tlsError = 0;
  int tlsFlags = 0;
  int socketError = 0;
  const char* stopReason = "not_started";
  if (job.cancel.load(std::memory_order_acquire)) {
    stopReason = "cancelled";
  } else if (WiFi.status() != WL_CONNECTED) {
    stopReason = "wifi_disconnected";
  } else if (HalMemory::getInternalHeap().freeBytes < MIN_INTERNAL_IN_FLIGHT) {
    stopReason = "low_internal_heap";
  } else if (job.client) {
    // This is blocking ONLY in the dedicated worker, never on the UI task.
    // Exactly one perform call: no repeated, potentially billable POST.
    performed = true;
    result = esp_http_client_perform(job.client);
    status = esp_http_client_get_status_code(job.client);
    if (status == 200) consumeStream(job);
  }

  if (job.client) {
    socketError = esp_http_client_get_errno(job.client);
    if (result != ESP_OK) {
      esp_http_client_get_and_clear_last_tls_error(job.client, &tlsError, &tlsFlags);
    }
    esp_http_client_cleanup(job.client);  // TLS freed before any deferred cache writes
    job.client = nullptr;
  }
  if (result == ESP_OK && status == 200 && job.rawNonStreaming && !job.parseError &&
      !job.cancel.load(std::memory_order_acquire)) {
    const std::string output = getOutput(job.response, job.gemini);
    if (!output.empty() && job.framer &&
        !job.framer->feedModel(output.data(), output.size(),
                               [&](std::string_view record) { acceptStreamObject(job, record); })) {
      job.parseError = true;
    }
  }
  // Save only completed records already received before closing Translate.
  flushDeferred(job);
  for (size_t i = 0; i < job.batch.size(); ++i) {
    if (job.stored[i]) ++job.savedCount;
  }
  if (job.cancel.load(std::memory_order_acquire)) {
    stopReason = "cancelled";
  } else if (performed) {
    if (result != ESP_OK) {
      stopReason = "transport_error";
    } else if (status != 200) {
      stopReason = "http_status";
    } else if (job.response.overflow) {
      stopReason = "response_overflow";
    } else if (job.parseError) {
      stopReason = "parser_error";
    } else {
      stopReason = job.savedCount ? "completed" : "no_usable_items";
    }
  }
  // logPrintf has a 256-byte cap, so keep every diagnostic line short.
  // In particular, reason=cancelled must never be truncated away.
  LOG_INF("AI_PRE",
          "Worker stored %u/%u HTTP=%d err=%d err_name=%s bytes=%u frames=%u objects=%u "
          "recognized=%u reason=%s phase=%s elapsed_ms=%u",
          static_cast<unsigned>(job.savedCount), static_cast<unsigned>(job.batch.size()), status,
          static_cast<int>(result), httpResultName(result),
          static_cast<unsigned>(job.response.totalBytes), static_cast<unsigned>(job.sseFrames),
          static_cast<unsigned>(job.completedObjects), static_cast<unsigned>(job.recognizedItems),
          stopReason, job.response.phase, static_cast<unsigned>(millis() - job.startedAt));
  LOG_INF("AI_PRE",
          "Stream diagnostics: frames_parse_fail=%u frames_with_text=%u textParts=%u model_chars=%u "
          "objects_parse_fail=%u objects_bad_id=%u rejected=%u",
          static_cast<unsigned>(job.framesParseFail), static_cast<unsigned>(job.framesWithText),
          static_cast<unsigned>(job.textParts), static_cast<unsigned>(job.modelChars),
          static_cast<unsigned>(job.objectsParseFail), static_cast<unsigned>(job.objectsBadId),
          static_cast<unsigned>(job.discardedObjects));
  LOG_INF("AI_PRE",
          "Transport diagnostics: tls=%d tls_flags=%d socket_errno=%d reasoning_frames=%u finish_length=%u",
          tlsError, tlsFlags, socketError,
          static_cast<unsigned>(job.reasoningFrames), static_cast<unsigned>(job.finishLength));
  // The handle and deferred writes are already gone. Publish completion, then
  // exchange the WiFi handoff bit to handle a concurrent UI exit exactly once.
  job.done.store(true, std::memory_order_release);
  if (job.shutdownWifiOnExit.exchange(false, std::memory_order_acq_rel)) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    LOG_INF("AI_PRE", "Cancelled worker: WiFi shutdown after HTTP cleanup");
  }
  // Release job buffers before allowing BLE to start. The worker's stack
  // is released by vTaskDeleteWithCaps immediately afterwards.
  holder.reset();
  gPrefetchTaskBusy.store(false, std::memory_order_release);
  ai_http_gate::release();
  vTaskDeleteWithCaps(nullptr);
}

void AiSentencePrefetchSession::pollBatch() {
  // done is published just before the worker's deferred WiFi handoff and
  // PSRAM cleanup. Keep the job attached until the global HTTPS lease clears,
  // so a reader exit cannot lose the worker responsible for radio teardown.
  if (!worker_ || !worker_->done.load(std::memory_order_acquire) ||
      gPrefetchTaskBusy.load(std::memory_order_acquire))
    return;
  const bool complete = worker_->savedCount == worker_->batch.size();
  savedSentences_ += static_cast<uint16_t>(worker_->savedCount);
  if (!complete && worker_->streaming && worker_->sseFrames > 0 && worker_->savedCount == 0) {
    gNonStreamingEndpoint = worker_->scope.url;
    LOG_INF("AI_PRE", "No usable JSON objects in SSE; next batch uses ordinary JSON");
  }
  worker_.reset();
  if (!complete) disabled_ = true;  // No retry storm within this session.
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

void AiSentencePrefetchSession::runInBackground(std::function<bool()> sectionStillLive,
                                                uint16_t requestLimit, uint32_t lastBookBatchAt) {
  sectionStillLive_ = std::move(sectionStillLive);
  maxRequests_ = requestLimit;
  if (lastBatchAt_ == 0) lastBatchAt_ = lastBookBatchAt;
  backgroundMode_ = true;
}

bool AiSentencePrefetchSession::finished() const {
  return !worker_ && (disabled_ || stopped_ || requestsStarted_ >= maxRequests_ ||
                      ((scanEnded_ || considered_ >= ai_prefetch::WINDOW_SENTENCES) && batch_.empty()));
}

bool AiSentencePrefetchSession::httpWorkerBusy() {
  return ai_http_gate::busy();
}

bool AiSentencePrefetchSession::cancel(bool shutdownWifi) {
  stopped_ = true;
  auto worker = std::move(worker_);
  if (worker) {
    if (shutdownWifi) worker->shutdownWifiOnExit.store(true, std::memory_order_release);
    worker->cancel.store(true, std::memory_order_release);
    // If worker finished before handoff, its HTTP client is already cleaned.
    if (shutdownWifi && worker->done.load(std::memory_order_acquire) &&
        worker->shutdownWifiOnExit.exchange(false, std::memory_order_acq_rel)) {
      WiFi.disconnect(false);
      WiFi.mode(WIFI_OFF);
      LOG_INF("AI_PRE", "Cancelled completed worker: WiFi shutdown after HTTP cleanup");
    }
  }
  pageTokens_.clear();
  batch_.clear();
  return shutdownWifi && static_cast<bool>(worker);
}

bool AiSentencePrefetchSession::openPage() {
  if (!section_ || !memoryReady()) return false;
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return false;
  if (sectionStillLive_ && !sectionStillLive_()) {
    disabled_ = true;
    LOG_INF("AI_BG", "Stop: reader section changed during scan");
    return false;
  }
  if (nextPage_ >= static_cast<int>(section_->pageCount)) {
    scanEnded_ = true;  // Never build EPUB pages merely for AI.
    LOG_INF("AI_PRE", "Lookahead ended: available pages exhausted, scanned=%u", static_cast<unsigned>(considered_));
    return false;
  }
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
  lastWordEnd_ = token.end;
  if (ai_sentence::isEnd(raw)) completeSentence();
}

void AiSentencePrefetchSession::finishScan() {
  if (!scanEnded_) scanEnded_ = true;
  completeSentence();
}

bool AiSentencePrefetchSession::launchBatch() {
  if (batch_.empty()) return false;
  if (requestsStarted_ >= maxRequests_) {
    disabled_ = true;
    LOG_INF("AI_BG", "Budget reached: requests=%u saved=%u",
            static_cast<unsigned>(requestsStarted_), static_cast<unsigned>(savedSentences_));
    return false;
  }
  if (backgroundMode_ && lastBatchAt_ != 0 &&
      static_cast<uint32_t>(millis() - lastBatchAt_) < BACKGROUND_MIN_BATCH_INTERVAL_MS)
    return false;
  // Do not retain a full EPUB page token list while mbedTLS is allocating its
  // internal buffers. The next Translate tick may safely reload this SAME
  // prepared page and skip already-consumed source offsets after the batch.
  if (tokenIndex_ < pageTokens_.size() && nextPage_ > 0) --nextPage_;
  pageTokens_.clear();
  std::vector<Token>().swap(pageTokens_);
  tokenIndex_ = 0;
  if (gPrefetchTaskBusy.load(std::memory_order_acquire) || ai_http_gate::busy()) return false;
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
  job->streaming = job->scope.url != gNonStreamingEndpoint;
  if (!job->streaming) LOG_INF("AI_PRE", "Using non-streaming compatibility mode for this endpoint");
  const bool endpointReady = buildUrl(job->url, job->scope.model, job->gemini, job->streaming);
  const bool fastHy4 = ai_completion::isHy4Go(job->scope.model, job->url);
  const std::string_view reasoningEffort =
      ai_completion::reasoningEffort(job->scope.model, job->url, AI_TRANSLATION_STORE.getReasoningEffort());
  // A single 4096-token request allows headroom if the gateway still emits reasoning.
  const unsigned maxTokens = fastHy4 ? 4096u : 2048u;
  bool requestReady =
      endpointReady && buildRequest(job->batch, job->gemini, job->streaming, job->scope.model, reasoningEffort, maxTokens,
                                    reinterpret_cast<char*>(job->body.get()), MAX_BATCH_BODY, job->bodyBytes);
  while (!requestReady && job->batch.size() > 1 && endpointReady) {
    // A long paragraph may exceed the fixed request cap. Preserve FIFO
    // priority and split into smaller packets rather than dropping the entire
    // 100-sentence lookahead window.
    batch_.insert(batch_.begin(), std::move(job->batch.back()));
    job->batch.pop_back();
    requestReady = buildRequest(job->batch, job->gemini, job->streaming, job->scope.model, reasoningEffort, maxTokens,
                                reinterpret_cast<char*>(job->body.get()), MAX_BATCH_BODY, job->bodyBytes);
  }
  if (!requestReady || !job->response.data || !job->framer || !trustedtime::trustedNow()) {
    // Do not reapply the 94 KiB preflight AFTER allocating JSON and response
    // buffers. TLS gets its own contiguous-block check just below.
    LOG_INF("AI_PRE", "Batch skipped: endpoint, clock, parser or bounded PSRAM buffers");
    disabled_ = true;
    return false;
  }
  job->checkpoint.nextOffset = job->batch.back().end;
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
  cfg.is_async = false;  // The dedicated FreeRTOS worker owns this blocking client.
  cfg.timeout_ms = HTTP_TIMEOUT_MS;  // Connect, headers and first byte may be slow.
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
  if (result == ESP_OK)
    result =
        esp_http_client_set_header(job->client, "Accept", job->streaming ? "text/event-stream" : "application/json");
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
    result = esp_http_client_set_post_field(job->client, reinterpret_cast<char*>(job->body.get()),
                                            static_cast<int>(job->bodyBytes));
  }
  if (result != ESP_OK) {
    LOG_INF("AI_PRE", "Batch skipped: HTTPS setup error=%d", static_cast<int>(result));
    disabled_ = true;
    return false;
  }
  job->startedAt = millis();
  job->response.startedAt = job->startedAt;
  LOG_INF("AI_PRE",
          "Batch started: count=%u mode=%s effort=%.*s budget=%u header_timeout=%d idle_timeout=%d internal=%u largest=%u",
          static_cast<unsigned>(job->batch.size()), job->streaming ? "sse" : "json",
          static_cast<int>(job->gemini ? sizeof("ignored-gemini") - 1 : reasoningEffort.size()),
           job->gemini ? "ignored-gemini" : reasoningEffort.data(), maxTokens, HTTP_TIMEOUT_MS,
          HTTP_READ_IDLE_TIMEOUT_MS,
          static_cast<unsigned>(HalMemory::getInternalHeap().freeBytes),
          static_cast<unsigned>(HalMemory::getInternalHeap().largestBlockBytes));
  // Allocate a separate low-priority FreeRTOS stack in PSRAM so blocking TLS
  // cannot block Translate input. A global lease prevents overlapping tasks
  // when the popup is closed and reopened quickly.
  bool wasBusy = false;
  if (!gPrefetchTaskBusy.compare_exchange_strong(wasBusy, true, std::memory_order_acq_rel)) return false;
  if (!ai_http_gate::acquire()) {
    batch_.insert(batch_.begin(), std::make_move_iterator(job->batch.begin()),
                  std::make_move_iterator(job->batch.end()));
    gPrefetchTaskBusy.store(false, std::memory_order_release);
    return false;
  }
  std::shared_ptr<Job> shared = std::move(job);
  if (shared->streaming) {
    shared->response.onDataContext = shared.get();
    shared->response.onDataReady = [](void* instance) { consumeStream(*static_cast<Job*>(instance)); };
  }
  auto* argument = new (std::nothrow) std::shared_ptr<Job>(shared);
  if (!argument ||
      xTaskCreateWithCaps(&AiSentencePrefetchSession::runBatch, "ai-prefetch", PREFETCH_TASK_STACK_BYTES, argument,
                          tskIDLE_PRIORITY + 1, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    delete argument;
    gPrefetchTaskBusy.store(false, std::memory_order_release);
    ai_http_gate::release();
    LOG_INF("AI_PRE", "Cannot create PSRAM HTTPS worker; prefetch disabled safely");
    disabled_ = true;
    return false;
  }
  worker_ = std::move(shared);
  ++requestsStarted_;
  lastBatchAt_ = millis();
  return true;
}

void AiSentencePrefetchSession::tick() {
  if (stopped_ || disabled_ || !section_) return;
  if (worker_) {
    pollBatch();  // Only inspect worker completion; no HTTPS runs on the UI task.
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
