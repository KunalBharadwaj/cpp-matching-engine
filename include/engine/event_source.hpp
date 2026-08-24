#pragma once

struct Command;

class EventSource {
public:
    virtual ~EventSource() = default;
    virtual bool next(Command& out) = 0;
};

// We want to keep virtual at the seams because virtual cannot be inlined and optimized by the compiler and also there is some indirection