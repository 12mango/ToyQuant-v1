#pragma once
#include <fstream>
#include <string>

class Logger {
public:
    Logger(const std::string& orders_file, const std::string& trades_file) {
        orders_out.open(orders_file);
        trades_out.open(trades_file);

        // 写表头
        orders_out << "ts,order_id,symbol,side,price,size,action\n";
        trades_out << "ts,order_id,symbol,side,price,size\n";
    }

    void log_order(long ts, int order_id, const std::string& symbol,
                   char side, double price, int size, const std::string& action) {
        orders_out << ts << "," << order_id << "," << symbol << ","
                   << side << "," << price << "," << size << "," << action << "\n";
    }

    void log_trade(long ts, int order_id, const std::string& symbol,
                   char side, double price, int size) {
        trades_out << ts << "," << order_id << "," << symbol << ","
                   << side << "," << price << "," << size << "\n";
    }

private:
    std::ofstream orders_out;
    std::ofstream trades_out;
};
