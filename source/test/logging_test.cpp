#include "StarLogging.hpp"

#include "gtest/gtest.h"

using namespace Star;

TEST(LoggingTest, RecentLogRing) {
  auto previousLimit = Logger::recentLogLimit();

  Logger::clearRecentLogMessages();
  Logger::setRecentLogLimit(2);

  Logger::info("recent one");
  Logger::warn("recent {}", 2);
  Logger::error("recent three");

  auto messages = Logger::recentLogMessages();
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].level, LogLevel::Warn);
  EXPECT_EQ(messages[0].message, "recent 2");
  EXPECT_EQ(messages[1].level, LogLevel::Error);
  EXPECT_EQ(messages[1].message, "recent three");
  EXPECT_GT(messages[1].timestamp, 0);

  Logger::setRecentLogLimit(0);
  EXPECT_TRUE(Logger::recentLogMessages().empty());
  Logger::info("recent hidden");
  EXPECT_TRUE(Logger::recentLogMessages().empty());

  Logger::setRecentLogLimit(previousLimit);
  Logger::clearRecentLogMessages();
}
