# CrossPoint indexed AI book cache (.aic) — CPAICPK1, version 1

This is the contract between the external Python generator and the Picco firmware.
The generator is intentionally not implemented here. Files are **plaintext** and
contain portions of the book and AI translations. Keep your microSD private.

## Install and lifecycle

Put **one** complete package alongside the EPUB, as
`/Books/Tender Is the Flesh.epub.aic` for
`/Books/Tender Is the Flesh.epub` (append `.aic` to the **exact device EPUB
path**, including its extension). The firmware only opens it when a Translate
popup starts; it does not open the SD card during page turns. Write to a
temporary filename and rename to `.aic` only after generator-side verification.

The package is independent of BYOK URL/model credentials; Hermes uses its
configured default model and the result is reusable offline even without an
on-device AI key. The firmware checks exact-translation cache and then the
existing position-based prefetch cache only when the package lacks the lookup.
Foreground online translation remains the last fallback, subject to existing
configuration and Wi-Fi rules.

## Exact binary structure (all numbers unsigned, **little endian**)

**Header: exactly 96 bytes**

| Offset | Type | Field |
| --- | --- | --- |
| 0 | 8 bytes | ASCII `CPAICPK1` |
| 8 | u32 | version = 1 |
| 12 | u32 | flags: bit 0 = COMPLETE; all other bits zero |
| 16 | 32 bytes | SHA-256 of the **raw, unchanged EPUB file bytes** |
| 48 | u32 | record_count (1–200,000) |
| 52 | u32 | reserved = 0 |
| 56 | u64 | index_offset = 96 |
| 64 | u64 | index_bytes = record_count × 24 |
| 72 | u64 | data_offset = index_offset + index_bytes |
| 80 | u64 | file_bytes = physical size of the .aic file |
| 88 | u32 | index_crc32 |
| 92 | u32 | header_crc32 of bytes 0..91 |

**Index: `record_count` consecutive 24-byte entries**, sorted by ascending
unsigned `sentence_hash`. Duplicate hashes are permitted for true collisions,
but the generator should deduplicate identical normalized sentence texts.

| Offset within entry | Type | Field |
| --- | --- | --- |
| 0 | u64 | sentence_hash = FNV-1a 64 of normalized sentence UTF-8 |
| 8 | u64 | record_offset (absolute file position) |
| 16 | u32 | record_size (16..26,512 bytes) |
| 20 | u32 | record_crc32 |

The index starts immediately after the header and is followed by record bytes.
Offsets may be nonmonotonic because the **index is sorted by hash**, not by
record placement. Each record must be inside the file and start at or after
`data_offset`.

**Record: variable length, exactly `record_size` bytes**

| Field | Type | Meaning |
| --- | --- | --- |
| source_bytes | u32 | 1..2048 |
| translated_bytes | u32 | 1..4096 |
| word_count | u16 | 0..48 |
| flags | u16 | 0 |
| reserved | u32 | 0 |
| source | source_bytes bytes | **normalized** UTF-8 source sentence |
| translation | translated_bytes bytes | translated sentence UTF-8 |
| word entries | variable | each entry below, concatenated |

Each word entry is **8 header bytes** followed by its source and translation:
`<u16 occurrence, u16 source_bytes, u16 translated_bytes, u16 reserved=0>`,
then `source_bytes` of normalized UTF-8 visible source word (1..160), then
`translated_bytes` of its contextual translation (1..256). `occurrence`
is **zero-based among tokens whose normalized visible source word is exactly
equal**, counted in the original logical order of this sentence. Thus the
second `had` is `("had",1)`, independent of other tokens. Include a mapping
for **every selectable word** for a full cache. Duplicate source word and
occurrence pairs must not be generated.

All CRC32s are standard CRC-32/ISO-HDLC (IEEE, zlib-compatible):
initial state `0xffffffff`, reflected polynomial `0xedb88320`, final XOR
`0xffffffff`. The record CRC covers the **entire raw record**, not the index
entry. Index CRC covers the entire packed index. Header CRC covers header
bytes 0..91. There is no global records CRC; a record is validated on access.

