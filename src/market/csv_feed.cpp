#include "market/csv_feed.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#include "legacy/tick_csv_parser.h"
#include "utils/logger.h"

CsvFeed::CsvFeed(const std::string& path, legacy::TickCallback cb, int ms_delay, Logger* logger)
    : path_(path), cb_(cb), ms_delay_(ms_delay), logger_(logger)
{
}

void CsvFeed::run()
{
    std::ifstream ifs(path_);
    if (!ifs.is_open())
    {
        if (logger_)
            logger_->error("CsvFeed: failed to open file ", path_);
        else
            std::cerr << "CsvFeed: failed to open file " << path_ << std::endl;
        return;
    }

    std::string line;
    while (std::getline(ifs, line))
    {
        if (line.empty()) continue;

        try
        {
            const auto first_separator = line.find(',');
            const std::string_view first_field = std::string_view(line).substr(0, first_separator);
            if (first_field == "ts" || first_field == "timestamp")
            {
                continue;
            }
            const legacy::Tick t = legacy::tick_csv::parse_row(line);

            // Dispatch the parsed tick to the consumer.
            cb_(t);

            // Optional playback delay.
            if (ms_delay_ > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms_delay_));
            }
        }
        catch (const std::exception& e)
        {
            if (logger_)
                logger_->error("CsvFeed parse error on line: ", line, " , exception: ", e.what());
            else
                std::cerr << "CsvFeed parse error on line: " << line << " , exception: " << e.what()
                          << std::endl;
            continue;  // Skip malformed rows.
        }
    }
}
