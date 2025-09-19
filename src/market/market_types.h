#pragma once
#include<string>
#include<cstdint>
#include<functional>

struct Tick{
    uint64_t ts;
    std::string symbol;
    double price;
    uint64_t size;
    char side;
};

using TickCallback = std::function<void(const Tick&)>;
