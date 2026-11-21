#!/usr/bin/env python3
"""Source-level guard for sentence-only AI translation.

The Picco hardware and the C++ unit tests still need execution by the builder.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
popup = (root / "src/activities/reader/AiSentenceTranslationActivity.cpp").read_text()
prefetch = (root / "src/activities/reader/AiSentencePrefetchSession.cpp").read_text()
exact = (root / "src/util/AiTranslationCache.cpp").read_text()
indexed = (root / "src/util/AiBookCache.cpp").read_text()
lookahead = (root / "src/util/AiPrefetchCache.cpp").read_text()

assert "Translate this complete sentence" in popup
assert "wordTranslation" not in popup
assert "word_" not in popup
assert "ai_book_cache::lookupSentence(" in popup
assert "ai_prefetch::lookupSentence(" in popup
assert "ai_translation_cache::load(" in popup
assert "ai_translation_cache::save(" in popup
assert 'append(tr(STR_AI_ORIGINAL), true);' in popup
assert 'append(sentenceTranslation_, false);' in popup
assert '"need_sentence"' not in prefetch
assert 'record.words.clear();' in prefetch
assert 'current_.words.push_back' not in prefetch
assert 'return !translated ||' not in prefetch
assert '0x32494143' in exact  # CAI2
assert 'key(s, sentence)' in exact
assert 'h.wordBytes == 0' in exact
assert 'ReadStatus readRecord(' in indexed
assert 'wordCount = le16(rh + 8)' in indexed  # still validate legacy entries
assert 'Result lookupSentence(' in indexed
assert 'delta.words.clear();' in lookahead
assert 'bool lookupSentence(' in lookahead
print("Sentence-only AI translation contract passed")
