#!/usr/bin/env python3
"""Prevent accidental regressions in the BYOK transport's security invariants.

Device behavior (TLS, DNS, HTTP) still requires a Picco hardware test.
"""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] /
          "src/activities/reader/AiSentenceTranslationActivity.cpp").read_text(encoding="utf-8")
required = {
    "#include <esp_http_client.h>",
    "httpConfig.cert_pem = ai_translation::ROOTS;",
    "httpConfig.skip_cert_common_name_check = false;",
    "httpConfig.disable_auto_redirect = true;",
    "esp_http_client_perform(client)",
    "AI_HTTP_TIMEOUT_MS = 90000",
    "MAX_RESPONSE_BYTES = 48 * 1024",
    "MALLOC_CAP_SPIRAM",
    "DeserializationOption::Filter(filter)",
    "extractTranslationText(output, wordTranslation, sentenceTranslation)",
    "ai_completion::maxOutputTokens(generationAttempt_)",
    "ai_completion::shouldRetry(finishReason, generationAttempt_)",
    "retryPending_ = true;",
    "AI output truncated (token limit)",
    "ai_endpoint::isOpenCodeGoUrl(url)",
    'esp_http_client_set_header(client, "x-opencode-session", goSession)',
    "static_cast<unsigned>(esp_random())",
    "ai_endpoint::migrateOpenCodeGoUrl(",
    "safeErrorId(providerType)",
    "safeScreenError(providerMessage, config.getKey(), sentence_)",
    "esp_http_client_get_and_clear_last_tls_error(",
    "response->capacity - response->used",
    'esp_http_client_set_header(client, "Authorization"',
    'esp_http_client_set_header(client, "x-goog-api-key"',
}
for pattern in required:
    assert pattern in source, f"Missing HTTPS security invariant: {pattern}"
for forbidden in ["setInsecure(", "WOLFSSL_VERIFY_NONE", "freeink::SecureHttpClient http;"]:
    assert forbidden not in source, f"BYOK transport must not use: {forbidden}"
print("AI HTTPS transport security guards passed")
