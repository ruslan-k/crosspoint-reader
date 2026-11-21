# AI sentence translation (BYOK)

## Indexed offline whole-book packages (Picco)

An external host can generate one indexed `.aic` package alongside each EPUB.
Translate checks this package by normalized source sentence and selected-word
occurrence **before** API configuration, existing caches, or any Wi-Fi request.
Each package is validated against the actual raw EPUB SHA-256, its sorted
fixed-width index CRC, and record CRCs. A valid COMPLETE package prevents
new on-device lookahead jobs. A missing entry falls back to the existing
on-device caches and online foreground translation.

See [the versioned on-disk format](ai-cache-format.md) for byte-level details,
path conventions, normalization, limits and host generator requirements.


This downstream CrossPoint build adds **Translate** to EPUB text selection.
The existing Lookup, Clip and Bookmark actions are unchanged. When no offline
dictionary is configured, the touch action bar shows **Clip / Bookmark /
Translate**.

The result is deliberately minimal: **word — translation**, **Original** (the
source sentence) and **Translation** (the translated sentence). There is no
part-of-speech classification, related-word list or vocabulary write.

## Configure

Open the device's **Web Settings** page from a trusted local network, then
scroll below **OPDS Servers** to **AI Translation**. Enter an HTTPS API URL,
an API key and the provider's model ID, then save. A blank key field preserves
the existing key; **Remove saved API key** clears it.

| Provider | API URL | Model example |
| --- | --- | --- |
| Gemini | `https://generativelanguage.googleapis.com/v1beta` | `gemini-2.5-flash` |
| Groq | `https://api.groq.com/openai/v1` | `openai/gpt-oss-20b` |
| OpenCode Go | `https://opencode.ai/inference/go/openai/v1` | `deepseek-v4.1-flash` |

Gemini calls `models/{model}:generateContent` with `x-goog-api-key`.
Other endpoints use OpenAI-compatible `/chat/completions` with a Bearer key.
The legacy OpenCode Go URL `/zen/go/v1` is normalized **in memory**
to `/inference/go/openai/v1`, without altering the saved key. For OpenCode Go,
only `model` and `messages` are sent, matching the official API example.
OpenCode Go requires the `x-opencode-session` header. The reader now generates
an independent 128-bit random session identifier for each one-request
translation (one sentence = one conversation), using it only with Go; the ID
is not logged or stored. The officially listed Go models include
`hy4-flash-free` (check your subscription/account and availability).
OpenCode Go is primarily intended for coding-agent traffic; provider-side
access policies may still restrict translation requests from other clients.
An already-complete `/chat/completions` URL is also accepted.

## Privacy and security

- Foreground lookup submits the selected word and surrounding sentence on demand.
  **On Picco, while the Translate popup is open**, background lookahead may also
  submit up to 100 following sentences with their numbered source words,
  in HTTPS requests of at most 4 sentences. This consumes additional API tokens
  and may incur provider charges. The lookahead does not transmit the entire EPUB.
- API keys are not compiled into firmware and never appear in the GET API
  response. POST `/api/ai-translation` supports setting and clearing the key.
- Successful word+sentence translations are saved in the local exact-match cache
  `/.crosspoint/ai-cache/<book-provider-model-hash>/<word-sentence-hash>.bin`.
  The 64-bit FNV-1a filename hash is for lookup, not encryption. The record
  validates the source word/sentence, scope, lengths and checksum before reuse.
  Matching entries open without starting Wi-Fi or calling the API.
  Cache files contain **plaintext book passages and translations** on the SD card.
  The web file manager and plugins cannot open this protected directory, but
  physical access to the SD card still exposes the contents. API keys are not
  written into the cache.
- Picco lookahead records are stored separately in
  `/.crosspoint/ai-prefetch/<book-provider-model-hash>/<sentence-position-hash>.bin`.
  Their sentence, translated sentence, per-word translations and source offsets
  are **plaintext** on the SD card. A checksum validates every record. A tiny
  `checkpoint.bin` is written atomically after completed batches; it is only
  an advisory progress hint. Missing/corrupt entries are individually checked
  before requesting again. The web file manager and plugins block this directory.
- The key is MAC-XOR obfuscated on SD as in CrossPoint's existing OPDS
  credentials. **This is not cryptographic encryption.** The backing file
  `/.crosspoint/ai-translation.json` is protected from web file operations.
