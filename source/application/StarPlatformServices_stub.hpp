#pragma once

#include "StarStatisticsService.hpp"
#include "StarUserGeneratedContentService.hpp"
#include "StarDesktopService.hpp"

namespace Star {

// STUB: Explicit placeholder platform services used for constrained targets (e.g. Nintendo 3DS).
StatisticsServicePtr makePlaceholderStatisticsService();
UserGeneratedContentServicePtr makePlaceholderUserGeneratedContentService();
DesktopServicePtr makePlaceholderDesktopService();

}
