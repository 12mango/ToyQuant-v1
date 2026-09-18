#pragma once
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

class Logger
{
   public:
    explicit Logger(const std::string& file = "");

    void set_verbose(bool enabled)
    {
        verbose_ = enabled;
    }

    template <typename... Args>
    void log(Args&&... args)
    {
        write(format(std::forward<Args>(args)...), std::cout);
    }

    template <typename... Args>
    void error(Args&&... args)
    {
        write(format(std::forward<Args>(args)...), std::cerr);
    }

    template <typename... Args>
    void debug(Args&&... args)
    {
        if (verbose_) write(format(std::forward<Args>(args)...), std::cout);
    }

   private:
    template <typename... Args>
    static std::string format(Args&&... args)
    {
        std::ostringstream stream;
        (stream << ... << args);
        return stream.str();
    }

    void write(const std::string& message, std::ostream& console);
    std::ofstream fout_;
    bool verbose_{false};
};
