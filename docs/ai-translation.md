# AI sentence translation (BYOK)

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
Select a Go model actually listed in the account; IDs such as
`hy4-flash-free` are not in the currently published Go catalogue.
An already-complete `/chat/completions` URL is also accepted.

## Privacy and security

- Only the word and surrounding sentence are submitted on demand.
- API keys are not compiled into firmware and never appear in the GET API
  response. POST `/api/ai-translation` supports setting and clearing the key.
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
  different origin. Response buffering is capped at 8 KiB, and only the
  API hostname, status and TLS error codes are logged, never the key or text.
- The same trust set (GTS R1/R2/R3/R4, ISRG Root X1, DigiCert Global Root G2,
  and verified WE1 intermediate) is used. Unknown chains fail closed.
- Wi-Fi starts on demand. The translation activity first tries the last
  successfully connected saved network, then asynchronously scans for other
  saved networks. During automatic reconnect, a compact status popup overlays
  the existing screen instead of opening the full Wi-Fi UI. The standard
  connection/selection screen appears only after saved-network attempts fail,
  or immediately when the user presses Confirm. Back cancels the selection.
- During the AI request, a compact `Translating...` popup is displayed; the
  translated word and Original/Translation page opens on success.
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

## Limitations

The sentence extractor uses EPUB word positions, paragraph starts, punctuation,
and a small abbreviation set. It can span rendered lines and forward page
boundaries already in the selection window. It does **not** recover the part
of a sentence on a preceding page, and does not handle every language's
sentence-boundary conventions. Verify boundary cases on the device.

The result uses the existing plain-text DictionaryDefinitionActivity, so its
word wrapping and page navigation are shared with dictionary display. Cyrillic
output requires an available font containing Cyrillic glyphs.

## ESP-IDF HTTPS transport A/B

The device logs through `serial-log (8).txt` on the mbedTLS firmware
`7fa72c3` confirm successful TLS and an HTTP 400 (206-byte provider error).
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

Build the relevant firmware environment with `pio run -e default` (and other
board profiles if shipped). Check Wi-Fi connection, HTTPS certificate
verification, low-memory error handling, provider-specific responses, and
returning to the exact reader page after closing the translation.

No API keys or book excerpts are committed to the repository.
