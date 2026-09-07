#pragma once

#include <string>

#include "common/types.h"

namespace tick_csv
{
Tick parse_row(const std::string& row);
}