#pragma once
#include <string>

#include "legacy/tick.h"

class Logger;

class CsvFeed
{
   public:
        CsvFeed(const std::string& path, legacy::TickCallback cb, int ms_delay = 0,
            Logger* logger = nullptr);
    void run();

   private:
    std::string path_;
    legacy::TickCallback cb_;
    int ms_delay_;
    Logger* logger_;
};
