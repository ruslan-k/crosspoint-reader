#!/usr/bin/env python3
"""Build a CrossPoint indexed AI book cache (.aic) with Hermes' default model.

Pipeline: EPUB -> exact-ish sentence/token extraction -> batch translation via
`hermes chat` -> CPAICPK1 v1 package. Resumable: results are cached on disk.
"""
import argparse
import hashlib
import html
import json
import os
import re
import struct
import subprocess
import tempfile
import sys
import time
import xml.etree.ElementTree as ET
import zipfile
import zlib

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1
ABBREV = ("mr.", "mrs.", "ms.", "dr.", "prof.", "sr.", "jr.", "st.",
          "vs.", "etc.", "e.g.", "i.e.", "p.m.", "a.m.", "u.s.", "u.k.")
CLOSE = ('"', "'", ")", "]", "}", "\u00bb", "\u201d", "\u2019")


def book_context(epub, records):
    """Small stable prefix shared by every batch: metadata + opening prose."""
    with zipfile.ZipFile(epub) as z:
        root = ET.fromstring(z.read("META-INF/container.xml"))
        opf_name = root.find(".//{urn:oasis:names:tc:opendocument:xmlns:container}rootfile").get("full-path")
        text = z.read(opf_name).decode("utf-8", "replace")
    def meta(tag):
        m = re.search(r"<" + tag + r"[^>]*>(.*?)</" + tag + r">", text, re.S | re.I)
        return html.unescape(re.sub(r"<[^>]+>", "", m.group(1))).strip() if m else ""
    opening = " ".join(r["sentence"] for r in records[8:18])
    return {"title": meta("dc:title"), "author": meta("dc:creator"),
            "language": meta("dc:language"), "opening": opening[:900]}


def is_end(token):
    """Port of ai_sentence::isEnd from the firmware."""
    t = token
    while t and t[-1] in CLOSE:
        t = t[:-1]
    if not t:
        return False
    if t.endswith(("\u3002", "\uff01", "\uff1f")):
        return True
    if t[-1] in "!?":
        return True
    if t[-1] != ".":
        return False
    if t.casefold() in ABBREV:
        return False
    if len(t) == 2 and t[0].isalpha() and t[0].isascii():
        return False
    return True


def normalize(text):
    """Port of ai_book_cache::normalize (whitespace fold only)."""
    out = []
    pending_space = False
    for ch in text:
        if ch in " \t\n\r\v\f\u00a0\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u200b\u202f\u3000":
            if out:
                pending_space = True
        else:
            if pending_space:
                out.append(" ")
                pending_space = False
            out.append(ch)
    return "".join(out)


def fnv1a(text):
    h = FNV_OFFSET
    for b in text.encode("utf-8"):
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def clean_token(tok):
    """Port of cleanWordStart (em-space + leading whitespace only)."""
    if tok.startswith("\u2003"):
        tok = tok[3:]
    return tok.lstrip(" \t\n\r\u00a0")


def spine_docs(path):
    with zipfile.ZipFile(path) as z:
        root = ET.fromstring(z.read("META-INF/container.xml"))
        opf_name = root.find(".//{urn:oasis:names:tc:opendocument:xmlns:container}rootfile").get("full-path")
        opf = ET.fromstring(z.read(opf_name))
        ns = {"o": "http://www.idpf.org/2007/opf"}
        manifest = {i.get("id"): i.get("href") for i in opf.findall(".//o:manifest/o:item", ns)}
        base = os.path.dirname(opf_name)
        for ref in opf.findall(".//o:spine/o:itemref", ns):
            href = manifest.get(ref.get("idref"), "")
            if not href.lower().endswith((".xhtml", ".html", ".htm")):
                continue
            name = os.path.normpath(os.path.join(base, href)).replace("\\", "/")
            try:
                yield ref.get("idref"), z.read(name)
            except KeyError:
                continue


