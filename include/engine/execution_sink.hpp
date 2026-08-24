#pragma once

#include "engine/trade.hpp"

class ExecutionSink {
public:
    virtual ~ExecutionSink() = default;
    virtual void on_trade(const Trade&) = 0;
    // TODO define on_book_update() for events emission
};

// Not using virtual destructor leads to undefined behaviour and also if we don't use virtual base destructor is statically dispatched so derived destructor is not able to run.