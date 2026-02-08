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
    JsonDocument doc;
    JsonArray arr = doc["logs"].to<JsonArray>();

    portENTER_CRITICAL(&_mux);
    const size_t count = _count;
    const size_t start = (_head + MAX_LOGS - count) % MAX_LOGS;
    for (size_t i = 0; i < count; i++) {
        const size_t idx = (start + i) % MAX_LOGS;
        JsonObject item = arr.add<JsonObject>();
        item["ms"] = _logs[idx].ms;
        item["msg"] = _logs[idx].msg;
    }
    portEXIT_CRITICAL(&_mux);

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
