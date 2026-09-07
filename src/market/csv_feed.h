#pragma once
#include <string>

#include "common/types.h"

class Logger;

class CsvFeed
{
   public:
    CsvFeed(const std::string& path, TickCallback cb, int ms_delay = 0, Logger* logger = nullptr);
    void run();

   private:
    std::string path_;
    TickCallback cb_;
    int ms_delay_;
    Logger* logger_;
};
