#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

/*
-------------------------------------------
 LineReader
-------------------------------------------

Replaces std::getline / gzgets on the market-data hot path. Both of those
APIs hand back a std::string and walk the input one byte at a time, which
costs hundreds of cycles per ~56 byte L2 row. This reader instead pulls
whole blocks from the underlying source and locates line boundaries with
memchr, which is SIMD vectorised in libc.

The common case therefore needs one vectorised scan and no allocation at
all: the returned view points straight into the block. A line that crosses
a block boundary is compacted to the front of the block before the next
read, so an owned copy is only made for a line larger than one block.

Lifetime contract: the returned view stays valid until the next call to
next_line(). Callers that must hold a row across calls have to copy it
into their own buffer.
-------------------------------------------
*/

class LineReader
{
   public:
    static constexpr std::size_t kDefaultBlockSize = std::size_t{1} << 20;  // 1 MiB

    // Opens the path. A trailing ".gz" selects the zlib backend, matching how the
    // existing market data readers choose their transport.
    explicit LineReader(const std::string& path, std::size_t block_size = kDefaultBlockSize);
    ~LineReader();

    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;

    // On success `line` views the internal block (or the growth buffer when a single
    // line exceeds the block) with the trailing "\n" and optional "\r" removed.
    // Returns false once the input is exhausted.
    bool next_line(std::string_view& line);

    // Number of successful block refills, i.e. read()/gzread() calls issued. Exposed so
    // tests can assert block level batching and the profiler can report read overhead.
    std::uint64_t block_reads() const
    {
        return block_reads_;
    }

   private:
    // Returns the bytes read, or 0 at end of input. Throws on a hard I/O error.
    std::ptrdiff_t read_block(char* destination, std::size_t capacity);

    std::ifstream plain_;
    void* gzip_{nullptr};  // gzFile, kept as void* so zlib.h stays out of this header
    std::vector<char> block_;
    std::size_t begin_{0};
    std::size_t end_{0};
    std::uint64_t block_reads_{0};
    bool drained_{false};
};
