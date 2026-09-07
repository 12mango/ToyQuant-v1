#pragma once

#include <cstdint>
#include <vector>

namespace metrics
{
double compute_fill_rate(uint64_t submitted, uint64_t filled);
double compute_cancel_rate(uint64_t submitted, uint64_t cancelled);
double compute_max_drawdown(const std::vector<double>& equity);
double compute_inventory_exposure(int64_t position);
}  // namespace metrics
