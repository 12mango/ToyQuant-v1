#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "common/instrument_spec.h"
#include "common/market_event.h"

class IMarketEventReader
{
   public:
    virtual ~IMarketEventReader() = default;
    virtual bool next(MarketEvent& event) = 0;
};

struct MarketDataReaders
{
    std::unique_ptr<IMarketEventReader> trades;
    std::unique_ptr<IMarketEventReader> quotes;
};

MarketDataReaders make_market_data_readers(const std::string& format,
                                           const std::string& trades_path,
                                           const std::string& quotes_path,
                                           const InstrumentSpec& instrument);

std::unique_ptr<IMarketEventReader> make_deribit_book_snapshot_reader(
    const std::string& path);

std::unique_ptr<IMarketEventReader> make_deribit_incremental_book_reader(
    const std::string& path);

std::unique_ptr<IMarketEventReader> make_deribit_depth_reader(const std::string& path);

std::unique_ptr<IMarketEventReader> make_deribit_trade_reader(const std::string& path);

// Binance: the aggregate-trade CSV and the book-ticker CSV of the public data dumps, both scaled into the
// instrument's quantity unit. Declared beside the OKX pair for the same reason: the router names them, and
// so can a test that wants one reader rather than the pair.
std::unique_ptr<IMarketEventReader> make_binance_trade_reader(const std::string& path,
                                                              const std::string& symbol,
                                                              uint64_t quantity_scale);

std::unique_ptr<IMarketEventReader> make_binance_book_reader(const std::string& path,
                                                             const std::string& symbol,
                                                             uint64_t quantity_scale);

// OKX order book JSON Lines and the traded CSV that tools/fetch_okx_trades.py produces from the archive.
std::unique_ptr<IMarketEventReader> make_okx_book_reader(const std::string& path, const std::string& symbol,
                                                         double contract_size, uint64_t quantity_scale);

std::unique_ptr<IMarketEventReader> make_okx_trade_reader(const std::string& path, const std::string& symbol,
                                                          double contract_size, uint64_t quantity_scale);

// Which reader a depth file needs, decided from the file itself: an OKX archive opens with a JSON object per
// line, and the Deribit depth files are CSV with a header.
std::string market_data_format_of(const std::string& quotes_path);
