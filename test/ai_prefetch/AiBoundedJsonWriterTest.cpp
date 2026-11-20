#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "AiBoundedJsonWriter.h"

TEST(AiBoundedJsonWriter, EscapesControlCharactersAndQuotes) {
  char storage[256] = {};
  ai_prefetch::BoundedJsonWriter writer(storage, sizeof(storage));
  ASSERT_TRUE(writer.append("{\"text\":"));
  ASSERT_TRUE(writer.quoted(std::string_view("quoted \" word\\\n\t")));
  ASSERT_TRUE(writer.append("}"));
  EXPECT_EQ(writer.view(), "{\"text\":\"quoted \\\" word\\\\\\n\\t\"}");
}

TEST(AiBoundedJsonWriter, KeepsUtf8UnchangedAndNumbers) {
  char storage[256] = {};
  ai_prefetch::BoundedJsonWriter writer(storage, sizeof(storage));
  EXPECT_TRUE(writer.quoted("Привет"));
  EXPECT_TRUE(writer.append(" "));
  EXPECT_TRUE(writer.integer(1234567890u));
  EXPECT_EQ(writer.view(), "\"Привет\" 1234567890");
}

TEST(AiBoundedJsonWriter, FailsClosedOnCapacityLimits) {
  char storage[8] = {};
  ai_prefetch::BoundedJsonWriter writer(storage, sizeof(storage));
  EXPECT_FALSE(writer.quoted("12345678"));
  EXPECT_LE(writer.size(), sizeof(storage));
  EXPECT_FALSE(writer.append("xxxxxxxxxxxxxxxxxxxx"));
}
