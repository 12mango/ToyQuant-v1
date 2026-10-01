// The router between the feed families. Each family's readers and factories live in the file beside this
// one -- binance_readers.cpp, deribit_readers.cpp, okx_readers.cpp -- so this file holds only the decisions
// a run's command line implies: which format the quotes file is, and for Deribit, which of the two depth
// shapes, decided by the header rather than by a flag.
//
// Nothing here parses anything. The point of the split is that a reader of this file has to hold one thing
// in mind -- the sequence of decisions -- and a reader of a family's file has to hold that family's row
// shapes, rather than a thousand lines of both.

#include "market/market_data_adapter.h"

#include <stdexcept>
#include <string>
#include <string_view>

#include "market/line_reader.h"

MarketDataReaders make_market_data_readers(const std::string& format,
                                           const std::string& trades_path,
                                           const std::string& quotes_path,
                                           const InstrumentSpec& instrument)
{
    if (format == "okx")
        return MarketDataReaders{
            make_okx_trade_reader(trades_path, instrument.symbol, instrument.contract_size,
                                  instrument.quantity_scale),
            make_okx_book_reader(quotes_path, instrument.symbol, instrument.contract_size,
                                 instrument.quantity_scale)};
    if (format != "binance")
        throw std::invalid_argument("unsupported market data format: " + format);
    // The symbol and the scale are checked by the Binance factories, which is where those checks lived
    // before the split: a run with no symbol is an error in either family rather than a default.
    return MarketDataReaders{
        make_binance_trade_reader(trades_path, instrument.symbol, instrument.quantity_scale),
        make_binance_book_reader(quotes_path, instrument.symbol, instrument.quantity_scale)};
}

std::string market_data_format_of(const std::string& quotes_path)
{
    LineReader probe(quotes_path);
    std::string_view first;
    if (!probe.next_line(first))
        throw std::invalid_argument("market data file is empty: " + quotes_path);
    if (!first.empty() && first.front() == '{' &&
        first.find("\"instId\"") != std::string_view::npos)
        return "okx";
    return "binance";
}
