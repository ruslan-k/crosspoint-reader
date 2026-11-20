#include <gtest/gtest.h>
#include <HalStorage.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "AiPrefetchCache.h"

using namespace ai_prefetch;

class AiPrefetchCacheTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
  void TearDown() override { Storage.reset(); }
  static Scope scope() { return {"/Books/Test.epub", "https://api.example/v1", "model-name"}; }
  static Sentence record() {
    Sentence s;
    s.spine = 2;
    s.start = 100;
    s.end = 116;
    s.source = "Destroy him.";
    s.translation = "Уничтожь его.";
    s.words.push_back({100, "Destroy", "Уничтожь"});
    s.words.push_back({108, "him.", "его."});
    return s;
  }
};

TEST_F(AiPrefetchCacheTest, WindowAndBatchBound) {
  EXPECT_EQ(WINDOW_SENTENCES, 100u);
  EXPECT_EQ(BATCH_SENTENCES, 4u);
}

TEST_F(AiPrefetchCacheTest, ExactHitAndWordOffsets) {
  auto s = record();
  ASSERT_TRUE(save(scope(), s));
  EXPECT_TRUE(has(scope(), s));
  std::string word, sentence;
  EXPECT_TRUE(lookup(scope(), s, 100, "Destroy", word, sentence));
  EXPECT_EQ(word, "Уничтожь");
  EXPECT_EQ(sentence, "Уничтожь его.");
  EXPECT_FALSE(lookup(scope(), s, 100, "him.", word, sentence));
  EXPECT_TRUE(lookup(scope(), s, 108, "him.", word, sentence));
  EXPECT_EQ(word, "его.");
}

TEST_F(AiPrefetchCacheTest, WrongBookModelSpineAndOffsetMiss) {
  auto s = record();
  ASSERT_TRUE(save(scope(), s));
  auto other = scope();
  other.model = "other-model";
  EXPECT_FALSE(has(other, s));
  other = scope();
  other.book = "/Books/Other.epub";
  EXPECT_FALSE(has(other, s));
  s.start += 25;
  s.end += 25;
  EXPECT_FALSE(has(scope(), s));
  s = record();
  ++s.spine;
  EXPECT_FALSE(has(scope(), s));
  s = record();
  s.source = "Destroy her.";
  EXPECT_FALSE(has(scope(), s));
}

TEST_F(AiPrefetchCacheTest, CorruptionFailsClosed) {
  auto s = record();
  ASSERT_TRUE(save(scope(), s));
  std::filesystem::path item;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(HalStorage::root())) {
    if (entry.path().extension() == ".bin") { item = entry.path(); break; }
  }
  ASSERT_FALSE(item.empty());
  {
    std::fstream file(item, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(-1, std::ios::end);
    file.put('X');
  }
  EXPECT_FALSE(has(scope(), s));
}

TEST_F(AiPrefetchCacheTest, DurableCheckpointIsOnlyAdvisory) {
  auto s = record();
  ASSERT_TRUE(save(scope(), s));
  Checkpoint c{2, 99, 116, 4};
  ASSERT_TRUE(saveCheckpoint(scope(), c));
  Checkpoint loaded;
  ASSERT_TRUE(loadCheckpoint(scope(), loaded));
  EXPECT_EQ(loaded.spine, 2u);
  EXPECT_EQ(loaded.anchor, 99u);
  EXPECT_EQ(loaded.nextOffset, 116u);
  EXPECT_EQ(loaded.scanned, 4u);
  for (const auto& entry : std::filesystem::recursive_directory_iterator(HalStorage::root())) {
    if (entry.path().filename() == "checkpoint.bin") {
      std::fstream file(entry.path(), std::ios::binary | std::ios::in | std::ios::out);
      file.seekp(0);
      file.put('X');
    }
  }
  EXPECT_FALSE(loadCheckpoint(scope(), loaded));
  EXPECT_TRUE(has(scope(), s));
}

TEST_F(AiPrefetchCacheTest, InvalidEntriesDoNotGetWritten) {
  auto s = record();
  s.words[1].translation.clear();
  EXPECT_FALSE(save(scope(), s));
  EXPECT_FALSE(has(scope(), s));
  s = record();
  s.source.resize(MAX_SENTENCE_BYTES + 1, 'A');
  EXPECT_FALSE(save(scope(), s));
}
