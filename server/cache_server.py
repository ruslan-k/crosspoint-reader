#!/usr/bin/env python3
"""CrossPoint AI Cache Server — translate EPUBs into indexed .aic packages.

Stdlib only. Jobs are resumable on disk. The server publishes a current .aic
snapshot after every batch so a reader can refresh its cache while reading.
"""
import email.parser
import email.policy
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_aic as gen

ROOT = os.environ.get("CP_CACHE_ROOT", "/tmp/crosspoint_cache/server")
PORT = int(os.environ.get("CP_CACHE_PORT", "8791"))
BATCH = int(os.environ.get("CP_CACHE_BATCH", "16"))
TOKEN = os.environ.get("CP_CACHE_TOKEN", "")
jobs = {}
lock = threading.Lock()


def job_paths(sha):
    d = os.path.join(ROOT, sha)
    return d, os.path.join(d, "book.epub"), os.path.join(d, "results.json"), \
        os.path.join(d, "cache.aic"), os.path.join(d, "state.json")


def load_state(sha):
    d, epub, results, aic, state_file = job_paths(sha)
    os.makedirs(d, exist_ok=True)
    if os.path.exists(state_file):
        return json.load(open(state_file))
    return {"state": "missing", "done": 0, "total": 0, "bytes": 0, "aic_sha256": ""}


def save_state(sha, state):
    d, _, _, _, state_file = job_paths(sha)
    tmp = state_file + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f)
    os.replace(tmp, state_file)
    with lock:
        jobs[sha] = state


def snapshot(sha, records, results, complete):
    """Publish an atomically replaced .aic with current translations."""
    d, epub, results_file, aic, _ = job_paths(sha)
    translated = [r for r in records if str(gen.fnv1a(r["sentence"])) in results]
    tmp = aic + ".part"
    info = gen.pack(epub, translated, results, tmp, complete=complete)
    os.replace(tmp, aic)
    digest = hashlib.sha256(open(aic, "rb").read()).hexdigest()
    return info["records"], info["bytes"], digest


def worker(sha):
    d, epub, results_file, aic, _ = job_paths(sha)
    try:
        results = json.load(open(results_file)) if os.path.exists(results_file) else {}
        records = gen.extract(epub)
        ctx = gen.book_context(epub, records)
        by_key = {str(gen.fnv1a(r["sentence"])): i for i, r in enumerate(records)}
        todo = [(by_key[str(gen.fnv1a(r["sentence"]))], r) for r in records
                if str(gen.fnv1a(r["sentence"])) not in results]
        save_state(sha, {"state": "translating", "done": len(results), "total": len(records),
                         "bytes": 0, "aic_sha256": ""})
        for start in range(0, len(todo), BATCH):
            chunk = todo[start:start + BATCH]
            resp = {}
            for attempt in range(3):
                try:
                    resp = gen.translate_batch([r for _, r in chunk], ctx, records, chunk[0][0])
                    break
                except Exception as exc:
                    print(f"[{sha[:8]}] batch {start} attempt {attempt}: {exc}", flush=True)
                    time.sleep(3)
            for i, (_, rec) in enumerate(chunk):
                obj = resp.get(i)
                if obj and obj.get("translation"):
                    results[str(gen.fnv1a(rec["sentence"]))] = obj
            with open(results_file, "w") as f:
                json.dump(results, f, ensure_ascii=False)
            done, size, digest = snapshot(sha, records, results, False)
            save_state(sha, {"state": "translating", "done": done, "total": len(records),
                             "bytes": size, "aic_sha256": digest})
            print(f"[{sha[:8]}] {done}/{len(records)}", flush=True)
        fully_translated = len(results) >= len(records)
        done, size, digest = snapshot(sha, records, results, fully_translated)
        save_state(sha, {"state": "ready" if fully_translated else "translating", "done": done, "total": len(records),
                         "bytes": size, "aic_sha256": digest})
        print(f"[{sha[:8]}] {'READY' if fully_translated else 'PARTIAL'} {done}/{len(records)}", flush=True)
    except Exception as exc:
        print(f"[{sha[:8]}] FAILED: {exc}", flush=True)
        save_state(sha, {"state": "error", "done": 0, "total": 0, "bytes": 0,
                         "aic_sha256": "", "error": str(exc)[:200]})


