#pragma once
#include <Arduino.h>

class DebugLogger {
public:
    static DebugLogger& instance();

    void log(const char* fmt, ...);
    String getJson();
    size_t getCount();
    // Monotonic count of every message ever logged. getCount() saturates once the ring
    // buffer is full, so it cannot be used to detect new entries.
    unsigned long getSequence();
    void clear();

private:
    DebugLogger();

    struct LogEntry {
        unsigned long ms;
        char msg[96];
    };

    static const size_t MAX_LOGS = 64;
    LogEntry _logs[MAX_LOGS];
    size_t _head;
    size_t _count;
    unsigned long _sequence;
    portMUX_TYPE _mux;
};
