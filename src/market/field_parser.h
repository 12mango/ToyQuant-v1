#pragma once

// The field parsers every reader in this directory shares. A row of a market data file is a handful of
// comma separated fields, and these turn the ones that are numbers -- and the ones that are booleans --
// into values, accepting exactly what the shapes the venues publish contain.
//
// A header, with every function in it `inline`, for a measured reason: these run once per field per row and
// they were inlined into the readers' row loops when all of this was one translation unit. Moving them
// into a .cpp of their own would have made them out-of-line calls on the hot path, which is a performance
// change and not a refactor, so the split that made one file per feed family put them here instead. The
// cold path is still a separate function, so the loops do not carry the exception machinery.
//
// The set is what more than one family uses: the row splitter, the two number conversions, the two
// quantity conversions a feed's column shape needs, and the boolean spellings two venues publish.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace csv_fields
{
// The cold path is a separate function so the hot loops that call these are not carrying the exception
// machinery, the std::string construction and the throw as inlined code on every row.
[[noreturn]] inline void throw_invalid_uint(std::string_view value)
{
    throw std::invalid_argument("invalid unsigned integer: " + std::string(value));
}

inline uint64_t parse_uint64(std::string_view value)
{
    // Timestamps and sizes in this feed are well under 19 digits, so the two-digit step below needs no
    // overflow check: a value of at most 18 digits cannot exceed 2^64-1. For that shape the loop
    // validates every character it consumes, so it accepts exactly what std::from_chars accepts and
    // produces the same value. Anything longer — or empty — goes through the general parser, which
    // keeps the previous behaviour for lengths that could overflow.
    constexpr std::size_t kMaxFastDigits = 18;
    const char* p = value.data();
    const char* const end = p + value.size();
    const std::size_t length = value.size();
    if (length == 0 || length > kMaxFastDigits)
    {
        uint64_t general = 0;
        const auto parsed = std::from_chars(p, end, general);
        if (parsed.ec != std::errc{} || parsed.ptr != end) throw_invalid_uint(value);
        return general;
    }

    uint64_t result = 0;
    if ((length & 1U) != 0)
    {
        const unsigned digit = static_cast<unsigned>(*p) - '0';
        if (digit > 9) throw_invalid_uint(value);
        result = digit;
        ++p;
    }
    while (p != end)
    {
        const unsigned high = static_cast<unsigned>(p[0]) - '0';
        const unsigned low = static_cast<unsigned>(p[1]) - '0';
        if (high > 9 || low > 9) throw_invalid_uint(value);
        result = result * 100 + high * 10 + low;
        p += 2;
    }
    return result;
}

inline double parse_double_slow(std::string_view value)
{
    double result = 0.0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result, std::chars_format::general);
    if (parsed.ec == std::errc{} && parsed.ptr == end) return result;
    return std::stod(std::string(value));
}

