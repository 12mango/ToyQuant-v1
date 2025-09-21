#pragma once
#include "order.h"
#include "execution_report.h"
#include <functional>

namespace exchange {

using ReportCallback = std::function<void(const ExecutionReport&)>;

class IExchange {
public:
    virtual ~IExchange() = default;
    virtual uint64_t send_order(const Order& order) = 0;
    virtual bool cancel_order(uint64_t order_id) = 0;
    virtual void set_report_callback(ReportCallback cb) = 0;
};

}
