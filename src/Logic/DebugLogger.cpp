#include "DebugLogger.h"
#include <ArduinoJson.h>
#include <cstdarg>

DebugLogger::DebugLogger() : _head(0), _count(0), _mux(portMUX_INITIALIZER_UNLOCKED) {
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
    portEXIT_CRITICAL(&_mux);
}

String DebugLogger::getJson() {
    LogEntry snapshot[MAX_LOGS];
    size_t count = 0;

    portENTER_CRITICAL(&_mux);
    count = _count;
    const size_t start = (_head + MAX_LOGS - count) % MAX_LOGS;
    for (size_t i = 0; i < count; i++) {
        const size_t idx = (start + i) % MAX_LOGS;
        snapshot[i] = _logs[idx];
    }
    portEXIT_CRITICAL(&_mux);

    JsonDocument doc;
    JsonArray arr = doc["logs"].to<JsonArray>();

    for (size_t i = 0; i < count; i++) {
        JsonObject item = arr.add<JsonObject>();
        item["ms"] = snapshot[i].ms;
        item["msg"] = snapshot[i].msg;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

void DebugLogger::clear() {
    portENTER_CRITICAL(&_mux);
    _head = 0;
    _count = 0;
    portEXIT_CRITICAL(&_mux);
}

size_t DebugLogger::getCount() {
    portENTER_CRITICAL(&_mux);
    const size_t count = _count;
    portEXIT_CRITICAL(&_mux);
    return count;
}
