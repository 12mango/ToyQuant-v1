#include "market/csv_feed.h"
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <iostream>

CsvFeed::CsvFeed(const std::string& path, TickCallback cb, int ms_delay)
    : path_(path), cb_(cb), ms_delay_(ms_delay) {}

void CsvFeed::run() {
    std::ifstream ifs(path_);
    if (!ifs.is_open()) {
        std::cerr << "CsvFeed: failed to open file " << path_ << std::endl;
        return;
    }

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;

        std::istringstream ss(line);
        std::string token;
        Tick t;

        try {
            // timestamp
            if (!std::getline(ss, token, ',')) continue;
            t.ts = std::stoull(token);

            // symbol
            if (!std::getline(ss, t.symbol, ',')) continue;

            // price
            if (!std::getline(ss, token, ',')) continue;
            t.price = std::stod(token);

            // size
            if (!std::getline(ss, token, ',')) continue;
            t.size = std::stoull(token);

            // side
            if (!std::getline(ss, token, ',')) continue;
            if (token.empty()) {
                t.side = Side::Unknown;
            } else {
                t.side = to_side(token[0]);
            }

            // 回调
            cb_(t);

            // 延迟
            if (ms_delay_ > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms_delay_));
            }

        } catch (const std::exception& e) {
            std::cerr << "CsvFeed parse error on line: " << line
                      << " , exception: " << e.what() << std::endl;
            continue; // 忽略非法行
        }
    }
}
