# AI sentence translation (BYOK and book cache)

Select **Translate** on any word in a sentence. The popup displays the
**Original** sentence and its **Translation** into Russian. It no longer
offers a separate word translation. Selecting another word in the same
sentence uses the same cached result.

## Cache lookup order

1. Picco: indexed CPAICPK1 .epub.aic beside the EPUB. Lookup matches the
   normalized source sentence; word mappings are not required.
2. Exact BYOK cache at /.crosspoint/ai-cache/, keyed by book path,
   provider URL, model and source sentence, **not by selected word**.
3. Picco: position-addressed lookahead cache at /.crosspoint/ai-prefetch/,
   when the reader knows the sentence offset.
4. Foreground translation using the user's configured HTTPS provider.

The exact cache now writes CAI2 records in namespace ru/v2.
Old CAI1 word-specific records are retained on SD but not reused.
Existing AFP1 prefetch records containing valid sentence translations remain
readable, including those without word translations.

## Foreground requests

The prompt sends only the complete source sentence and asks for a faithful
Russian translation without extra text. The response parser accepts plain
text or a compact translation JSON object; historical sentence_translation
JSON and WORD:/SENTENCE: replies still decode for compatibility. Output
limits, provider settings, TLS validation and the bounded retry remain.
Only a valid, nonempty sentence translation is cached.

## Picco prefetch

The lookahead remains limited to 100 subsequent sentences, with up to four
sentences per request. The model returns one NDJSON object per sentence:

```json
{"id":0,"translation":"..."}
```

Source words and contextual word translations are no longer sent/requested.
Previously translated sentences are skipped even when word mappings are
absent. PSRAM, memory reserves, Wi-Fi/BLE lifetime, cancellation and
checkpoint guards remain in force.

## CrossPoint AI Cache Server

The optional server can translate an entire EPUB and publish .aic snapshots
while the user reads. The CPAICPK1 binary format remains unchanged:
the host should produce word_count=0 records and mark COMPLETE only when
**all extracted sentences** have been translated. Existing packages
containing word entries remain usable. See [ai-cache-format.md](ai-cache-format.md).

Sentence boundaries must still exactly match the reader's extraction,
including punctuation and its byte-only whitespace normalization.
