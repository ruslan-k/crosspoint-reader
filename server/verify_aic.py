#!/usr/bin/env python3
"""Verify a CPAICPK1 package: header, index, records, book id, alignment keys."""
import hashlib
import importlib.util
import json
import os
import struct
import sys
import zlib

BATCH = int(os.environ.get("CP_CACHE_BATCH", "16"))

spec = importlib.util.spec_from_file_location(
    "gen", os.path.join(os.path.dirname(__file__), "build_aic.py"))
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


def verify(epub, aic, results=None):
    data = open(aic, "rb").read()
    problems = []
    if data[:8] != b"CPAICPK1":
        raise AssertionError("bad magic")
    version, flags = struct.unpack_from("<II", data, 8)
    book_sha = data[16:48]
    count, reserved = struct.unpack_from("<II", data, 48)
    idx_off, idx_bytes, data_off, file_bytes = struct.unpack_from("<QQQQ", data, 56)
    idx_crc, hdr_crc = struct.unpack_from("<II", data, 88)
    if version != 1 or flags & ~1 or reserved:
        problems.append(f"bad header version/flags/reserved: {version}/{flags}/{reserved}")
    if zlib.crc32(data[:92]) != hdr_crc:
        problems.append("header crc mismatch")
    if zlib.crc32(data[96:96 + idx_bytes]) != idx_crc:
        problems.append("index crc mismatch")
    want_sha = hashlib.sha256(open(epub, "rb").read()).digest()
    if book_sha != want_sha:
        problems.append("book sha mismatch")
    if idx_off != 96 or idx_bytes != count * 24 or data_off != 96 + idx_bytes or file_bytes != len(data):
        problems.append("offset/size mismatch")
    records = gen.extract(epub)
    by_src = {r["sentence"]: i for i, r in enumerate(records)}
    last = -1
    used = set()
    for n in range(count):
        key, off, length, crc = struct.unpack_from("<QQII", data, 96 + n * 24)
        if key <= last:
            problems.append(f"index not sorted at {n}")
        last = key
        if off < data_off or off + length > len(data) or length < 16:
            problems.append(f"bad record bounds at {n}")
            continue
        body = data[off:off + length]
        if zlib.crc32(body) != crc:
            problems.append(f"record crc mismatch at {n}")
        src_len, dst_len, words, zero, zero2 = struct.unpack_from("<IIHHI", body, 0)
        if words or zero or zero2 or src_len + dst_len + 16 != length:
            problems.append(f"bad record layout at {n}")
            continue
        src = body[16:16 + src_len].decode("utf-8", "replace")
        dst = body[16 + src_len:].decode("utf-8", "replace")
        if gen.fnv1a(src) != key:
            problems.append(f"hash mismatch at {n}")
        if src not in by_src:
            problems.append(f"source not in epub at {n}: {src[:60]!r}")
        else:
            i = by_src[src]
            used.add(i)
            if results:
                obj = results.get(str(key), {})
                want = f"{i % BATCH:02d}-{key & 0xffff:04x}"
                if obj.get("key") not in (None, want):
                    problems.append(f"alignment key mismatch at {n} (idx {i}): {obj.get('key')} != {want}")
    return {"count": count, "complete": bool(flags & 1), "bytes": len(data),
            "sources_covered": len(used), "sources_total": len(records),
            "problems": problems[:20], "problem_count": len(problems)}


if __name__ == "__main__":
    epub, aic = sys.argv[1], sys.argv[2]
    results_file = sys.argv[3] if len(sys.argv) > 3 else None
    results = json.load(open(results_file, encoding="utf-8")) if results_file else None
    print(json.dumps(verify(epub, aic, results), ensure_ascii=False, indent=1))
