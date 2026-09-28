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

    void set_enabled(bool enabled)
    {
        enabled_ = enabled;
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
    bool enabled_{true};
};
