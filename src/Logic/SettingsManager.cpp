#include "SettingsManager.h"

static int clampInt(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static void sanitizeConfig(Config& cfg) {
    cfg.micThreshold = clampInt(cfg.micThreshold, 100, 4095);
    cfg.t1Delay = clampInt(cfg.t1Delay, 0, 60000);
    cfg.t1Duration = clampInt(cfg.t1Duration, 100, 60000);
    cfg.t2Delay = clampInt(cfg.t2Delay, 0, 60000);
    cfg.t2Duration = clampInt(cfg.t2Duration, 100, 60000);
    cfg.t3Delay = clampInt(cfg.t3Delay, 0, 60000);
    cfg.t3Duration = clampInt(cfg.t3Duration, 100, 60000);
    cfg.targetTimeoutMs = clampInt(cfg.targetTimeoutMs, 500, 120000);
}

SettingsManager::SettingsManager() {
    // Defaults
    _config.micThreshold = 2000;
    _config.t1Delay = 0;
    _config.t1Duration = 2000;
    _config.t2Delay = 1000;
    _config.t2Duration = 2000;
    _config.t3Delay = 2000;
    _config.t3Duration = 2000;
    _config.targetTimeoutMs = 5000;
}

void SettingsManager::begin() {
    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS Mount Failed");
        return;
    }
    load();
}

void SettingsManager::load() {
    if (LittleFS.exists(_filename)) {
        File file = LittleFS.open(_filename, "r");
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, file);
        if (!error) {
            _config.micThreshold = doc["micThreshold"] | 2000;
            _config.t1Delay = doc["t1Delay"] | 0;
            _config.t1Duration = doc["t1Duration"] | 2000;
            _config.t2Delay = doc["t2Delay"] | 1000;
            _config.t2Duration = doc["t2Duration"] | 2000;
            _config.t3Delay = doc["t3Delay"] | 2000;
            _config.t3Duration = doc["t3Duration"] | 2000;
            _config.targetTimeoutMs = doc["targetTimeoutMs"] | 5000;
        }
        file.close();
    }
    sanitizeConfig(_config);
}

void SettingsManager::save() {
    JsonDocument doc;
    doc["micThreshold"] = _config.micThreshold;
    doc["t1Delay"] = _config.t1Delay;
    doc["t1Duration"] = _config.t1Duration;
    doc["t2Delay"] = _config.t2Delay;
    doc["t2Duration"] = _config.t2Duration;
    doc["t3Delay"] = _config.t3Delay;
    doc["t3Duration"] = _config.t3Duration;
    doc["targetTimeoutMs"] = _config.targetTimeoutMs;

    File file = LittleFS.open(_filename, "w");
    serializeJson(doc, file);
    file.close();
}

String SettingsManager::getJson() {
    sanitizeConfig(_config);
    JsonDocument doc;
    doc["micThreshold"] = _config.micThreshold;
    doc["t1Delay"] = _config.t1Delay;
    doc["t1Duration"] = _config.t1Duration;
    doc["t2Delay"] = _config.t2Delay;
    doc["t2Duration"] = _config.t2Duration;
    doc["t3Delay"] = _config.t3Delay;
    doc["t3Duration"] = _config.t3Duration;
    doc["targetTimeoutMs"] = _config.targetTimeoutMs;
    
    String output;
    serializeJson(doc, output);
    return output;
}

Config& SettingsManager::getConfig() {
    return _config;
}
