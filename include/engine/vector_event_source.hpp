#pragma once

#include "engine/event_source.hpp"
#include <vector>

class VectorEventSource: public EventSource {
    std::vector<Command> commands;
    std::size_t cursor = 0;

public:
    explicit VectorEventSource(std::vector<Command> cmds) : commands(std::move(cmds)) {}
    bool next(Command& out) override {
        if(cursor >= commands.size()) return false;
        out = commands[cursor++];
        return true;
    }
};