#include "utils/logger.h"

#include <filesystem>
#include <stdexcept>

Logger::Logger(const std::string& file)
{
    if (file.empty()) return;

    const auto parent = std::filesystem::path(file).parent_path();
    std::error_code error;
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error)
    {
        throw std::runtime_error("failed to create log directory '" + parent.string() +
                                 "': " + error.message());
    }

    fout_.open(file, std::ios::out | std::ios::trunc);
    if (!fout_.is_open()) throw std::runtime_error("failed to open log file '" + file + "'");
}

void Logger::write(const std::string& message, std::ostream& console)
{
    console << message << std::endl;
    if (fout_.is_open()) fout_ << message << std::endl;
}
