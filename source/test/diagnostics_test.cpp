#include "StarAlgorithm.hpp"
#include "StarDiagnostics.hpp"
#include "StarFile.hpp"
#include "StarLogging.hpp"

#include "gtest/gtest.h"

using namespace Star;

TEST(DiagnosticsTest, RedactsSensitiveText) {
  auto redacted = Diagnostics::redactSensitiveText(
      "password=hunter2 token: abc Authorization: Bearer xyz 192.168.1.20 C:\\Users\\Tass\\OpenStarbound /home/tass/openstarbound");

  EXPECT_FALSE(redacted.contains("hunter2"));
  EXPECT_FALSE(redacted.contains("abc"));
  EXPECT_FALSE(redacted.contains("xyz"));
  EXPECT_FALSE(redacted.contains("192.168.1.20"));
  EXPECT_FALSE(redacted.contains("Tass"));
  EXPECT_FALSE(redacted.contains("tass"));
  EXPECT_TRUE(redacted.contains("<redacted>"));
  EXPECT_TRUE(redacted.contains("<redacted-ipv4>"));
}

TEST(DiagnosticsTest, WritesCrashReportJson) {
  auto previousLimit = Logger::recentLogLimit();
  auto directory = File::temporaryDirectory();
  auto finallyGuard = finally([&directory]() { File::removeDirectoryRecursive(directory); });

  Logger::clearRecentLogMessages();
  Logger::setRecentLogLimit(4);
  Logger::info("before crash password=hunter2 from 10.0.0.5");

  auto path = Diagnostics::writeCrashReport(directory, {"unitTest", "crashed with token=abc", "caught at C:\\Users\\Tass\\OpenStarbound"});
  ASSERT_TRUE(path);

  auto reportText = File::readFileString(*path);
  EXPECT_FALSE(reportText.contains("hunter2"));
  EXPECT_FALSE(reportText.contains("10.0.0.5"));
  EXPECT_FALSE(reportText.contains("token=abc"));
  EXPECT_FALSE(reportText.contains("Tass"));

  auto report = Json::parseJson(reportText);
  EXPECT_EQ(report.getInt("formatVersion"), 1);
  EXPECT_EQ(report.get("exception").getString("type"), "unitTest");
  EXPECT_FALSE(report.get("version").getString("openstarbound").empty());
  EXPECT_EQ(report.getArray("recentLogs").size(), 1u);

  Logger::setRecentLogLimit(previousLimit);
  Logger::clearRecentLogMessages();
}