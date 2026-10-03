#pragma once

#include <string>

// The funding **types** are core and live in `common/funding.h`. This is the reader beside them, not a shim: the two
// functions below are what knows a venue's file format.
#include "common/funding.h"

// Which reader a funding file needs, decided from its own header rather than from its name:
//   * OKX daily archive: `instrument_name,funding_rate,funding_time`, every swap on the venue in one file
//   * Binance Vision monthly archive: `calc_time,funding_interval_hours,last_funding_rate`, one symbol
// Returns "okx" or "binance", and throws std::invalid_argument for anything else.
std::string funding_rate_format_of(const std::string& path);

// Reads one instrument's funding history. A plain or gzipped CSV, through the same LineReader every other reader
// uses. The symbol is required for the OKX file, which carries the whole venue; the Binance file is one symbol
// already and the argument then only labels the series.
FundingRateSeries read_funding_rate_series(const std::string& path, const std::string& symbol);

