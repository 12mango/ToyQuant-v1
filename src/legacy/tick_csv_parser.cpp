#include "legacy/tick_csv_parser.h"

#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
std::string read_field(std::istringstream& stream, const char* field_name)
{
    std::string value;
    if (!std::getline(stream, value, ','))
    {
        throw std::invalid_argument(std::string("missing ") + field_name);
    }
    return value;
}
}  // namespace

namespace legacy::tick_csv
{
Tick parse_row(const std::string& row)
{
    std::istringstream stream(row);

    Tick tick;
    tick.ts = std::stoull(read_field(stream, "timestamp"));
    tick.symbol = read_field(stream, "symbol");
    tick.price = std::stod(read_field(stream, "price"));
    tick.size = std::stoull(read_field(stream, "size"));

    std::string side;
    std::getline(stream, side, ',');
    tick.side = side.empty() ? Side::Unknown : to_side(side.front());
    return tick;
}
}  // namespace legacy::tick_csv