- The web interface itself is served over local HTTP; only configure on
  trusted Wi-Fi, and do not expose the device's web server to the internet.
- Outbound **AI requests use ESP-IDF's `esp_http_client` with mbedTLS**, with
  root/intermediate PEM verification **and hostname matching enabled**. This
  replaces FreeInk wolfSSL only for BYOK translation after repeated hardware
  `ASN_NO_SIGNER_E (-188)` failures against `opencode.ai`. Other firmware
  networking keeps its existing wolfSSL implementation.
- All HTTP redirects are disabled: the API key is never forwarded to a
  different origin. Foreground response buffering is capped at 48 KiB in PSRAM
  (12 KiB fallback); Picco prefetch uses a PSRAM-only 48 KiB cap and a bounded
  24 KiB request. Only sanitized diagnostics (such as status and counts) are logged,
  never the key, book text, translated words or provider response.
- The same trust set (GTS R1/R2/R3/R4, ISRG Root X1, DigiCert Global Root G2,
  and verified WE1 intermediate) is used. Unknown chains fail closed.
- Wi-Fi starts on demand. The translation activity first tries the last
  successfully connected saved network, then asynchronously scans for other
  saved networks. During automatic reconnect, a compact status popup overlays
  the existing screen instead of opening the full Wi-Fi UI. The standard
  connection/selection screen appears only after saved-network attempts fail,
  or immediately when the user presses Confirm. Back cancels the selection.
- During the AI request, a compact `Translating...` popup is displayed. On success
  a **dictionary-style overlay** opens over the retained EPUB page: compact
  10pt Ubuntu text with Cyrillic support (Noto Sans 12/14 fallback),
  word + translation, Original, Translation. Swipe up/down (or left/right),
  tap the upper/lower body, or use page buttons to scroll. The Close label,
  progress numbers, and Swipe to scroll footer are removed. Tap **outside**
  the popup or press Back to dismiss it without changing the reader position.
  On memory-constrained boards without spare framebuffer storage, the overlay
  falls back to a white background.
- Wi-Fi is switched off when leaving the translation if it was started by
  this activity. Pre-existing Wi-Fi sessions are left running.
- If the serial console shows wolfSSL error `-188` (`ASN_NO_SIGNER_E`),
  the AI server's signing CA is not in the embedded trust store or its
  certificate chain is incomplete. Do not disable TLS verification. A
  different custom provider may require adding its documented public CA.
- Some OpenCode Console free-tier models are restricted to the OpenCode
  application rather than third-party clients. For BYOK on a reader, use a
  provider/model combination that permits external API usage.
- Translation requires active Wi-Fi and a valid system clock. API quotas
  and billing are controlled by the respective provider.

## Long model responses

Picco log `serial-log (1).txt` shows `ESP_ERR_HTTP_EAGAIN` at the original
30-second timeout, and separate `HTTP 200 response_bytes=7859` failures
caused by an 8 KiB response-body limit being exceeded on the next chunk.
The client now waits up to 90 seconds and stores a bounded 48 KiB response
in PSRAM where available (12 KiB internal-heap fallback). Oversized replies
fail explicitly as `AI response too long` rather than misleading `HTTP 200`.
The translation prompt requests compact JSON with `word_translation` and
`sentence_translation`; the result parser accepts this JSON, the previous
`WORD:`/`SENTENCE:` lines, and common OpenAI-style `content` array/`text`
reply shapes, ignoring large unused reasoning metadata. Repeated provider
latency or a noncompliant model can still cause an error; no successful
long-sentence hardware test is claimed here.


## Token-limit recovery (OpenCode Go / reasoning models)

A Picco serial log from November 21, 2026 showed an HTTP **200** from
`opencode.ai` with `finish=length`, response body of 10,960 bytes and
`text_bytes=0`. This is not a TLS or connectivity error: the provider used
its allowed completion budget before producing a usable translation. The old
firmware used `max_tokens=1024` for all OpenAI-compatible models.

Foreground translation now starts with **2,048 output tokens** (reasoning tokens
may be included by the provider) and retries **once** with a cap of **4,096**
only when the provider reports `finish_reason=length` or Gemini reports
`finishReason=MAX_TOKENS` and no valid translation is present. No retry storm
is possible; no failed or partial result is cached. If the second attempt also
hits the limit, the screen reports `AI output truncated (token limit)` instead
of claiming that the model returned a malformed translation. The serial log
includes token counts when provided but never logs the model response itself.

