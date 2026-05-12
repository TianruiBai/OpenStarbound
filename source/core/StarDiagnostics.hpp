#pragma once

#include "StarJson.hpp"

namespace Star::Diagnostics {

struct CrashReport {
  String type;
  String message;
  String caughtAt;
};

String redactSensitiveText(String const& text);
Json crashReportJson(CrashReport const& report);
Maybe<String> writeCrashReport(CrashReport const& report);
Maybe<String> writeCrashReport(String const& directory, CrashReport const& report);

}