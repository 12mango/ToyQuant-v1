#include "backtest_driver.h"
#include <iostream>

BacktestDriver::BacktestDriver(const std::string& tick_file,
                               const std::string& orders_file,
                               const std::string& trades_file)
    : strat(10, 0.0001) // 参数和 main.cpp 一致
{
    orders_out.open(orders_file);
    trades_out.open(trades_file);

    // CSV 表头
    orders_out << "ts,symbol,side,price,quantity,order_id\n";
    trades_out << "ts,symbol,side,price,quantity,order_id\n";

    engine.set_report_callback([this](const ExecutionReport& report){
        this->on_report(report);
    });

    // 启动 CSV 回测
    CsvFeed feed(tick_file, [this](const Tick& t){ this->on_tick(t); }, 0);
    feed.run();
}

void BacktestDriver::on_tick(const Tick& t) {
    // 1. 更新 OrderBook
    ob.on_tick(t);
    auto top = ob.top(t.symbol);

    // 2. 策略生成订单
    auto orders = strat.on_top_of_book(t.symbol, top);
    for (auto& o : orders) {
        o.order_id = next_order_id++;

        // 3. 转成 exchange::Order
        exchange::Order ex_order;
        ex_order.id = o.order_id;
        ex_order.symbol = o.symbol;
        ex_order.side = (o.side == Order::BUY ? exchange::Side::Buy : exchange::Side::Sell);
        ex_order.type = exchange::OrderType::Limit;
        ex_order.price = o.price;
        ex_order.qty = o.quantity;
        ex_order.remaining = o.quantity;
        ex_order.timestamp = t.ts;
        ex_order.owner = "MarketMaker";

        // 4. 记录 orders.csv
        orders_out << t.ts << "," << o.symbol << ","
                   << (o.side == Order::BUY ? "B" : "S") << ","
                   << o.price << "," << o.quantity << ","
                   << o.order_id << "\n";

        // 5. 送撮合
        engine.send_order(ex_order);
    }
}

void BacktestDriver::on_report(const ExecutionReport& report) {
    if(report.exec_type == ExecType::Trade) {
        trades_out << report.order_id << "," 
                   << report.symbol << ","
                   << (report.quantity > 0 ? "B" : "S") << "," // 可根据你的侧判断
                   << report.price << ","
                   << report.quantity << ","
                   << report.order_id << "\n";
    }
}

void BacktestDriver::run() {
    // CsvFeed 在构造函数里已运行完毕
}
