#!/usr/bin/env python3
"""Self-check: NDJSON parser must reject shifted/missing sentence mappings."""
import importlib.util
import json
import os

spec = importlib.util.spec_from_file_location(
    "gen", os.path.join(os.path.dirname(__file__), "build_aic.py"))
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

KEYS = {0: "00-aaaa", 1: "01-bbbb", 2: "02-cccc"}


def lines(*items):
    return "\n".join(json.dumps(x) for x in items)


good = lines(*({"id": i, "key": k, "translation": f"T{i}"} for i, k in KEYS.items()))
assert set(gen.parse_ndjson(good, KEYS)) == set(KEYS)

# Translation of sentence 1 echoed under id 0 with sentence 1's key: must fail.
shifted = lines({"id": 0, "key": KEYS[1], "translation": "T1"},
                {"id": 1, "key": KEYS[2], "translation": "T2"},
                {"id": 2, "key": KEYS[0], "translation": "T0"})
try:
    gen.parse_ndjson(shifted, KEYS)
except ValueError:
    pass
else:
    raise AssertionError("shifted mapping accepted")

# One line missing: must fail instead of silently leaving a hole.
missing = lines(*({"id": i, "key": k, "translation": f"T{i}"}
                  for i, k in list(KEYS.items())[:2]))
try:
    gen.parse_ndjson(missing, KEYS)
except ValueError:
    pass
else:
    raise AssertionError("missing line accepted")

# A line from outside the batch must be ignored, not assigned to a sentence.
extra = good + "\n" + json.dumps({"id": 9, "key": "09-dead", "translation": "T9"})
assert set(gen.parse_ndjson(extra, KEYS)) == set(KEYS)
assert gen.has_untranslated("to be burdened with such a family", "и вдругsuch обзавестись")
assert not gen.has_untranslated("The cop watched", "Коп наблюдал")
print("alignment self-check OK")
