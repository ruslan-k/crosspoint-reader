#!/usr/bin/env python3
"""Source guards for Picco's optional query-token cache-server integration."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
read = lambda path: (root / path).read_text(encoding="utf-8")
store = read("src/AITranslationStore.cpp")
web = read("src/network/CrossPointWebServer.cpp")
html = read("src/network/html/SettingsPage.html")
client = read("src/util/AiCacheServer.cpp")
reader = read("src/activities/reader/EpubReaderActivity.cpp")
header = read("src/util/AiCacheServerUrl.h")

assert 'doc["ai_cache_server_token_obf"] = obfuscation::obfuscateToBase64(cacheServerToken_);' in store
assert 'doc["ai_cache_server_token_obf"] | ""' in store
get_handler = web.split('void CrossPointWebServer::handleGetAiTranslation() const {')[1].split(
    'void CrossPointWebServer::handlePostAiTranslation()')[0]
assert 'doc["hasCacheServerToken"]' in get_handler
assert 'getCacheServerToken()' not in get_handler
assert 'doc["ai_cache_server_token"]' not in get_handler
assert 'id="ai-cache-server-token"' in html and 'type="password" id="ai-cache-server-token"' in html
assert 'data.ai_cache_server_token = cacheToken' in html
assert 'data.clearCacheServerToken = true' in html
assert 'const bool replaceCacheToken = clearCacheToken || doc["ai_cache_server_token"].is<const char*>();' in web
assert 'getCacheServerToken()' in reader
for method in ('"/status"', '"/job"', '"/download"'):
    assert method in client
assert client.count('buildRequestUrl(work.url,') == 3
assert client.count('setError(work, "unauthorized")') == 3
for generic_error in ('status_http_or_json', 'upload_failed', 'download_or_validation'):
    assert f'if (work.error.empty()) setError(work, "{generic_error}");' in client
assert 'LOG_INF("AI_SRV", "error: %s", job->error.c_str())' in client
assert 'LOG_INF("AI_SRV", "error: unauthorized")' not in client  # logged through sanitized reason
assert 'result += "token=";' in header
print("AI Cache Server token source contracts passed")
