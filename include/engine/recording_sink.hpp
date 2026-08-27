#pragma once

#include <vector>
#include "engine/execution_sink.hpp"
#include "trade.hpp"

class RecordingSink: public ExecutionSink {
public:
    std::vector<Trade> trades;

    void on_trade(const Trade& t) override {
        trades.push_back(t);
    }
};