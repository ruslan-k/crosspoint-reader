# CrossPoint AI Cache Server

Optional host-side companion that translates an EPUB into Russian and publishes a
CPAICPK1 `.epub.aic` package while the reader is still reading. The firmware's
sentence translation popup then serves sentences from the on-SD cache instead
of waiting for a per-sentence network request.

## Requirements

- Python 3, standard library only
- `hermes` on `PATH` and signed in to the model/provider used by `build_aic.py`
  (currently `hy4-flash-free` / `opencode-go`, `reasoning=none`)
- The EPUB and its SHA-256 (both are sent by the reader)

## Run

```bash
cd server
python3 cache_server.py
```

| Variable | Default | Meaning |
| --- | --- | --- |
| `CP_CACHE_PORT` | `8791` | Listening port |
| `CP_CACHE_ROOT` | `/tmp/crosspoint_cache/server` | Job storage (EPUB, results, snapshot) |
| `CP_CACHE_BATCH` | `16` | Sentences per translation request |
| `CP_CACHE_TOKEN` | empty | Optional shared secret, sent by the reader as `?token=` |

In the device web UI set **AI Cache Server URL** to `http://<host>:8791` and,
if `CP_CACHE_TOKEN` is set, the same value in **AI Cache Server Token**. The
reader posts the book, polls status and downloads published `.aic` snapshots;
the package is stored next to the EPUB.

## API

- `GET /health` → `{"ok":true,"version":1}`
- `POST /api/cache/job` — multipart `file` (EPUB) and `sha` (hex SHA-256). Returns
  `202` when queued or `200` if the job is already known. A SHA mismatch is rejected.
- `GET /api/cache/status?sha=...` → `state` (`missing`, `queued`, `translating`,
  `ready`, `error`), `done`, `total`, `bytes`, `aic_sha256`
- `GET /api/cache/download?sha=...` → current `.aic` bytes with an
  `X-Aic-Sha256` header

Jobs are resumable: translations are stored by FNV-1a hash of the normalized
source sentence, so an interrupted job continues from where it stopped.

## Sentence alignment

Each sentence in a batch has an `id` and a short `key` (batch position plus 16
bits of FNV-1a of the source text). The model must echo both. A response line is
accepted only when its key matches the expected sentence and all ids `0..N-1`
appear exactly once; otherwise the batch is retried up to three times. This
prevents a model numbering mistake from attaching a translation to the wrong
sentence. Increasing reasoning effort does not prevent this class of error.

Each batch writes its prompt to a separate temporary file, so concurrent book
jobs cannot overwrite each other's prompts.

## Verification

```bash
python3 test_alignment.py
python3 verify_aic.py book.epub <sha>/cache.aic <sha>/results.json
```

`test_alignment.py` checks that shifted, missing and out-of-batch response lines
are rejected. `verify_aic.py` checks header/index CRCs, sorted hashes, record
bounds and CRCs, book SHA-256, `FNV-1a(source)` against each index key, and the
echoed alignment keys. Both reported clean results for 645 sentences across
four public-domain O. Henry stories (160/157/164/164 sentences).

## Format

Sentence splitting and byte-only whitespace normalization in `build_aic.py`
mirror the firmware extractor — changing them breaks cache lookups. See
[ai-cache-format.md](../docs/ai-cache-format.md) and
[ai-translation.md](../docs/ai-translation.md).
