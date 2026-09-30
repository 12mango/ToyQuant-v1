#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "common/instrument_spec.h"
#include "market/market_event.h"

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

// OKX order book JSON Lines and the traded CSV that tools/fetch_okx_trades.py produces from the archive.
std::unique_ptr<IMarketEventReader> make_okx_book_reader(const std::string& path, const std::string& symbol,
                                                         double contract_size, uint64_t quantity_scale);

std::unique_ptr<IMarketEventReader> make_okx_trade_reader(const std::string& path, const std::string& symbol,
                                                          double contract_size, uint64_t quantity_scale);
