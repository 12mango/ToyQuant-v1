#pragma once
// Shared scaffolding for the tests. Two things are said here rather than in twenty-one files.
//
// The first is that `assert` is the test. NDEBUG deletes it, and this project has a preset that defines
// NDEBUG -- `linux-relwithdebinfo` -- so a test built that way is a program that checks nothing and exits
// zero. The guard belongs on the target rather than in the file, because a flag cannot be forgotten when a
// test is added, and that is what `add_toy_test()` in CMakeLists.txt does with `-UNDEBUG`. The undef below
// is the same statement for a test compiled by hand outside CMake, and it has to be here rather than in
// `<cassert>`, since what turns the macro off is whichever include runs first.
//
// The second is scratch space. Seven tests each grew their own: a raw `/tmp` path that was never removed, a
// `temp_directory_path()` path removed on the last line of `main` -- which an early `assert`, being an
// abort, walks straight past -- and a directory named after the test and cleared at both ends. A class that
// removes itself is the same amount of code where it is used and is correct on every exit path.
//
// Included as "support/test_support.h", which resolves against the directory of the including test.

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace test_support
{

inline long process_id()
{
#if defined(_WIN32)
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// The leaf is the caller's name plus the process and a counter. The name only has to be recognisable in an
// `ls`; uniqueness has to hold against a second run of the same test at the same time, which is what the
// process id is for, and against two scratch files in one test, which is what the counter is for.
//
// The unique part goes before the extension and not after it. The readers pick their parser from the last
// extension -- `.gz` decides the gzip backend, `.jsonl` the lines of order book -- so a file called
// `trades.csv.gz_1234_1` is read as a plain unreadable file, which is how this was found.
inline std::filesystem::path scratch_path(std::string_view name)
{
    static int counter = 0;
    const std::filesystem::path given{std::string{name}};
    std::string leaf{"toy_quant_"};
    leaf.append(given.stem().string());
    leaf.push_back('_');
    leaf.append(std::to_string(process_id()));
    leaf.push_back('_');
    leaf.append(std::to_string(counter++));
    leaf.append(given.extension().string());
    return std::filesystem::temp_directory_path() / leaf;
}

// A scratch file, removed when it leaves scope: on the normal return and on the abort an `assert` takes.
class TempFile
{
   public:
    explicit TempFile(std::string_view name) : path_(scratch_path(name)) {}
    ~TempFile() { remove(); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::filesystem::path& path() const { return path_; }

    // Most inputs are one string, so this is the open/truncate/write/close sequence they would otherwise
    // spell out. Tests that stream their input -- a row per iteration -- call `open()` and keep the stream.
    void write(std::string_view contents) const
    {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << contents;
        assert(out.good() && "a scratch file the test cannot write is the test's failure, not the code's");
    }

    std::ofstream open() const { return std::ofstream(path_, std::ios::binary | std::ios::trunc); }

    void remove()
    {
        if (path_.empty())
        {
            return;
        }
        std::error_code error;
        std::filesystem::remove(path_, error);
        path_.clear();
    }

   private:
    std::filesystem::path path_;
};

// The tests whose input is a set of files -- a trades file and a depth file, or that pair per symbol -- want
// a directory rather than one name, and want it gone for the same reason.
class TempDir
{
   public:
    explicit TempDir(std::string_view name) : path_(scratch_path(name))
    {
        std::error_code error;
        std::filesystem::create_directories(path_, error);
    }
    ~TempDir()
    {
        if (path_.empty())
        {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        path_.clear();
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    // `root / "trades.csv"` reads the way the call sites already did, so a test barely changes shape when
    // its directory becomes one that removes itself.
    std::filesystem::path operator/(std::string_view leaf) const { return path_ / std::string(leaf); }

   private:
    std::filesystem::path path_;
};

}  // namespace test_support
