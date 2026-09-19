#pragma once

#include <string>

#include "legacy/tick.h"

namespace legacy::tick_csv
{
Tick parse_row(const std::string& row);
}
