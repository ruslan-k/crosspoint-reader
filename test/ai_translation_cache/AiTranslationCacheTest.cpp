#include <HalStorage.h>
#include <gtest/gtest.h>
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
};

TEST_F(AiTranslationCacheTest, SentenceHitSurvivesRepeatedReads) {
  ASSERT_TRUE(save("/Books/Story.epub", "https://api.example/v1", "m1",
                   "Destroy him.", "Уничтожь его."));
  std::string translated;
  ASSERT_TRUE(load("/Books/Story.epub", "https://api.example/v1", "m1", "Destroy him.", translated));
  EXPECT_EQ(translated, "Уничтожь его.");
  translated.clear();
  ASSERT_TRUE(load("/Books/Story.epub", "https://api.example/v1", "m1", "Destroy him.", translated));
  EXPECT_EQ(translated, "Уничтожь его.");
}

TEST_F(AiTranslationCacheTest, BookProviderModelSentenceAreTheKey) {
  ASSERT_TRUE(save("/Books/Story.epub", "https://api.example/v1", "m1",
                   "Destroy him.", "Уничтожь его."));
  std::string translated;
  EXPECT_FALSE(load("/Books/Other.epub", "https://api.example/v1", "m1", "Destroy him.", translated));
  EXPECT_FALSE(load("/Books/Story.epub", "https://other.example/v1", "m1", "Destroy him.", translated));
  EXPECT_FALSE(load("/Books/Story.epub", "https://api.example/v1", "m2", "Destroy him.", translated));
  EXPECT_FALSE(load("/Books/Story.epub", "https://api.example/v1", "m1", "Destroy her.", translated));
  EXPECT_TRUE(load("/Books/Story.epub", "https://api.example/v1", "m1", "Destroy him.", translated));
}

TEST_F(AiTranslationCacheTest, CorruptPayloadIsNeverReturned) {
  ASSERT_TRUE(save("/a.epub", "https://api.example/v1", "m",
                   "Test sentence.", "Тестовое предложение."));
  std::filesystem::path entry;
  for (const auto& f : std::filesystem::recursive_directory_iterator(HalStorage::root())) {
    if (f.path().extension() == ".bin") { entry = f.path(); break; }
  }
  ASSERT_FALSE(entry.empty());
  {
    std::fstream out(entry, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(-1, std::ios::end);
    out.put('X');
  }
  std::string translated;
  EXPECT_FALSE(load("/a.epub", "https://api.example/v1", "m", "Test sentence.", translated));
  EXPECT_TRUE(translated.empty());
}

TEST_F(AiTranslationCacheTest, RejectsOversizedInputAndDoesNotWrite) {
  EXPECT_FALSE(save("/a.epub", "https://api.example/v1", "m",
                    std::string(2049, 'a'), "Тест."));
  EXPECT_FALSE(Storage.exists("/.crosspoint/ai-cache"));
}
