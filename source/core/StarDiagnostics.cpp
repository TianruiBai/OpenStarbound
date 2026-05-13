#include "StarDiagnostics.hpp"
#include "StarFile.hpp"
#include "StarLogging.hpp"
#include "StarTime.hpp"
#include "StarVersion.hpp"

#include <regex>

namespace Star::Diagnostics {

namespace {

std::string replaceRegex(std::string text, char const* pattern, char const* replacement) {
  try {
    return std::regex_replace(text, std::regex(pattern, std::regex_constants::icase), replacement);
  } catch (std::regex_error const&) {
    return text;
  }
}

}

String redactSensitiveText(String const& value) {
  auto text = value.utf8();
  text = replaceRegex(std::move(text), R"star(((?:"?(?:password|passwd|rconServerPassword|token|authToken|accessToken|secret|authorization)"?\s*[:=]\s*)(?:Bearer\s+)?)(?:"[^"\r\n]*"|[^\s,}\]\r\n]+))star", "$1\"<redacted>\"");
  text = replaceRegex(std::move(text), R"star(\b(?:\d{1,3}\.){3}\d{1,3}\b)star", "<redacted-ipv4>");
  text = replaceRegex(std::move(text), R"star(([A-Za-z]:\\Users\\)[^\\\s]+)star", "$1<redacted>");
  text = replaceRegex(std::move(text), R"star((/(?:home|Users)/)[^/\s]+)star", "$1<redacted>");
  return String(std::move(text));
}

Json crashReportJson(CrashReport const& report) {
  JsonArray recentLogs;
  for (auto const& message : Logger::recentLogMessages()) {
    recentLogs.append(JsonObject{
        {"timestamp", message.timestamp},
        {"level", LogLevelNames.getRight(message.level)},
        {"message", redactSensitiveText(message.message)}});
  }

  JsonObject exception{
      {"type", report.type},
      {"message", redactSensitiveText(report.message)}};
  if (!report.caughtAt.empty())
    exception["caughtAt"] = redactSensitiveText(report.caughtAt);

  return JsonObject{
      {"formatVersion", 1},
      {"createdAt", Time::printCurrentDateAndTime()},
      {"createdAtMilliseconds", Time::millisecondsSinceEpoch()},
      {"version", JsonObject{
          {"openstarbound", OpenStarVersionString},
          {"releaseName", OpenStarReleaseNameString},
          {"starbound", StarVersionString},
          {"source", StarSourceIdentifierString},
          {"architecture", StarArchitectureString}}},
      {"exception", std::move(exception)},
      {"recentLogs", std::move(recentLogs)}};
}

Maybe<String> writeCrashReport(CrashReport const& report) {
  return writeCrashReport(File::relativeTo(File::currentDirectory(), "crashes"), report);
}

Maybe<String> writeCrashReport(String const& directory, CrashReport const& report) {
  try {
    File::makeDirectoryRecursive(directory);

    auto timestamp = Time::millisecondsSinceEpoch();
    auto reportText = crashReportJson(report).printJson(2, true);
    for (unsigned i = 0; i < 1000; ++i) {
      auto path = File::relativeTo(directory, strf("crash-{}-{}.json", timestamp, i));
      if (!File::exists(path)) {
        File::writeFile(reportText, path);
        return path;
      }
    }
  } catch (std::exception const&) {
  }

  return {};
}

}