// The checks in this test are the test: keep them enabled even when the build defines
// NDEBUG, which is the case for the RelWithDebInfo profiling preset.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "market/line_reader.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

namespace
{
std::string mixed_payload()
{
    std::string payload;
    payload += "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n";
    // Enough rows that a 64 byte block has to straddle almost every line.
    for (int index = 0; index < 200; ++index)
    {
        payload += "deribit,BTC-PERPETUAL,15856992000";
        payload += std::to_string(100000 + index);
        payload += ",15856992000";
        payload += std::to_string(100500 + index);
        payload += ",false,bid,6421.5,10\n";
    }
    // CRLF row, empty row, and a final row without a trailing newline.
    payload += "deribit,BTC-PERPETUAL,1585699300000000,1585699300000001,false,ask,6422.0,7\r\n";
    payload += "\n";
    payload += "deribit,BTC-PERPETUAL,1585699301000000,1585699301000001,true,bid,6420.5,3";
    return payload;
}

void write_text(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream output(path, std::ios::binary);
    assert(output);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Reference semantics for the reader: split on '\n', drop one trailing '\r'.
std::vector<std::string> reference_lines(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    assert(input);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> reader_lines(const std::string& path, std::size_t block_size)
{
    LineReader reader(path, block_size);
    std::vector<std::string> lines;
    std::string_view line;
    while (reader.next_line(line)) lines.emplace_back(line);
    return lines;
}

void expect_equal(const std::vector<std::string>& actual, const std::vector<std::string>& expected,
                  const std::string& label)
{
    if (actual == expected) return;

    std::cerr << "line_reader_test mismatch in " << label << ": actual=" << actual.size()
              << " expected=" << expected.size() << "\n";
    const std::size_t common = std::min(actual.size(), expected.size());
    for (std::size_t index = 0; index < common; ++index)
    {
        if (actual[index] == expected[index]) continue;
        std::cerr << "  first difference at line " << index << "\n"
                  << "    actual   = '" << actual[index] << "'\n"
                  << "    expected = '" << expected[index] << "'\n";
        break;
    }
    assert(false);
}
}  // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path();
    const auto plain_path = root / "toy_quant_line_reader.csv";
    const auto gzip_path = root / "toy_quant_line_reader.csv.gz";

    const std::string payload = mixed_payload();
    write_text(plain_path, payload);

    const auto expected = reference_lines(plain_path.string());
    assert(expected.size() == 204);
    assert(expected.front() ==
           "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount");
    assert(expected[1] ==
           "deribit,BTC-PERPETUAL,15856992000100000,15856992000100500,false,bid,6421.5,10");
    assert(expected[200] ==
           "deribit,BTC-PERPETUAL,15856992000100199,15856992000100699,false,bid,6421.5,10");
    assert(expected[201] ==
           "deribit,BTC-PERPETUAL,1585699300000000,1585699300000001,false,ask,6422.0,7");
    assert(expected[202].empty());
    assert(expected[203] ==
           "deribit,BTC-PERPETUAL,1585699301000000,1585699301000001,true,bid,6420.5,3");
    std::cout << "line_reader_test: payload=" << payload.size()
              << " bytes, lines=" << expected.size() << "\n";

    // Default block size: the whole payload fits in a single refill.
    expect_equal(reader_lines(plain_path.string(), LineReader::kDefaultBlockSize), expected,
                 "default 1 MiB block");

    // Block sizes below the payload size force the compaction path on nearly every line.
    for (std::size_t block_size : {std::size_t{64}, std::size_t{97}, std::size_t{1024}})
        expect_equal(reader_lines(plain_path.string(), block_size), expected,
                     "block size " + std::to_string(block_size));

    // A single line larger than the block must grow the buffer instead of looping forever.
    {
        const auto long_path = root / "toy_quant_line_reader_long.csv";
        std::string long_payload(200000, 'x');
        long_payload += "\nshort\n";
        write_text(long_path, long_payload);
        expect_equal(reader_lines(long_path.string(), std::size_t{64}),
                     {std::string(200000, 'x'), "short"}, "line larger than the block");
        std::filesystem::remove(long_path);
    }

    // Block level batching: one refill, not one read per line.
    {
        LineReader reader(plain_path.string(), LineReader::kDefaultBlockSize);
        std::string_view line;
        while (reader.next_line(line))
        {
        }
        assert(reader.block_reads() == 1);
    }

    // gzip backend produces the same lines as the plain backend.
    {
        gzFile output = gzopen(gzip_path.string().c_str(), "wb");
        assert(output != nullptr);
        assert(gzwrite(output, payload.data(), static_cast<unsigned>(payload.size())) ==
               static_cast<int>(payload.size()));
        assert(gzclose(output) == Z_OK);
    }
    expect_equal(reader_lines(gzip_path.string(), LineReader::kDefaultBlockSize), expected,
                 "gzip default block");
    expect_equal(reader_lines(gzip_path.string(), std::size_t{64}), expected, "gzip 64 byte block");

    // Parity with the repository fixture that market_data_adapter_test also consumes.
    {
        const auto fixture =
            std::filesystem::path(PROJECT_ROOT_DIR) / "data/v2/test_aggTrades_5k.csv";
        if (std::filesystem::exists(fixture))
        {
            const auto fixture_expected = reference_lines(fixture.string());
            assert(fixture_expected.size() > 1000);
            expect_equal(reader_lines(fixture.string(), LineReader::kDefaultBlockSize),
                         fixture_expected, "fixture default block");
            expect_equal(reader_lines(fixture.string(), std::size_t{64}), fixture_expected,
                         "fixture 64 byte block");
            std::cout << "line_reader_test: fixture lines=" << fixture_expected.size() << "\n";
        }
        else
        {
            std::cout << "line_reader_test: fixture missing, skipped\n";
        }
    }

    std::filesystem::remove(plain_path);
    std::filesystem::remove(gzip_path);
    std::cout << "line_reader_test passed\n";
    return 0;
}
