#include "AiCacheServer.h"

#if defined(FREEINK_DEVICE_PICCO)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BlePageTurner.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
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
#include <new>
#include <string>
#include <utility>

#include "WifiCredentialStore.h"
#include "util/AiBookCache.h"
#include "util/AiCacheServerUrl.h"
#include "util/AiHttpWorkerGate.h"
#include "util/AiTranslationRoots.h"

namespace ai_cache_server {
namespace {
constexpr uint32_t POLL_INTERVAL_MS = 15000;
constexpr uint32_t WIFI_JOIN_TIMEOUT_MS = 12000;
constexpr uint32_t MIN_INTERNAL_FREE = 94 * 1024;
constexpr uint32_t MIN_INTERNAL_BLOCK = 36 * 1024;
constexpr uint32_t MIN_PSRAM_FREE = 128 * 1024;
constexpr uint64_t MAX_EPUB_BYTES = 256ULL * 1024 * 1024;
constexpr uint64_t MAX_PACKAGE_BYTES = 256ULL * 1024 * 1024;
constexpr uint32_t WORKER_STACK_BYTES = 12 * 1024;
constexpr char BOUNDARY[] = "crosspoint-aic-v1";

enum class Step : uint8_t { Status, Upload, Download };
struct Work {
  std::string path;
  std::string url;
  std::string token;
  std::string sha;
  std::string expectedAicSha;
  std::string error;
  uint8_t bookSha[32]{};
  Step step = Step::Status;
  Step next = Step::Status;
  uint32_t doneCount = 0;
  uint32_t totalCount = 0;
  uint64_t bytes = 0;
  bool changed = false;
  bool complete = false;
  bool finished = false;
  bool succeeded = false;
  std::atomic<bool> cancel{false};
  std::atomic<bool> shutdownWifiOnExit{false};
  std::atomic<bool> done{false};
};

struct State {
  std::string bookPath;
  std::string baseUrl;
  std::string token;
  std::string sha;
  std::string aicSha;
  uint8_t bookSha[32]{};
  std::shared_ptr<Work> worker;
  Step step = Step::Status;
  uint32_t lastDone = 0;
  uint32_t pendingDone = 0;
  uint32_t nextPollAt = 0;
  uint32_t joiningAt = 0;
  bool wifiOwned = false;
  bool joining = false;
  bool terminal = false;
  bool updated = false;
};
State state;

bool hex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (const char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::string toHex(const uint8_t digest[32]) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    out[i * 2] = digits[digest[i] >> 4];
    out[i * 2 + 1] = digits[digest[i] & 15];
  }
  return out;
}

void setError(Work& work, const char* reason) {
  work.error = reason;
  work.succeeded = false;
}

esp_http_client_handle_t clientFor(const std::string& url, esp_http_client_method_t method) {
  esp_http_client_config_t cfg{};
  cfg.url = url.c_str();
  cfg.method = method;
  cfg.timeout_ms = 15000;
  cfg.cert_pem = ai_translation::ROOTS;
  cfg.disable_auto_redirect = true;  // Never send EPUB bytes to a redirect.
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 2048;
  auto* client = esp_http_client_init(&cfg);
  if (client) esp_http_client_set_header(client, "User-Agent", "CrossPoint-AI-Cache/1");
  return client;
}

bool beginResponse(esp_http_client_handle_t client, int64_t requestBytes, int& status) {
  if (requestBytes > INT32_MAX || esp_http_client_open(client, static_cast<int>(requestBytes)) != ESP_OK)
    return false;
  // Chunked responses return -1 for content length; the HTTP status is still valid.
  esp_http_client_fetch_headers(client);
  status = esp_http_client_get_status_code(client);
  return status > 0;
}

bool readStatus(Work& work, std::string& serverState) {
  const std::string endpoint = buildRequestUrl(work.url, "/status", work.sha, work.token);
  auto* client = clientFor(endpoint, HTTP_METHOD_GET);
  if (!client) return false;
  int status = 0;
  const bool received = beginResponse(client, 0, status);
  if (received && status == 401) setError(work, "unauthorized");
  bool ok = received && status == 200;
  std::string body;
  if (ok) {
    char buffer[256];
    for (;;) {
      if (work.cancel.load()) { ok = false; break; }
      const int n = esp_http_client_read(client, buffer, sizeof(buffer));
      if (n < 0 || (n == 0 && !esp_http_client_is_complete_data_received(client))) {
        ok = false;
        break;
      }
      if (n == 0) break;
      if (body.size() + static_cast<size_t>(n) > 1536) { ok = false; break; }
      body.append(buffer, static_cast<size_t>(n));
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (!ok) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok || !doc["state"].is<const char*>())
    return false;
  serverState = doc["state"].as<const char*>();
  if (serverState != "missing" && serverState != "queued" && serverState != "translating" &&
      serverState != "ready" && serverState != "error") return false;
  work.doneCount = doc["done"] | 0u;
  work.totalCount = doc["total"] | 0u;
  work.expectedAicSha = std::string(doc["aic_sha256"] | "");
  return work.expectedAicSha.empty() || hex64(work.expectedAicSha);
}

bool uploadEpub(Work& work) {
  HalFile epub;
  if (!Storage.openFileForRead("AI_SRV", work.path, epub) || epub.isDirectory())
    return false;
  const uint64_t bytes = epub.fileSize64();
  if (bytes == 0 || bytes > MAX_EPUB_BYTES) return false;

  const std::string prefix = std::string("--") + BOUNDARY +
      "\r\nContent-Disposition: form-data; name=\"sha\"\r\n\r\n" + work.sha +
      "\r\n--" + BOUNDARY +
      "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"book.epub\"\r\n"
      "Content-Type: application/epub+zip\r\n\r\n";
  const std::string suffix = std::string("\r\n--") + BOUNDARY + "--\r\n";
  const uint64_t total = prefix.size() + bytes + suffix.size();
  if (total > INT32_MAX) return false;
  auto* client = clientFor(buildRequestUrl(work.url, "/job", "", work.token), HTTP_METHOD_POST);
  if (!client) return false;
  const std::string contentType = std::string("multipart/form-data; boundary=") + BOUNDARY;
  esp_http_client_set_header(client, "Content-Type", contentType.c_str());
  const bool opened = esp_http_client_open(client, static_cast<int>(total)) == ESP_OK;
  auto writeAll = [&](const char* data, size_t length) {
    while (length) {
      if (work.cancel.load()) return false;
      const int n = esp_http_client_write(client, data, static_cast<int>(std::min<size_t>(length, 2048)));
      if (n <= 0) return false;
      data += n;
      length -= static_cast<size_t>(n);
    }
    return true;
  };
  bool ok = opened && writeAll(prefix.data(), prefix.size());
  std::array<uint8_t, 2048> buffer{};
  uint64_t sent = 0;
  while (ok && sent < bytes) {
    if (work.cancel.load()) { ok = false; break; }
    const size_t n = static_cast<size_t>(std::min<uint64_t>(buffer.size(), bytes - sent));
    if (epub.read(buffer.data(), n) != static_cast<int>(n) ||
        !writeAll(reinterpret_cast<const char*>(buffer.data()), n)) { ok = false; break; }
    sent += n;
  }
  epub.close();
  if (ok) ok = writeAll(suffix.data(), suffix.size());
  if (opened && !work.cancel.load()) {
    // A server may reject the POST before the EPUB body has been fully sent.
    // Read its response even after a short write to recognize HTTP 401.
    esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status == 401) setError(work, "unauthorized");
    if (ok) ok = status == 200 || status == 201 || status == 202;
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok && sent == bytes && !work.cancel.load();
}

bool downloadPackage(Work& work) {
  const std::string endpoint = buildRequestUrl(work.url, "/download", work.sha, work.token);
  auto* client = clientFor(endpoint, HTTP_METHOD_GET);
  if (!client) return false;
  int status = 0;
  const bool received = beginResponse(client, 0, status);
  if (received && status == 401) setError(work, "unauthorized");
  bool ok = received && status == 200;
  const int64_t length = esp_http_client_get_content_length(client);
  if (length > static_cast<int64_t>(MAX_PACKAGE_BYTES)) ok = false;
  const std::string temporary = work.path + ".aic.part";
  HalFile destination;
  if (ok) ok = Storage.openFileForWrite("AI_SRV", temporary, destination);
  uint64_t bytes = 0;
  std::array<uint8_t, 2048> buffer{};
  while (ok) {
    if (work.cancel.load()) { ok = false; break; }
    const int n = esp_http_client_read(client, reinterpret_cast<char*>(buffer.data()),
                                       static_cast<int>(buffer.size()));
    if (n < 0 || (n == 0 && !esp_http_client_is_complete_data_received(client))) {
      ok = false;
      break;
    }
    if (n == 0) break;
    if (bytes + static_cast<uint64_t>(n) > MAX_PACKAGE_BYTES ||
        destination.write(buffer.data(), static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      ok = false;
      break;
    }
    bytes += static_cast<uint64_t>(n);
  }
  if (destination) {
    destination.flush();
    destination.close();
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (length >= 0 && bytes != static_cast<uint64_t>(length)) ok = false;
  bool complete = false;
  if (ok && !work.expectedAicSha.empty()) {
    uint8_t actual[32]{};
    ok = ai_book_cache::sha256File(temporary, actual) && toHex(actual) == work.expectedAicSha;
  }
  if (ok) ok = ai_book_cache::validatePackage(temporary, work.bookSha, complete);
  if (ok && !work.cancel.load()) {
    ok = ai_book_cache::installPackage(temporary, work.path);
  } else {
    ok = false;
  }
  if (!ok) Storage.remove(temporary.c_str());
  if (ok) {
    work.bytes = bytes;
    work.changed = true;
    work.complete = complete;
    LOG_INF("AI_SRV", "downloaded bytes=%u complete=%u", static_cast<unsigned>(bytes), complete ? 1u : 0u);
  }
  return ok;
}

void runWorker(void* arg) {
  std::unique_ptr<std::shared_ptr<Work>> wrapper(static_cast<std::shared_ptr<Work>*>(arg));
  std::shared_ptr<Work> holder = std::move(*wrapper);
  wrapper.reset();
  Work& work = *holder;
  bool ok = WiFi.status() == WL_CONNECTED && !work.cancel.load();
  if (!ok) {
    setError(work, "wifi_disconnected");
  } else if (work.sha.empty()) {
    if (!ai_book_cache::sha256File(work.path, work.bookSha)) {
      ok = false;
      setError(work, "epub_sha256");
    } else {
      work.sha = toHex(work.bookSha);
    }
  }
  if (ok && work.step == Step::Status) {
    std::string serverState;
    ok = readStatus(work, serverState);
    if (ok) {
      LOG_INF("AI_SRV", "progress %u/%u", static_cast<unsigned>(work.doneCount),
              static_cast<unsigned>(work.totalCount));
      if (serverState == "missing") {
        work.next = Step::Upload;
      } else if (serverState == "ready" || (work.doneCount && work.doneCount > 0)) {
        work.next = Step::Download;
        work.finished = serverState == "ready";
      } else if (serverState == "error") {
        ok = false;
        setError(work, "server_error");
      } else {
        work.next = Step::Status;
      }
    } else {
      if (work.error.empty()) setError(work, "status_http_or_json");
    }
  } else if (ok && work.step == Step::Upload) {
    ok = uploadEpub(work);
    if (ok) {
      LOG_INF("AI_SRV", "job queued sha=%.16s", work.sha.c_str());
      work.next = Step::Status;
    } else {
      if (work.error.empty()) setError(work, "upload_failed");
    }
  } else if (ok && work.step == Step::Download) {
    ok = downloadPackage(work);
    if (ok) {
      work.next = Step::Status;
      work.finished = work.complete;
    } else {
      if (work.error.empty()) setError(work, "download_or_validation");
    }
  }
  if (work.cancel.load()) {
    ok = false;
    setError(work, "cancelled");
  }
  work.succeeded = ok;
  // The worker is the last owner of HTTP and SD handles. Never shut down WiFi
  // before TLS teardown, even if the reader was destroyed during a download.
  if (work.shutdownWifiOnExit.exchange(false)) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
  work.done.store(true, std::memory_order_release);
  holder.reset();
  ai_http_gate::release();
  vTaskDeleteWithCaps(nullptr);
}

void releaseWifi() {
  if (!state.wifiOwned) return;
  if (state.worker) return;
  if (ai_http_gate::busy()) {
    // A prefetch worker currently owns the HTTP lease. Do not remove its
    // radio until it has finished TLS teardown.
    ai_http_gate::requestWifiShutdown();
    state.wifiOwned = false;
    state.joining = false;
    return;
  }
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  state.wifiOwned = false;
  state.joining = false;
}

bool memoryReady() {
  const auto internal = HalMemory::getInternalHeap();
  const auto psram = HalMemory::getPsramHeap();
  return internal.freeBytes >= MIN_INTERNAL_FREE && internal.largestBlockBytes >= MIN_INTERNAL_BLOCK &&
         psram.freeBytes >= MIN_PSRAM_FREE;
}

void launch() {
  if (ai_http_gate::busy() || !memoryReady()) return;
  if (!ai_http_gate::acquire()) return;
  auto job = std::make_shared<Work>();
  job->path = state.bookPath;
  job->url = state.baseUrl;
  job->token = state.token;
  job->sha = state.sha;
  job->expectedAicSha = state.aicSha;
  memcpy(job->bookSha, state.bookSha, sizeof(job->bookSha));
  job->step = state.step;
  job->doneCount = state.pendingDone;
  auto* argument = new (std::nothrow) std::shared_ptr<Work>(job);
  if (!argument ||
      xTaskCreateWithCaps(runWorker, "ai-cache-server", WORKER_STACK_BYTES, argument,
                          tskIDLE_PRIORITY + 1, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    delete argument;
    ai_http_gate::release();
    LOG_INF("AI_SRV", "error: worker_create");
    state.terminal = true;
    return;
  }
  state.worker = std::move(job);
}
}  // namespace

bool validBaseUrl(const std::string& url) {
  if (url.empty()) return true;
  const size_t prefix = url.rfind("http://", 0) == 0 ? 7 : url.rfind("https://", 0) == 0 ? 8 : 0;
  if (!prefix || url.size() > 255 || url.size() <= prefix ||
      url.find_first_of("#@\r\n\t ") != std::string::npos) return false;
  const size_t query = url.find('?', prefix);
  const size_t end = query == std::string::npos ? url.size() : query;
  const size_t slash = url.find('/', prefix);
  if (url[prefix] == ':' || url[prefix] == '/' || url[prefix] == '?') return false;
  if (slash == std::string::npos || slash >= end) return true;
  const std::string path = url.substr(slash, end - slash);
  return path == "/" || path == "/api/cache" || path == "/api/cache/";
}

void cancel() {
  if (state.worker) {
    const bool finished = state.worker->done.load(std::memory_order_acquire);
    if (finished) {
      // The worker may have already passed its shutdown handoff but not yet
      // released the shared HTTP lease. Defer radio teardown to gate release.
      state.worker.reset();
      releaseWifi();
    } else {
      state.worker->cancel.store(true, std::memory_order_release);
      if (state.wifiOwned) state.worker->shutdownWifiOnExit.store(true, std::memory_order_release);
      state.worker.reset();
    }
  } else {
    releaseWifi();
  }
  state = State{};
}

void watch(const std::string& bookPath, const std::string& baseUrl, const std::string& token) {
  if (state.bookPath == bookPath && state.baseUrl == baseUrl && state.token == token && !state.terminal) return;
  cancel();
  if (!validBaseUrl(baseUrl) || baseUrl.empty()) {
    LOG_INF("AI_SRV", "disabled");
    return;
  }
  state.bookPath = bookPath;
  state.baseUrl = baseUrl;
  state.token = token;
  state.nextPollAt = millis();
}

bool active() { return !state.bookPath.empty() && !state.terminal; }
bool workerPending() { return state.worker && !state.worker->done.load(std::memory_order_acquire); }

void adoptWifi() {
  if (active()) state.wifiOwned = true;
}

void pauseForForeground() {
  if (state.worker) state.worker->cancel.store(true, std::memory_order_release);
  if (active()) {
    state.step = Step::Status;
    state.nextPollAt = millis() + POLL_INTERVAL_MS;
  }
}

bool takeUpdated(const std::string& bookPath) {
  if (state.bookPath != bookPath || !state.updated) return false;
  state.updated = false;
  return true;
}

void tick(bool otherWifiUser) {
  if (!active()) return;
  if (state.worker) {
    if (!state.worker->done.load(std::memory_order_acquire) || ai_http_gate::busy()) return;
    auto job = std::move(state.worker);
    if (!job->sha.empty()) {
      state.sha = job->sha;
      memcpy(state.bookSha, job->bookSha, sizeof(state.bookSha));
    }
    if (job->cancel.load()) {
      state.step = Step::Status;
      state.nextPollAt = millis() + POLL_INTERVAL_MS;
    } else if (!job->succeeded) {
      LOG_INF("AI_SRV", "error: %s", job->error.c_str());
      state.terminal = true;
    } else {
      if (job->changed) {
        state.updated = true;
        state.lastDone = state.pendingDone;
      }
      state.step = job->next;
      if (job->step == Step::Status) {
        state.aicSha = job->expectedAicSha;
        // Only commit the watermark after the snapshot is installed.
        if (job->next == Step::Download) {
          if (job->finished || job->doneCount > state.lastDone) {
            state.pendingDone = job->doneCount;
          } else {
            state.step = Step::Status;
          }
        }
      }
      state.nextPollAt = millis() + (state.step == Step::Status ? POLL_INTERVAL_MS : 0);
      if (job->finished && job->complete) state.terminal = true;
    }
    // Reuse the connection for an immediately following upload/download;
    // park the radio between the 15-second status polls.
    if (!otherWifiUser && (state.terminal || state.step == Step::Status)) releaseWifi();
    return;
  }
  if (state.terminal) { if (!otherWifiUser) releaseWifi(); return; }
  if (static_cast<int32_t>(millis() - state.nextPollAt) < 0) return;
  if (ai_http_gate::busy() || bleturner::holdsHeap()) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (!state.wifiOwned) {
      if (WiFi.getMode() != WIFI_MODE_NULL && WiFi.getMode() != WIFI_MODE_STA) return;
      // Reuse the BLE teardown gate before allocating a WiFi station.
      if (!bleturner::stopNow(0)) return;
      WIFI_STORE.loadFromFile();
      const auto ssid = WIFI_STORE.getLastConnectedSsid();
      const auto cred = WIFI_STORE.findCredential(ssid);
      if (!cred || cred->ssid.empty()) {
        LOG_INF("AI_SRV", "error: no_saved_wifi");
        state.terminal = true;
        return;
      }
      WiFi.persistent(false);
      WiFi.mode(WIFI_STA);
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
      state.wifiOwned = true;
      state.joining = true;
      state.joiningAt = millis();
      return;
    }
    if (state.joining && millis() - state.joiningAt < WIFI_JOIN_TIMEOUT_MS) return;
    LOG_INF("AI_SRV", "error: wifi_timeout");
    state.terminal = true;
    releaseWifi();
    return;
  }
  state.joining = false;
  launch();
}
}  // namespace ai_cache_server

#else
namespace ai_cache_server {
bool validBaseUrl(const std::string& url) { return url.empty(); }
void watch(const std::string&, const std::string&, const std::string&) {}
void tick(bool) {}
void pauseForForeground() {}
void adoptWifi() {}
bool active() { return false; }
bool workerPending() { return false; }
bool takeUpdated(const std::string&) { return false; }
void cancel() {}
}  // namespace ai_cache_server
#endif
