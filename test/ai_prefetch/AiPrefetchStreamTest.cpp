#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "AiPrefetchStream.h"

TEST(AiPrefetchStream, SplitsSseAcrossArbitraryTcpChunksAndStopsAtDone) {
  char events[512] = {};
  char objects[2048] = {};
  ai_prefetch::StreamFramer framer(events, sizeof(events), objects, sizeof(objects));
  const std::string wire =
      "event: message\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"ignored\"}}]}\r\n\r\n"
      "data: [DONE]\n\n";
  std::vector<std::string> data;
  for (char c : wire) {
    ASSERT_TRUE(framer.feedSse(&c, 1, [&](std::string_view s) { data.emplace_back(s); }));
  }
  ASSERT_EQ(data.size(), 1u);
  EXPECT_EQ(data.front(), "{\"choices\":[{\"delta\":{\"content\":\"ignored\"}}]}");
  EXPECT_TRUE(framer.finished());
}

TEST(AiPrefetchStream, CompletesFirstSentenceWithoutWaitingForSecond) {
  char events[128] = {};
  char objects[2048] = {};
  ai_prefetch::StreamFramer framer(events, sizeof(events), objects, sizeof(objects));
  std::vector<std::string> records;
  const auto save = [&](std::string_view record) { records.emplace_back(record); };
  const std::string first =
      "{\"id\":0,\"translation\":\"Уничтожь {его}.\",\"words\":[{\"i\":0,\"translation\":\"Да\\\\\\\"\"}]}";
  const std::string second = "{\"id\":1,\"translation\":\"Здравствуй.\",\"words\":[]}";
  for (char ch : first) ASSERT_TRUE(framer.feedModel(&ch, 1, save));
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records.front(), first);
  EXPECT_FALSE(framer.hasIncompleteObject());

  const std::string partial = second.substr(0, second.size() / 2);
  ASSERT_TRUE(framer.feedModel(partial.data(), partial.size(), save));
  EXPECT_EQ(records.size(), 1u);  // First record already durable.
  EXPECT_TRUE(framer.hasIncompleteObject());
  EXPECT_TRUE(framer.feedModel(second.data() + partial.size(), second.size() - partial.size(), save));
  EXPECT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1], second);
}

TEST(AiPrefetchStream, RejectsOversizedDataOrObjects) {
  char events[64] = {};
  char objects[64] = {};
  ai_prefetch::StreamFramer framer(events, sizeof(events), objects, sizeof(objects));
  std::string huge(80, 'a');
  EXPECT_FALSE(framer.feedSse(huge.data(), huge.size(), [](std::string_view) {}));
  const std::string json = "{\"text\":\"" + huge + "\"}";
  EXPECT_FALSE(framer.feedModel(json.data(), json.size(), [](std::string_view) {}));
}

TEST(AiPrefetchStream, SkipsMarkdownAndRespectsEscapes) {
  char events[64] = {};
  char objects[4096] = {};
  ai_prefetch::StreamFramer framer(events, sizeof(events), objects, sizeof(objects));
  std::vector<std::string> data;
  const std::string response = "begin\n\`\`\`json\n{\"id\":0,\"translation\":\"quote: \\\\\" } \"}\n\`\`\`";
  EXPECT_TRUE(framer.feedModel(response.data(), response.size(),
                               [&](std::string_view record) { data.emplace_back(record); }));
  EXPECT_EQ(data.size(), 1u);
}