## Source normalization (v1)

Operate on UTF-8 **byte sequences**; no Unicode case-folding, NFC/NFKC,
punctuation rewriting or HTML entity rewriting. Replace each maximal run of
ASCII space/tab/LF/CR/VT/FF, U+00A0, U+2000..U+200B, U+202F or U+3000
with a single ASCII space **between** non-whitespace bytes; trim leading
and trailing whitespace. Preserve all other bytes, including punctuation
and case. Apply to sentence text and word tokens before hashing/comparing.
The generator must extract visible text so that Picco's `ClipSelectionActivity`
produces the same **sentence boundaries and punctuation**, not simply split
an HTML chapter on full stops. Whitespace normalization tolerates layout
differences; it cannot repair differences in sentence segmentation.

Hash exactly those normalized sentence bytes with FNV-1a 64: offset basis
`0xcbf29ce484222325`, XOR each UTF-8 byte then multiply modulo 2^64 by
`0x100000001b3`. No book-id, position, separator or terminating NUL in this
hash. Always compare stored source UTF-8 after hash lookup.

Host example for packing primitives (illustrative; no generator implemented):

```python
import hashlib, struct, zlib

book_id = hashlib.sha256(open("Tender Is the Flesh.epub", "rb").read()).digest()
key = 0xcbf29ce484222325
for b in normalized_sentence.encode("utf-8"):
    key = ((key ^ b) * 0x100000001b3) & ((1 << 64) - 1)

word_entry = struct.pack("<HHHH", occurrence, len(word_utf8),
                         len(word_ru_utf8), 0) + word_utf8 + word_ru_utf8
record = (struct.pack("<IIHHI", len(sentence_utf8), len(sentence_ru_utf8),
                      word_count, 0, 0) +
          sentence_utf8 + sentence_ru_utf8 + b"".join(word_entries))
index_entry = struct.pack("<QQII", key, record_offset, len(record),
                          zlib.crc32(record))
```

Do not use Python `str.split()` alone for normalization or token discovery;
it does not guarantee matching the firmware's exact whitespace/token rules.
Sample small batches and compare hash + selected-word occurrence on the device.

## COMPLETE semantics and failure handling

Set flag bit 0 **only if the host has translated every extracted sentence
and every selectable word**, with no missing, truncated or invalid output.
Repeated identical sentences may share a single record: this is a
content-addressed cache, so their contextual translations will be the same
at each occurrence. If different context matters, choose a generic translation
or wait for a future position-disambiguated version of the format.

The device validates magic, version, bounds, header/index CRC, raw EPUB SHA-256
and individual record CRC/length before serving a hit. It reads one index
entry per binary-search step using seek and never loads the entire package into
RAM. Only a matching, validated word+sentence pair is accepted. Any miss,
unsupported selected token or damaged record falls back through existing
caches and then to online translation. A **valid COMPLETE package** disables
new Picco lookahead prefetch jobs even on individual misses, as the host is
responsible for coverage; remove/replace the package to resume lookahead.

The initial full EPUB SHA-256 is streamed using a 2-KiB buffer on the first
Translate after boot (or after EPUB path/size/FAT mtime change). This may
briefly block the popup on very large books. Later Translate opens reuse the
digest for that book during the same boot. A byte-for-byte modification that
preserves both size and FAT modification time may not be noticed until reboot.
If the raw EPUB bytes differ between host and device, the package is rejected.

The package is not authenticated: CRC protects against accidental corruption,
**not malicious tampering**. Install only packages from a trusted source.

## Diagnostics

```text
[AI_PACK] not found (.epub.aic alongside book)
[AI_PACK] found book-id=0123456789abcdef records=4231 complete=1
[AI_PACK] rejected: book-id SHA-256 mismatch
[AI_PACK] rejected: invalid header, index or checksum
[AI_CACHE] Book package cache hit
[AI_CACHE] Book package miss: sentence_missing
[AI_CACHE] Book package miss: word_missing
[AI_CACHE] Online fallback: word_missing
[AI_PRE] Disabled: complete book package
```

These are expected patterns, not evidence of a successful hardware test.
