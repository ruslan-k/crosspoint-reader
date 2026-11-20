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
| OpenCode Go | `https://opencode.ai/zen/go/v1` | `deepseek-v4.1-flash` |

Gemini calls `models/{model}:generateContent` with `x-goog-api-key`.
Other endpoints use OpenAI-compatible `/chat/completions` with a Bearer key.
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
- Outbound requests use wolfSSL TLS chain and hostname verification with a
  small built-in CA set (GTS R1/R2/R3/R4, ISRG Root X1, DigiCert Global Root G2).
  Unrecognised CA chains fail closed; no `setInsecure()` fallback.
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
