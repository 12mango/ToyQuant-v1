#pragma once
#include "market/market_types.h"
#include <string>

class CsvFeed {
public:
    CsvFeed(const std::string& path, TickCallback cb, int ms_delay = 0);
    void run();

private:
    std::string path_;
    TickCallback cb_;
    int ms_delay_;
};
