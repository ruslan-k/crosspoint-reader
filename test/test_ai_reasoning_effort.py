#!/usr/bin/env python3
"""Source contract: the AI reasoning-effort selector persists and reaches both request paths.

On-device/model-specific acceptance still needs a Picco test; this test
ensures the provider-default/secure transport paths cannot regress silently.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
read = lambda path: (root / path).read_text(encoding="utf-8")

store_h = read("src/AITranslationStore.h")
store_c = read("src/AITranslationStore.cpp")
server = read("src/network/CrossPointWebServer.cpp")
settings = read("src/network/html/SettingsPage.html")
foreground = read("src/activities/reader/AiSentenceTranslationActivity.cpp")
prefetch = read("src/activities/reader/AiSentencePrefetchSession.cpp")

# Four well-defined options; default preserves existing firmware/provider behavior.
for value in ("default", "low", "medium", "high"):
    assert f'effort == "{value}"' in store_h
    assert f"['{value}', " in settings
assert 'std::string reasoningEffort_ = "default";' in store_h
assert 'doc["reasoningEffort"] = reasoningEffort_;' in store_c
assert 'doc["reasoningEffort"] | "default"' in store_c
assert "validReasoningEffort(effort) ? effort : \"default\"" in store_c

# Existing clients that omit the new field must not reset the saved selection.
assert 'doc["reasoningEffort"] = AI_TRANSLATION_STORE.getReasoningEffort();' in server
assert 'AITranslationStore::validReasoningEffort(reasoningEffort)' in server
assert 'AI_TRANSLATION_STORE.getReasoningEffort();' in server
assert 'configure(url, model, key, replaceKey, reasoningEffort, cacheServerUrl)' in server

# Web settings display the saved value and send it on explicit Save.
assert 'id="ai-reasoning-effort"' in settings
assert 'selectedEffort' in settings
assert "reasoningEffort: document.getElementById('ai-reasoning-effort').value" in settings
assert "enableItemSave(this.closest(" in settings

# Only OpenAI-compatible chat-completions get the extra top-level field.
# Gemini remains unchanged; default sends nothing for non-Hy4 requests.
assert 'if (strcmp(reasoningEffort, "default") != 0) req["reasoning_effort"] = reasoningEffort;' in foreground
assert 'if (reasoningEffort != "default" &&' in prefetch
assert 'request.append(",\\\"reasoning_effort\\\":")' in prefetch
assert 'req["generationConfig"].to<JsonObject>()' in foreground
assert 'generationConfig' in prefetch
assert 'httpConfig.cert_pem = ai_translation::ROOTS;' in foreground
assert 'cfg.cert_pem = ai_translation::ROOTS;' in prefetch
assert 'esp_http_client_perform(job.client)' in prefetch


# OpenCode Go hy4-flash-free uses none and 4096 in both paths, without
# changing the persisted choice or the behavior of other endpoints.
assert 'job->scope.model == "hy4-flash-free" && ai_endpoint::isOpenCodeGoUrl(job->url)' in prefetch
assert 'fastHy4 ? std::string_view("none")' in prefetch
assert 'fastHy4 ? 4096u : 2048u' in prefetch
assert 'request.integer(maxTokens)' in prefetch
assert 'effort=%.*s budget=%u' in prefetch
assert 'job->gemini ? "ignored-gemini"' in prefetch
assert 'const bool fastHy4 = config.getModel() == "hy4-flash-free" && ai_endpoint::isOpenCodeGoUrl(url);' in foreground
assert 'const char* reasoningEffort = fastHy4 ? "none" : config.getReasoningEffort().c_str();' in foreground
assert 'req["max_tokens"] = fastHy4 ? 4096u : ai_completion::maxOutputTokens(generationAttempt_);' in foreground
assert 'LOG_INF("AI", "Reasoning effort: %s", gemini ? "ignored-gemini" : reasoningEffort);' in foreground

print("AI reasoning-effort selector/persistence/request contracts passed")