**Costs:** The retry makes one additional paid API call. This does not guarantee
that every thinking model will comply; prefer a faster non-reasoning translator
if it repeatedly exhausts its output cap.

## Limitations

The sentence extractor uses EPUB word positions, paragraph starts, punctuation,
and a small abbreviation set. It can span rendered lines and forward page
boundaries already in the selection window. It does **not** recover the part
of a sentence on a preceding page, and does not handle every language's
sentence-boundary conventions. Verify boundary cases on the device.

The popup is now drawn directly by `AiSentenceTranslationActivity`; it does
not use `DictionaryDefinitionActivity`. Its word wrapping and scrolling are
bounded, and the chosen Ubuntu 10pt font includes Cyrillic glyphs.

### Priority Streaming Cache v2 (Picco, 2026-11-21)

The previous all-or-nothing four-sentence prefetch repeatedly logged
\`[AI_PRE] Batch skipped: bad endpoint, clock or low RAM\` even when
foreground translation succeeded. It duplicated JSON prompts in internal RAM
and refused a valid sentence if *any* contextual word was missing.

**Picco transport fix (serial-log 7, 2026-11-21).**
The ESP32-S3 async HTTP client was configured with a 100-ms read-idle
timeout. This is a socket timeout, not a harmless per-tick budget: a
pause between SSE chunks can make esp_http_client_perform return
ESP_ERR_HTTP_INCOMPLETE_DATA (0x700c / 28684), exactly as shown in the
device trace, with Stream stored 0/4 on every attempt.
The socket read-idle timeout is now 30 seconds, while the overall batch
lifetime remains 60 seconds. A longer socket timeout may delay Translate
UI input while a transport call is waiting, but never affects page turns.
If a 200-status SSE request still ends incomplete without any cached
items, later packets for that endpoint use ordinary non-streaming JSON
for the rest of this boot, without immediate paid retries.
New diagnostics report mode=sse/json and frame/part counts, never book text.

The stream is now parsed from each HTTP_EVENT_ON_DATA callback, not only
when esp_http_client_perform returns. This keeps the transport buffer
bounded to 48 KiB even for large multi-frame streams. ArduinoJson's
parsing allocator uses PSRAM so low internal heap during TLS cannot
discard complete JSON objects. The final log includes byte count and
HTTP_EVENT_ON_DATA count to distinguish empty streams from invalid frames.


The new implementation uses:
- **100-sentence lookahead**, sorted by source offset, with the **nearest four
  missing/partial sentences** requested first.
- **SSE streaming** for OpenAI-compatible \`chat/completions\` (\`stream=true\`)
  and Gemini \`streamGenerateContent?alt=sse\`, parsed incrementally from
  bounded PSRAM buffers. Gateways ignoring streaming can still return a normal
  complete JSON response, which is parsed without issuing another request.
- A compact per-sentence JSON object as soon as the model emits it. Each
  validated sentence and/or individual translated word is **atomically merged**
  with its prior cache record. The preexisting \`AFP1\` disk format remains
  readable, including previous complete entries. Incomplete word mappings do
  **not** erase translated sentences or previously completed words.
- Only **untranslated words** and a \`need_sentence\` flag enter subsequent
  requests. A corrupt or missing record is scanned again; the checkpoint is
  advisory and never suppresses a missing record.
- A fixed-capacity JSON writer builds input directly in PSRAM, with no large
  temporary request documents/strings on the internal heap. HTTPS now runs
  in one low-priority task using a 12-KiB PSRAM stack rather than blocking
  Translate's UI loop. A global guard prevents overlapping network requests
  even if Translate closes and reopens. On close, network cancellation is
  signalled immediately; the detached task may need up to its bounded socket
  timeout to notice cancellation. No new work is scheduled after closing.
- If internal RAM is too low to persist a streamed record while TLS is alive,
  keep the finished JSON record temporarily in PSRAM and write it after
  disconnecting TLS, including when Translate closes. Incomplete stream
  fragments are discarded and never cached.

**Not a guarantee:** a model must emit a complete JSON object before Translate
closes for that entry to become durable. Some gateways ignore SSE or stop
before the first object; they cannot supply an offline result in that case.
Watch \`[AI_PRE] Stream item saved\`, \`Deferred stream item saved\`, and
\`Stream stored N/4\`. All source and translation text remains plaintext on
the microSD (under the protected \`/.crosspoint/ai-prefetch/\` tree).


### Picco lookahead prefetch (100 / 4 / Translate-only)

Implementation settings are fixed for Picco:

- **Window:** up to 100 following sentences (within already-laid-out pages
  of the active EPUB spine). The scan reads only one cached page at a time,
  never triggering EPUB parsing or pagination; if the section cache ends,
  lookahead stops there until the user later opens Translate again.
- **Batch:** at most 4 sentences per HTTPS request (last or oversized
  batches may be smaller). The provider emits per-sentence JSON records over
  SSE, with contextual word translations identified by absolute source offsets.
  Every valid sentence or individual word is atomically merged on arrival.
  Previously cached sentence translations are preserved during word-only fills.
  Repeated words at different offsets are not confused.
- **Lifetime:** only while Translate is **Showing**, Wi-Fi is connected,
  and the clock is trusted. Offline cache hits never require a radio.
  A **cooperative asynchronous HTTPS state machine** uses Translate's existing
  UI task, making at most one 100-ms transport step per tick. It does **not**
  create another FreeRTOS stack or execute during page turns.
  Closing the modal destroys the HTTPS client synchronously.
- **Memory:** Picco-only. The original 120-KiB internal RAM requirement
  prevented every batch in measured tests (~101-105 KiB available after a
  foreground translation). The new cooperative path requires **94 KiB free
  internal**, **36 KiB largest internal block**, 144 KiB PSRAM and 64 KiB
  largest PSRAM block **before** starting TLS. Abort the current request if
  internal free memory drops below 15 KiB; disabled after failure, no storm.
  It copies only a bounded number of short tokens from each EPUB page and
  destroys the Page object before HTTPS. Response is capped at 48 KiB in
  PSRAM, outgoing JSON at 24 KiB; overall timeout 60 seconds.
- **Crash recovery:** each validated sentence is written to an atomic,
  checksummed binary record. The progress checkpoint is a hint, **never**
  proof that all earlier sentences were translated. The scanner verifies
  individual records and resends missing ones upon the next Translate open.
- **Fallbacks:** a model may not return complete JSON or every numbered
  word. Each completed response item is saved even when other words are
  missing. Incomplete JSON fragments are discarded; fully parsed items can
  survive connection loss. A selected word without a valid cached mapping
  still uses normal foreground translation. A failed network batch disables
  retries until the next Translate popup, avoiding provider rate-limit storms.

The current implementation does **not** prefetch beyond the current spine
or unseen/unbuilt pages, and it has not yet been verified against real AI
models, battery performance, or memory pressure on physical Picco hardware.
100 is a lookahead limit, not a guarantee that 100 replies will be ready
before the user closes the popup. In particular, a single batch can take
many seconds on reasoning models. **Closing Translate cancels the in-flight
request**; there is deliberately no network request after the modal closes.
Therefore immediately opening Translate for the next sentence may still go
online unless the preceding batch was fully processed and saved. The next
popup rechecks individual records and can fill gaps.

After flashing the new build, validate with serial logs:
- `[AI_CACHE] Foreground translation cache save: ok` means the selected
  word+sentence pair was saved; selecting the exact pair again should produce
  `[AI_CACHE] Exact translation cache hit` without Wi-Fi.
- `[AI_PRE] Batch started: count=4` confirms lookahead started.
  `[AI_PRE] Batch stored 4/4 sentences` confirms a full packet is durable.
  A later selection should produce `[AI_CACHE] Prefetched translation cache hit`.
- `[AI_PRE] Waiting for RAM` shows a real guard block and reports the
  internal free/contiguous counts. A failed or partial batch is never
  checkpointed as fully complete; only validated records are reused.

## ESP-IDF HTTPS transport A/B

The device logs through `serial-log (8).txt` on the mbedTLS firmware
`7fa72c3` confirm successful TLS and an HTTP 400 (206-byte provider error).
The subsequent `serial-log.txt` from firmware `22c1d32` identifies
`MissingSessionID` for `hy4-flash-free`: Go refuses a request without
`x-opencode-session`. This revision supplies a generated ID only for Go;
the earlier TLS and URL migration fixes remain intact.
The previous implementation hid the provider error body. The next revision
logs only status, model and machine-readable error type/code; on the local
E-Ink screen it shows a bounded, control-character-filtered provider error
message (with reflected API keys and complete sentences hidden). This
helps distinguish obsolete endpoints and unknown model IDs without leaking
book text or credentials into serial logs.

The device log `serial-log (7).txt` on firmware `25852e3` confirms a
successful saved Wi-Fi connection and a successful wolfSSL trust-store load
(`CA load result=1`), followed by the same `-188` on TLS 1.3 and TLS 1.2.
Adding the authentic WE1 intermediate did **not** repair the chain validation.
The next A/B therefore changes the **TLS implementation**, not the CA list.
ESP-IDF's `esp_http_client` uses mbedTLS and validates the configured PEM
chain and HTTPS hostname. If it fails, the serial log now records the IDF
result, mbedTLS error code and verification flags, without credentials.

A successful firmware build does not establish a successful HTTPS translation
on the device. Test both the provider's response and Wi-Fi cleanup on Picco.

## Diagnostics on Picco

After verifying the configured endpoint `opencode.ai` in the device log,
certificate transparency records show its 2026 leaf is signed by **Google Trust
Services WE1**, and Google's CP/CPS identifies WE1 as issued by **GTS Root R4**.
The six previously embedded roots include R4, yet wolfSSL 5.7.2 still returned
`-188` with a successfully loaded trust store. This revision explicitly
includes the authentic WE1 intermediate (SHA-256
`1dfc1605fbad358d8bc844f76d15203fac9ca5c1a79fd4857ffaf2864fbebf96`)
as a supplemental CA, without disabling chain or hostname verification.
This is a **targeted compatibility experiment**, not a claim of successful
device TLS. If it still fails, inspect the actual server-presented chain and
wolfSSL's certificate verification callback before making further CA changes.


On the Picco build from `53055c8`, successful Wi-Fi connection followed by
`wolfSSL_connect failed: -188` does **not** establish which trust anchor is
missing. The previous GTS-root expansion did not resolve the user's failure.
This build logs only the HTTPS **host** (not the path, key or book text) and
the wolfSSL `CA load` result. Interpret the next serial log as follows:

- `CA load: result=1`: the compiled wolfSSL accepted the bundled CA store.
  A later `-188` points to a server chain not verifiable from those roots
  (or a missing intermediate); inspect that **specific** server hostname.
- `CA load: result!=1`: the wolfSSL build could not load the trust store.
  Investigate PEM parsing, cryptography configuration and memory rather than
  adding more roots.
- `wolfSSL_connect failed: -188`: TLS failed **before** any AI HTTP request
  or API-key validation, independently of the model.

A one-time Picco crash after saved Wi-Fi rejoin was recorded as
`assert failed: sys_untimeout (... Required to lock TCPIP core ...)`.
`WifiSelectionActivity` starts SNTP after joining; translation's fallback
`trustedtime::syncNow()` previously restarted an already active SNTP using
`configTzTime()`, potentially reaching `sntp_stop` off the lwIP core.
`syncNow()` now reuses an already-started SNTP session. This addresses a
concrete crash path but cannot prove the crash's PC without a symbolized
backtrace from the exact firmware.

Do not enable `setInsecure()` or record real API keys in diagnostic logs.

## Verification

The repository host test can run with:

```sh
c++ -std=c++20 test/ai_sentence/AiSentenceUtilsTest.cpp -o /tmp/cp-ai-sentence-test
/tmp/cp-ai-sentence-test
```

Build the Picco firmware environment with `pio run -e picco` (the custom
GitHub Actions workflow also targets Picco only). Check Wi-Fi connection, HTTPS certificate
verification, low-memory error handling, provider-specific responses, and
returning to the exact reader page after closing the translation.

No API keys or book excerpts are committed to the repository.

### Picco prefetch worker after serial-log 8 (2026-11-21)

The previous async HTTP client blocked the Translate UI for 12–32 seconds
per esp_http_client_perform call. A single bounded FreeRTOS worker now runs
TLS with a PSRAM-allocated stack via xTaskCreateWithCaps. The UI loop only
checks an atomic completion flag; no TLS or SD calls run in the page-turn path.
Network idle timeout is now 6 seconds, and overall request lifetime is at
most 60 seconds. The worker's shared job is safe after the popup is destroyed,
and is cancelled when Translate closes. New log counts completed JSON objects,
recognized model item IDs, and rejected objects to distinguish a malformed
model response from a network failure. Output budget is 2048 tokens rather
than 6144 to reduce excessive streamed data. Only an on-device test can
confirm the currently selected model produces valid NDJSON before exit.
