#!/usr/bin/env python3
"""Check quality rejection and server-side repair without network calls."""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, filename))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("gen", "build_aic.py")

assert gen.quality_issues("The cop watched", "Коп наблюдал") == []
assert "latin_leftover" in gen.quality_issues("to be burdened with a family", "и вдругsuch обзавестись")
assert "latin_leftover" in gen.quality_issues("conscientious policeman", "добросовестного copa")
assert "empty" in gen.quality_issues("Hello there", "")
assert "untranslated_copy" in gen.quality_issues("Hello there", "Hello there")

server = load("server", "cache_server.py")
source = "The cop watched"
key = str(gen.fnv1a(source))
results = {key: {"id": 0, "key": "00-0000", "translation": "добросовестного copa"}}
server.gen.translate_batch = lambda batch, ctx, records, offset: {
    0: {"id": 0, "key": "00-0000", "translation": "Коп наблюдал"}}
bad, fixed = server.quality_repair("test", [{"sentence": source}], results, {})
assert (bad, fixed) == (1, 1)
assert results[key]["translation"] == "Коп наблюдал"
assert results[key]["key"] == f"00-{gen.fnv1a(source) & 0xffff:04x}"
print("quality repair self-check OK")
