#include <gtest/gtest.h>

#include <HalStorage.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "AiTranslationCache.h"

using ai_translation_cache::load;
using ai_translation_cache::save;

class AiTranslationCacheTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
  void TearDown() override { Storage.reset(); }

  static bool read(std::string_view book, std::string_view provider, std::string_view model,
                   std::string_view word, std::string_view sentence, std::string& w, std::string& s) {
    return load(book, provider, model, word, sentence, w, s);
  }
};

TEST_F(AiTranslationCacheTest, ExactHitSurvivesRepeatedReads) {
  ASSERT_TRUE(save("/Books/Story.epub", "https://api.example/v1", "m1", "Destroy", "Destroy him.",
                   "Уничтожь", "Уничтожь его."));
  std::string w, s;
  ASSERT_TRUE(read("/Books/Story.epub", "https://api.example/v1", "m1", "Destroy", "Destroy him.", w, s));
  EXPECT_EQ(w, "Уничтожь");
  EXPECT_EQ(s, "Уничтожь его.");
  w.clear();
  s.clear();
  ASSERT_TRUE(read("/Books/Story.epub", "https://api.example/v1", "m1", "Destroy", "Destroy him.", w, s));
  EXPECT_EQ(s, "Уничтожь его.");
}

TEST_F(AiTranslationCacheTest, BookProviderModelWordAndSentenceAreAllPartOfKey) {
  ASSERT_TRUE(save("/Books/Story.epub", "https://api.example/v1", "m1", "Destroy", "Destroy him.",
                   "Уничтожь", "Уничтожь его."));
  std::string w, s;
  EXPECT_FALSE(read("/Books/Other.epub", "https://api.example/v1", "m1", "Destroy", "Destroy him.", w, s));
  EXPECT_FALSE(read("/Books/Story.epub", "https://other.example/v1", "m1", "Destroy", "Destroy him.", w, s));
  EXPECT_FALSE(read("/Books/Story.epub", "https://api.example/v1", "m2", "Destroy", "Destroy him.", w, s));
  EXPECT_FALSE(read("/Books/Story.epub", "https://api.example/v1", "m1", "him", "Destroy him.", w, s));
  EXPECT_FALSE(read("/Books/Story.epub", "https://api.example/v1", "m1", "Destroy", "Destroy her.", w, s));
}

TEST_F(AiTranslationCacheTest, CorruptPayloadIsNeverReturned) {
  ASSERT_TRUE(save("/a.epub", "https://api.example/v1", "m", "test", "Test sentence.",
                   "тест", "Тестовое предложение."));
  std::filesystem::path entry;
  for (const auto& f : std::filesystem::recursive_directory_iterator(HalStorage::root())) {
    if (f.path().extension() == ".bin") { entry = f.path(); break; }
  }
  ASSERT_FALSE(entry.empty());
  { std::fstream out(entry, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(-1, std::ios::end);
    out.put('X');
  }
  std::string w, s;
  EXPECT_FALSE(read("/a.epub", "https://api.example/v1", "m", "test", "Test sentence.", w, s));
  EXPECT_TRUE(w.empty());
  EXPECT_TRUE(s.empty());
}

TEST_F(AiTranslationCacheTest, RejectsOversizedInputAndDoesNotWrite) {
  EXPECT_FALSE(save("/a.epub", "https://api.example/v1", "m", "test",
                    std::string(2049, 'a'), "тест", "Тест."));
  EXPECT_FALSE(Storage.exists("/.crosspoint/ai-cache"));
}
