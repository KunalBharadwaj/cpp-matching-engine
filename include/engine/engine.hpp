#pragma once

#include "engine/event_source.hpp"
#include "engine/order_book.hpp"
#include "engine/execution_sink.hpp"
#include "engine/command.hpp"

inline void run(EventSource& source, OrderBook& book, ExecutionSink& sink) {
    Command cmd;
    while(source.next(cmd)) {
        switch(cmd.type) {
            case CommandType::New:  book.process(cmd.order, sink); break;
            case CommandType::Cancel: book.cancelOrder(cmd.cancel_id); break;
        }
    }
}