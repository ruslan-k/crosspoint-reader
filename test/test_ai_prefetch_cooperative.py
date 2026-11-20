#!/usr/bin/env python3
"""Source contract for Picco's popup-owned HTTPS prefetch worker.

Hardware networking and power-loss behavior still require real Picco tests.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
prefetch = (root / "src/activities/reader/AiSentencePrefetchSession.cpp").read_text()
header = (root / "src/activities/reader/AiSentencePrefetchSession.h").read_text()
activity = (root / "src/activities/reader/AiSentenceTranslationActivity.cpp").read_text()
cache = (root / "src/util/AiPrefetchCache.cpp").read_text()

assert "MIN_INTERNAL_FREE = 94 * 1024" in prefetch
assert "MIN_INTERNAL_BLOCK = 36 * 1024" in prefetch
assert "MIN_INTERNAL_IN_FLIGHT = 15 * 1024" in prefetch
assert "HTTP_READ_IDLE_TIMEOUT_MS = 6000" in prefetch
assert "cfg.timeout_ms = HTTP_READ_IDLE_TIMEOUT_MS;" in prefetch
assert "HTTP_STEP_TIMEOUT_MS = 100" not in prefetch
assert "ESP_ERR_HTTP_INCOMPLETE_DATA" in prefetch
assert "gNonStreamingEndpoint = job.scope.url;" in prefetch
assert "onDataReady" in prefetch
assert "result->onDataReady(result->onDataContext)" in prefetch
assert "JsonDocument chunk(&gPsramJsonAllocator)" in prefetch
assert "JsonDocument parsed(&gPsramJsonAllocator)" in prefetch
assert "job->streaming = job->scope.url != gNonStreamingEndpoint" in prefetch
assert "job->streaming ? \"text/event-stream\" : \"application/json\"" in prefetch
assert "HTTP_TIMEOUT_MS = 60000" in prefetch
assert "cfg.is_async = true;" in prefetch
assert "esp_http_client_perform(job.client)" in prefetch
assert "pollBatch();" in prefetch
assert "esp_http_client_cleanup(job.client)" in prefetch
assert "auto page = section_->loadPage(nextPage_)" in prefetch
assert "token.text = word;" in prefetch
assert "reportedMemoryPause_" in header
assert "std::shared_ptr<Job> worker_" in header
assert "xTaskCreateWithCaps(" in prefetch
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in prefetch
assert "vTaskDeleteWithCaps(nullptr)" in prefetch
assert "std::atomic<bool> gPrefetchTaskBusy" in prefetch
assert "worker_->cancel.store(true, std::memory_order_release)" in prefetch
assert "void AiSentencePrefetchSession::runBatch(void* data)" in prefetch
assert "std::atomic<bool> done{false}" in prefetch
for banned in ("gPrefetchHttpBusy", "page_ = section_->loadPage"):
    assert banned not in prefetch, banned
assert "ai_prefetch::WINDOW_SENTENCES" in prefetch
assert "ai_prefetch::BATCH_SENTENCES" in prefetch
assert "saveCheckpoint(job.scope, job.checkpoint)" in prefetch
assert "ai_prefetch::pending(scope_, current_, delta)" in prefetch
assert "if (prefetch_) prefetch_->tick();" in activity
assert "prefetch_->cancel();" in activity
assert "Foreground translation cache save: %s" in activity
assert "Exact translation cache hit" in activity
assert "Prefetched translation cache hit" in activity
assert "Storage.exists(path.c_str())" in cache
assert "StreamFramer" in prefetch
assert "saveItem(item, job.scope, job.batch, job.stored)" in prefetch
assert "completedObjects" in prefetch and "recognizedItems" in prefetch
assert "flushDeferred(job);" in prefetch
assert "request[\"stream\"]" not in prefetch  # fixed-buffer PSRAM writer, not ArduinoJson docs
assert "text/event-stream" in prefetch and "max_tokens" in prefetch
assert "BoundedJsonWriter" in prefetch
assert "HalMemory::allocatePsram(MAX_BATCH_BODY)" in prefetch
print("Picco PSRAM worker prefetch source guards passed")
