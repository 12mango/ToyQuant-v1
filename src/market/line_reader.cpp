#include "market/line_reader.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>

#include <zlib.h>

namespace
{
// Guards against a caller supplying a block so small that the reader spends its time in
// read() rather than scanning lines. Tests use the floor to exercise straddling lines.
constexpr std::size_t kMinBlockSize = 64;

bool has_gzip_suffix(const std::string& path)
{
    return path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
}
}  // namespace

LineReader::LineReader(const std::string& path, std::size_t block_size)
    : block_(std::max(block_size, kMinBlockSize))
{
    if (has_gzip_suffix(path))
    {
        gzFile input = gzopen(path.c_str(), "rb");
        if (input == nullptr)
            throw std::runtime_error("failed to open gzip input file: " + path);
        gzip_ = input;
    }
    else
    {
        plain_.open(path);
        if (!plain_) throw std::runtime_error("failed to open input file: " + path);
    }
}

LineReader::~LineReader()
{
    if (gzip_ != nullptr) gzclose(static_cast<gzFile>(gzip_));
}

std::ptrdiff_t LineReader::read_block(char* destination, std::size_t capacity)
{
    if (gzip_ != nullptr)
    {
        const auto request = static_cast<unsigned>(
            std::min<std::size_t>(capacity, static_cast<std::size_t>(INT_MAX)));
        const int read = gzread(static_cast<gzFile>(gzip_), destination, request);
        if (read < 0) throw std::runtime_error("failed to read gzip input");
        return static_cast<std::ptrdiff_t>(read);
    }

    plain_.read(destination, static_cast<std::streamsize>(capacity));
    const auto read = static_cast<std::ptrdiff_t>(plain_.gcount());
    if (read == 0 && plain_.bad()) throw std::runtime_error("failed to read input file");
    return read;
}

bool LineReader::next_line(std::string_view& line)
{
    for (;;)
    {
        char* const base = block_.data();

        if (begin_ < end_)
        {
            const void* const found = std::memchr(base + begin_, '\n', end_ - begin_);
            if (found != nullptr)
            {
                const std::size_t newline_offset =
                    static_cast<std::size_t>(static_cast<const char*>(found) - base);
                const std::size_t line_begin = begin_;
                begin_ = newline_offset + 1;
                std::size_t length = newline_offset - line_begin;
                if (length > 0 && base[newline_offset - 1] == '\r') --length;
                line = std::string_view(base + line_begin, length);
                return true;
            }
        }

        if (drained_)
        {
            // Final line of the input, only reached when it has no trailing newline.
            if (begin_ >= end_) return false;
            const std::size_t line_begin = begin_;
            begin_ = end_;
            std::size_t length = end_ - line_begin;
            if (length > 0 && base[end_ - 1] == '\r') --length;
            line = std::string_view(base + line_begin, length);
            return true;
        }

        // No newline left in the buffer: compact the tail to the front so the pending
        // partial line stays contiguous with the bytes read next, then refill.
        if (begin_ > 0)
        {
            std::memmove(base, base + begin_, end_ - begin_);
            end_ -= begin_;
            begin_ = 0;
        }
        if (end_ == block_.size()) block_.resize(block_.size() * 2);

        const std::ptrdiff_t read = read_block(block_.data() + end_, block_.size() - end_);
        if (read <= 0)
        {
            drained_ = true;
            continue;
        }
        end_ += static_cast<std::size_t>(read);
        ++block_reads_;
    }
}
