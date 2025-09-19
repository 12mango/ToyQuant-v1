#include "market/csv_feed.h"
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

CsvFeed::CsvFeed(const std::string& path, TickCallback cb, int ms_delay)
    : path_(path), cb_(cb), ms_delay_(ms_delay) {}

void CsvFeed::run() {
    std::ifstream ifs(path_);
    if (!ifs.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string token;
        Tick t;
        if (!std::getline(ss, token, ',')) continue;
        t.ts = std::stoull(token);
        if (!std::getline(ss, t.symbol, ',')) continue;
        if (!std::getline(ss, token, ',')) continue;
        t.price = std::stod(token);
        if (!std::getline(ss, token, ',')) continue;
        t.size = std::stoull(token);
        if (!std::getline(ss, token, ',')) continue;
        t.side = token.empty() ? 'N' : token[0];
        cb_(t);
        if (ms_delay_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms_delay_));
        }
    }
}