def paragraphs(xhtml):
    root = ET.fromstring(xhtml)
    for el in root.iter():
        if el.tag.rsplit("}", 1)[-1].lower() not in ("p", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote"):
            continue
        text = html.unescape("".join(el.itertext()))
        # Keep whitespace gaps: the firmware inserts a space between tokens
        # whose source offsets are not adjacent.
        if len(text.strip()) >= 15:
            yield text


def extract(epub, limit=0):
    """Yield {sentence, words:[{source, occurrence}]} in reading order."""
    out = []
    seen = set()
    for _item_id, data in spine_docs(epub):
        try:
            paras = list(paragraphs(data))
        except ET.ParseError:
            continue
        for para in paras:
            current, words = [], []
            for raw in re.split(r"(\s+)", para):
                if not raw:
                    continue
                if raw.isspace():
                    continue
                tok = clean_token(raw)
                if not tok:
                    continue
                if current:
                    current.append(" ")
                else:
                    pass
                current.append(tok)
                if len(tok) <= 160:
                    words.append({"source": normalize(tok), "occurrence": 0})
                if is_end(tok):
                    _emit(out, seen, current, words, limit)
                    current, words = [], []
            _emit(out, seen, current, words, limit)
            if limit and len(out) >= limit:
                return out
    return out


def _emit(out, seen, current, words, limit):
    if not current:
        return
    sentence = normalize("".join(current))
    if len(sentence.encode()) > 2048 or len(sentence) < 1:
        return
    key = fnv1a(sentence)
    if key in seen:
        return
    if limit and len(out) >= limit:
        return
    seen.add(key)
    counts = {}
    for w in words:
        w["occurrence"] = counts.get(w["source"], 0)
        counts[w["source"]] = w["occurrence"] + 1
    out.append({"sentence": sentence, "words": words})


PROMPT_HEAD = (
    "You are translating a novel into Russian. Use the book context and the immediately "
    "preceding/following sentences to resolve pronouns, tense and ambiguous words. "
    "Context fields are for disambiguation only: never translate or output them. "
    "Return one compact, standalone JSON object per sentence (NDJSON), in increasing id order. "
    'Schema: {"id":0,"key":"ab12","translation":"..."}. '
    "Copy key verbatim from that sentence. "
    "No analysis, reasoning, markdown fences or enclosing array. Each line must be standalone JSON. Source:\n"
)


def parse_ndjson(raw, keys):
    """Map output by echoed key; reject shifted, duplicate or missing ids."""
    out = {}
    for line in raw.splitlines():
        line = line.strip().strip("`").strip()
        if not line.startswith("{"):
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(obj, dict):
            continue
        try:
            i = int(obj.get("id"))
        except (TypeError, ValueError):
            continue
        if i in out or obj.get("key") != keys.get(i):
            continue
        out[i] = obj
    if set(out) != set(keys):
        raise ValueError(f"translation ids/keys incomplete: {sorted(out)} != {sorted(keys)}")
    return out


def has_untranslated(source, translation):
    """Catch a lowercase English word copied verbatim into Russian output."""
    leftovers = {w.lower() for w in re.findall(r"\b[a-z]{3,}\b", source)}
    return bool(leftovers & {w.lower() for w in re.findall(r"[A-Za-z]{3,}", translation)})


def translate_batch(batch, ctx, all_records, offset):
    payload = {"book": ctx, "sentences": []}
    keys = {}
    for i, rec in enumerate(batch):
        prev_text = all_records[offset + i - 1]["sentence"] if offset + i > 0 else ""
        next_text = all_records[offset + i + 1]["sentence"] if offset + i + 1 < len(all_records) else ""
        keys[i] = f"{i:02d}-{fnv1a(rec['sentence']) & 0xffff:04x}"
        payload["sentences"].append({
            "id": i,
            "key": keys[i],
            "text": rec["sentence"],
            "prev": prev_text[:300],
            "next": next_text[:300],
        })
    prompt = PROMPT_HEAD + json.dumps(payload, ensure_ascii=False)
    fd, pf = tempfile.mkstemp(prefix="cp_prompt_", suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(prompt)
    cmd = ["hermes", "chat", "--query-file", pf, "--oneshot", "-Q", "--reasoning", "none",
           "--toolsets", "", "-m", "hy4-flash-free", "--provider", "opencode-go",
           "--max-turns", "1", "--run-budget", "180"]
    try:
        raw = subprocess.run(cmd, capture_output=True, text=True, timeout=240).stdout
    finally:
        os.unlink(pf)
    out = parse_ndjson(raw, keys)
    for i, obj in out.items():
        if has_untranslated(batch[i]["sentence"], obj["translation"]):
            raise ValueError(f"untranslated source word in sentence {i}")
    return out


def pack(epub, records, translations, out_path, complete=True):
    book_sha = hashlib.sha256(open(epub, "rb").read()).digest()
    entries, payloads = [], []
    used = set()
    for rec in records:
        tr = translations.get(str(fnv1a(rec["sentence"])))
        if not tr or not tr.get("translation"):
            continue
        src = rec["sentence"].encode()
        dst = tr["translation"].encode()
        if not (1 <= len(src) <= 2048 and 1 <= len(dst) <= 4096):
            continue
        word_entries = []  # sentence-only cache (firmware 489fa697+)
        body = struct.pack("<IIHHI", len(src), len(dst), len(word_entries), 0, 0) + src + dst + b"".join(word_entries)
        key = fnv1a(rec["sentence"])
        if key in used:
            continue
        used.add(key)
        entries.append((key, body))
    entries.sort(key=lambda e: e[0])
    index = bytearray()
    data = bytearray()
    for key, body in entries:
        index += struct.pack("<QQII", key, 96 + len(entries) * 24 + len(data), len(body), zlib.crc32(body))
        data += body
    header = bytearray(96)
    header[0:8] = b"CPAICPK1"
    struct.pack_into("<II", header, 8, 1, 1 if complete else 0)
    header[16:48] = book_sha
    struct.pack_into("<II", header, 48, len(entries), 0)
    struct.pack_into("<QQQQ", header, 56, 96, len(index), 96 + len(index), 96 + len(index) + len(data))
    struct.pack_into("<I", header, 88, zlib.crc32(index))
    struct.pack_into("<I", header, 92, zlib.crc32(header[:92]))
    with open(out_path, "wb") as f:
        f.write(header + index + data)
    return {"records": len(entries), "bytes": len(header) + len(index) + len(data), "book_sha": book_sha.hex()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("epub")
    ap.add_argument("out")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--batch", type=int, default=16)
    ap.add_argument("--results", default="results.json")
    args = ap.parse_args()

    records = extract(args.epub, args.limit)
    print(f"sentences={len(records)}", flush=True)
    results = {}
    if os.path.exists(args.results):
        results = json.load(open(args.results, encoding="utf-8"))
    ctx = book_context(args.epub, records)
    records_by_key = {str(fnv1a(r["sentence"])): i for i, r in enumerate(records)}
    todo = [(records_by_key[str(fnv1a(r["sentence"]))], r) for r in records
            if str(fnv1a(r["sentence"])) not in results]
    print(f"todo={len(todo)}", flush=True)
    for start in range(0, len(todo), args.batch):
        chunk = todo[start:start + args.batch]
        for attempt in range(3):
            try:
                resp = translate_batch([r for _, r in chunk], ctx, records, chunk[0][0])
                break
            except Exception as e:
                print(f"batch {start} attempt {attempt}: {e}", flush=True)
                resp = {}
                time.sleep(3)
        for i, (_, rec) in enumerate(chunk):
            obj = resp.get(i)
            if obj and obj.get("translation"):
                results[str(fnv1a(rec["sentence"]))] = obj
        json.dump(results, open(args.results, "w", encoding="utf-8"), ensure_ascii=False)
        print(f"done {min(start + args.batch, len(todo))}/{len(todo)} translated={len(results)}", flush=True)
    info = pack(args.epub, records, results, args.out, complete=(len(results) == len(records)))
    print("PACKED", json.dumps(info), flush=True)


if __name__ == "__main__":
    main()
