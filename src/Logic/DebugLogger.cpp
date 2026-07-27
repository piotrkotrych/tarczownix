#include "DebugLogger.h"
#include <ArduinoJson.h>
#include <cstdarg>

DebugLogger::DebugLogger() : _head(0), _count(0), _sequence(0), _mux(portMUX_INITIALIZER_UNLOCKED) {
}

DebugLogger& DebugLogger::instance() {
    static DebugLogger logger;
    return logger;
}

void DebugLogger::log(const char* fmt, ...) {
    char buffer[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    portENTER_CRITICAL(&_mux);
    _logs[_head].ms = millis();
    strncpy(_logs[_head].msg, buffer, sizeof(_logs[_head].msg) - 1);
    _logs[_head].msg[sizeof(_logs[_head].msg) - 1] = '\0';
    _head = (_head + 1) % MAX_LOGS;
    if (_count < MAX_LOGS) {
        _count++;
    }
    _sequence++;
    portEXIT_CRITICAL(&_mux);
}

String DebugLogger::getJson() {
    JsonDocument doc;
    JsonArray arr = doc["logs"].to<JsonArray>();

    // Copy one entry at a time rather than snapshotting the whole ring buffer: the full
    // snapshot was ~6.4 KB of stack, and this runs on the AsyncTCP task.
    LogEntry entry;
    for (size_t i = 0;; i++) {
        bool hasEntry = false;

        portENTER_CRITICAL(&_mux);
        if (i < _count) {
            const size_t start = (_head + MAX_LOGS - _count) % MAX_LOGS;
            entry = _logs[(start + i) % MAX_LOGS];
            hasEntry = true;
        }
        portEXIT_CRITICAL(&_mux);

        if (!hasEntry) {
            break;
        }

        JsonObject item = arr.add<JsonObject>();
        item["ms"] = entry.ms;
        item["msg"] = entry.msg;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

void DebugLogger::clear() {
    portENTER_CRITICAL(&_mux);
    _head = 0;
    _count = 0;
    _sequence++;
    portEXIT_CRITICAL(&_mux);
}

size_t DebugLogger::getCount() {
    portENTER_CRITICAL(&_mux);
    const size_t count = _count;
    portEXIT_CRITICAL(&_mux);
    return count;
}

unsigned long DebugLogger::getSequence() {
    portENTER_CRITICAL(&_mux);
    const unsigned long sequence = _sequence;
    portEXIT_CRITICAL(&_mux);
    return sequence;
}
