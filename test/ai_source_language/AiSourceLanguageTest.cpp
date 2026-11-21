#include <cassert>
#include <cstdio>
#include <string>

#include "util/AiSourceLanguage.h"

static void feed(LanguageProbe& probe, const std::string& text) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
  size_t offset = 0;
  while (offset < text.size()) {
    const size_t written = probe.write(bytes + offset, text.size() - offset);
    if (written == 0) break;
    offset += written;
  }
}

static LanguageCounters count(const std::string& text, size_t budget = 65536) {
  LanguageProbe probe(budget);
  feed(probe, text);
  return probe.counters();
}

int main() {
  std::string russianText;
  for (int i = 0; i < 40; ++i) russianText += "Это тест. ";
  const std::string russian =
      "<html><body><p>" + russianText +
      "<script>ыыыыыыыы</script><p data-x='ёёёёёё'>Привет мир</p></body></html>";
  const LanguageCounters rc = count(russian);
  assert(classifySourceLanguage(rc, "en") == SourceLanguage::Russian);
  assert(rc.cyrillic > 0 && rc.ruUnique >= 4);

  std::string chinese;
  for (int i = 0; i < 40; ++i) chinese += "\xE4\xB8\xAD";  // 中
  const LanguageCounters cc = count("<html><body><p>" + chinese + "</p></body></html>");
  assert(classifySourceLanguage(cc, "en") == SourceLanguage::Chinese);

  const LanguageCounters bc = count("<html><body><p>Това е български текст събор.</p></body></html>");
  assert(classifySourceLanguage(bc, "bg") == SourceLanguage::Bulgarian);

  const LanguageCounters uc = count("<html><body><p>Це тест. Привіт Україна.</p></body></html>");
  assert(classifySourceLanguage(uc, "uk") == SourceLanguage::Ukrainian);

  const LanguageCounters ec = count("<html><body><p>Hello world, this is an English book.</p></body></html>");
  assert(classifySourceLanguage(ec, "en") == SourceLanguage::Other);

  LanguageCounters empty;
  assert(classifySourceLanguage(empty, "en") == SourceLanguage::Unknown);
  assert(!sourceAllowed(SourceLanguage::Unknown));
  assert(!sourceAllowed(SourceLanguage::Russian));
  assert(!sourceAllowed(SourceLanguage::Chinese));
  assert(sourceAllowed(SourceLanguage::Bulgarian));
  assert(sourceAllowed(SourceLanguage::Ukrainian));
  assert(sourceAllowed(SourceLanguage::Other));

  // Russian-looking text in attributes and script bodies must not count.
  const LanguageCounters attrs = count("<html data-x='ыыыы'><script>ёёёё</script><body>Hello world.</body></html>");
  assert(attrs.cyrillic == 0 && attrs.ruUnique == 0);

  // Fixed byte budget must stop the stream early.
  const LanguageCounters capped = count(russian, 64);
  assert(capped.cyrillic < rc.cyrillic);

  std::puts("AI source language classification checks passed");
  return 0;
}