def multipart_fields(body, content_type):
    """Minimal multipart/form-data parser (stdlib email)."""
    header = "Content-Type: " + content_type + "\r\nMIME-Version: 1.0\r\n\r\n"
    msg = email.parser.BytesParser(policy=email.policy.default).parsebytes(header.encode() + body)
    out = {}
    for part in msg.iter_parts():
        name = part.get_param("name", header="content-disposition")
        data = part.get_payload(decode=True)
        if name and data is not None:
            out[name] = (part.get_filename(), data)
    return out


class Handler(BaseHTTPRequestHandler):
    server_version = "CrossPointAICache/1"

    def log_message(self, fmt, *args):
        msg = fmt % args
        msg = re.sub(r"(token=)[^&\s]+", r"\1<redacted>", msg)
        print("[http] " + msg, flush=True)

    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        query = self.path.split("?", 1)[1] if "?" in self.path else ""
        params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
        if TOKEN and (path.startswith("/api/cache/") or path in ("/status", "/download")) \
                and params.get("token", "") != TOKEN:
            return self.reply(401, {"error": "unauthorized"})
        if path == "/health":
            return self.reply(200, {"ok": True, "version": 1})
        if path in ("/api/cache/status", "/status"):
            sha = params.get("sha", "").lower()
            if len(sha) != 64:
                return self.reply(400, {"error": "bad sha"})
            return self.reply(200, load_state(sha))
        if path in ("/api/cache/download", "/download"):
            sha = params.get("sha", "").lower()
            d, _, _, aic, _ = job_paths(sha)
            if not os.path.exists(aic):
                return self.reply(404, {"error": "no package"})
            data = open(aic, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Aic-Sha256", hashlib.sha256(data).hexdigest())
            self.end_headers()
            self.wfile.write(data)
            return
        self.reply(404, {"error": "not found"})

    def do_POST(self):
        if self.path.split("?", 1)[0] not in ("/api/cache/job", "/job"):
            return self.reply(404, {"error": "not found"})
        params = dict(p.split("=", 1) for p in self.path.split("?", 1)[1].split("&") if "=" in p) \
            if "?" in self.path else {}
        if TOKEN and params.get("token", "") != TOKEN:
            return self.reply(401, {"error": "unauthorized"})
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 64 * 1024 * 1024:
            return self.reply(413, {"error": "bad size"})
        body = self.rfile.read(length)
        fields = multipart_fields(body, self.headers.get("Content-Type", ""))
        epub_part = fields.get("file")
        sha = fields.get("sha", (None, b""))[1].decode().strip().lower()
        if not epub_part or len(sha) != 64:
            return self.reply(400, {"error": "need file and sha"})
        data = epub_part[1]
        digest = hashlib.sha256(data).hexdigest()
        if digest != sha:
            return self.reply(400, {"error": "sha mismatch", "actual": digest})
        d, epub, results, aic, _ = job_paths(sha)
        os.makedirs(d, exist_ok=True)
        with open(epub, "wb") as f:
            f.write(data)
        state = load_state(sha)
        if state["state"] in ("queued", "translating", "ready"):
            return self.reply(200, {"job_id": sha, "state": state["state"]})
        save_state(sha, {"state": "queued", "done": 0, "total": 0, "bytes": 0, "aic_sha256": ""})
        threading.Thread(target=worker, args=(sha,), daemon=True).start()
        self.reply(202, {"job_id": sha, "state": "queued"})


if __name__ == "__main__":
    os.makedirs(ROOT, exist_ok=True)
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"CrossPoint AI Cache Server on 0.0.0.0:{PORT}, jobs in {ROOT}", flush=True)
    server.serve_forever()
