#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <iomanip>

enum class RunMode { Backtest, Realtime };

class Logger {
public:
    Logger(const std::string& file, RunMode mode)
        : mode_(mode) {
        if(!file.empty()) {
            fout_.open(file, std::ios::out | std::ios::app);
        }
    }

    template<typename... Args>
    void log(Args&&... args) {
        std::ostringstream oss;
        (oss << ... << args); // C++17 fold expression

        std::string msg = oss.str();
        if(fout_.is_open()) {
            fout_ << msg << std::endl;
        }
        std::cout << msg << std::endl;
    }

private:
    RunMode mode_;
    std::ofstream fout_;
};
