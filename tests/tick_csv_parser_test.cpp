#include "legacy/tick_csv_parser.h"

#include <cassert>
#include <stdexcept>
#include <string>

namespace
{
void expect_invalid(const std::string& row)
{
    try
    {
        static_cast<void>(legacy::tick_csv::parse_row(row));
        assert(false && "expected parse_row to reject malformed CSV input");
    }
    catch (const std::invalid_argument&)
    {
    }
}
}  // namespace

int main()
{
    const legacy::Tick tick = legacy::tick_csv::parse_row("42,EURUSD,1.2345,100,b");
    assert(tick.ts == 42);
    assert(tick.symbol == "EURUSD");
    assert(tick.price == 1.2345);
    assert(tick.size == 100);
    assert(tick.side == Side::Buy);

    assert(legacy::tick_csv::parse_row("43,EURUSD,1.2346,200,").side == Side::Unknown);
    assert(legacy::tick_csv::parse_row("43,EURUSD,1.2346,200").side == Side::Unknown);
    expect_invalid("43,EURUSD,not-a-price,200,B");
    expect_invalid("43,EURUSD,1.2346");
    assert(legacy::tick_csv::parse_row("43,EURUSD,1.2346,200,B,extra").side == Side::Buy);
}