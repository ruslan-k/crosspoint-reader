#!/usr/bin/env python3
"""Guard the Picco AI popup UX and bounded-response implementation."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
src = (ROOT / "src/activities/reader/AiSentenceTranslationActivity.cpp").read_text(encoding="utf-8")
hdr = (ROOT / "src/activities/reader/AiSentenceTranslationActivity.h").read_text(encoding="utf-8")
clip = (ROOT / "src/activities/reader/ClipSelectionActivity.h").read_text(encoding="utf-8")

required = [
    "captureUnderlyingPage();",
    "MALLOC_CAP_SPIRAM",
    "memcpy(renderer.getWriteTarget(), background_, HalDisplay::BUFFER_SIZE)",
    "void AiSentenceTranslationActivity::drawPopup() const",
    "void AiSentenceTranslationActivity::scrollPopup(",
    "MappedInputManager::SwipeDir::Up",
    "MappedInputManager::Button::PageForward",
    "mappedInput.wasScreenTapped(x, y)",
    "renderer.setClipRect(",
    "popupLines_.push_back(",
    "append(sentence_, false);",
    "append(sentenceTranslation_, false);",
    "renderer.drawText(fontId",
    "requestUpdate();",
]
for item in required:
    assert item in src, f"Missing popup capability: {item}"
assert "DictionaryDefinitionActivity>(" not in src, "Old full-screen result activity remains"
assert "Showing" in hdr and "popupScroll_" in hdr
assert "MAX_SELECTABLE_WORDS = 320" in clip
print("AI translation popup, scrolling, and selection guards passed")