// Prices in this feed are plain decimals, and the book works in integer ticks, so the conversion can
// be done with integer arithmetic instead of the general purpose parser. std::from_chars<double> is
// defined out of line in libstdc++, which makes it a real call per row; the loops below avoid it for
// the shape the feed actually uses.
//
// The fast path only accepts what it can reproduce exactly: an optional sign, at least one integer
// digit, an optional fraction of at most five digits, and a mantissa that a double holds exactly. For
// that shape the value is mantissa / 10^decimals, and because both operands are exact and IEEE
// division is correctly rounded, the result is the correctly rounded value of the exact decimal,
// which is what std::from_chars produces. Everything else falls back to the general parser, so the
// returned value is identical for every input.
inline double parse_double(std::string_view value)
{
    constexpr std::int64_t kMaxExactMantissa = (std::int64_t{1} << 53) - 1;
    constexpr int kMaxFastDecimals = 5;
    static constexpr double kPow10[] = {1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0};

    std::int64_t mantissa = 0;
    int decimals = 0;
    std::size_t index = 0;
    bool negative = false;
    if (index < value.size() && (value[index] == '-' || value[index] == '+'))
    {
        negative = value[index] == '-';
        ++index;
    }
    const std::size_t digits_begin = index;
    while (index < value.size() && value[index] >= '0' && value[index] <= '9')
    {
        mantissa = mantissa * 10 + (value[index] - '0');
        if (mantissa > kMaxExactMantissa) return parse_double_slow(value);
        ++index;
    }
    if (index == digits_begin) return parse_double_slow(value);
    if (index < value.size() && value[index] == '.')
    {
        ++index;
        const std::size_t fraction_begin = index;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9')
        {
            if (decimals == kMaxFastDecimals) return parse_double_slow(value);
            mantissa = mantissa * 10 + (value[index] - '0');
            if (mantissa > kMaxExactMantissa) return parse_double_slow(value);
            ++decimals;
            ++index;
        }
        // A trailing '.' is accepted by std::stod but not by the integer path, so let it fall back.
        if (index == fraction_begin) return parse_double_slow(value);
    }
    // Anything left over (an exponent, a stray character) is not this fast path's business.
    if (index != value.size()) return parse_double_slow(value);

    const double magnitude = static_cast<double>(mantissa) / kPow10[decimals];
    return negative ? -magnitude : magnitude;
}

inline void split_csv_views(std::string_view row, std::vector<std::string_view>& fields,
                     std::size_t expected_fields)
{
    fields.clear();
    fields.reserve(expected_fields);
    std::size_t field_start = 0;
    while (field_start < row.size())
    {
        const std::size_t delimiter = row.find(',', field_start);
        std::size_t field_end = delimiter == std::string_view::npos ? row.size() : delimiter;
        if (field_end > field_start && row[field_end - 1] == '\r') --field_end;
        std::size_t value_start = field_start;
        if (fields.empty() && field_end - value_start >= 3 &&
            static_cast<unsigned char>(row[value_start]) == 0xEF &&
            static_cast<unsigned char>(row[value_start + 1]) == 0xBB &&
            static_cast<unsigned char>(row[value_start + 2]) == 0xBF)
            value_start += 3;
        fields.emplace_back(row.data() + value_start, field_end - value_start);
        if (delimiter == std::string_view::npos) break;
        field_start = delimiter + 1;
    }
}

inline uint64_t scaled_quantity(std::string_view value, uint64_t scale)
{
    const double quantity = parse_double(value);
    if (!std::isfinite(quantity) || quantity < 0.0)
        throw std::invalid_argument("quantity must be finite and non-negative");
    return static_cast<uint64_t>(std::llround(quantity * static_cast<double>(scale)));
}

inline uint64_t snapshot_quantity(std::string_view value)
{
    const double quantity = parse_double(value);
    if (!std::isfinite(quantity) || quantity < 0.0 ||
        quantity > static_cast<double>(std::numeric_limits<uint64_t>::max()))
        throw std::invalid_argument("snapshot quantity must be finite and non-negative");
    return static_cast<uint64_t>(std::llround(quantity));
}

inline bool parse_boolean(std::string_view value)
{
    // First character dispatch. It runs once per row, and the set of accepted spellings is
    // identical to the chain it replaces: only the order of the comparisons changes. A first
    // character of 'f' goes straight to the false spellings instead of testing the true ones
    // first.
    if (!value.empty())
    {
        switch (value.front())
        {
            case 't':
            case 'T':
                if (value == "true" || value == "TRUE") return true;
                break;
            case 'f':
            case 'F':
                if (value == "false" || value == "FALSE") return false;
                break;
            case '1':
                if (value == "1") return true;
                break;
            case '0':
                if (value == "0") return false;
                break;
            default:
                break;
        }
    }

    std::string normalized(value);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                    [](unsigned char character)
                                    { return std::isspace(character) || character == '"'; }),
                     normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (normalized == "true" || normalized == "1") return true;
    if (normalized == "false" || normalized == "0") return false;
    throw std::invalid_argument("invalid boolean value: " + std::string(value));
}
}  // namespace csv_fields
