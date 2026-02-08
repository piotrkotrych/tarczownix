#include "SettingsManager.h"

SettingsManager::SettingsManager() {
    // Defaults
    _config.micThreshold = 2000;
    _config.t1Delay = 0;
    _config.t1Duration = 2000;
    _config.t2Delay = 1000;
    _config.t2Duration = 2000;
    _config.t3Delay = 2000;
    _config.t3Duration = 2000;
}

void SettingsManager::begin() {
    if (!LittleFS.begin(true)) {
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
        }
        file.close();
    }
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

    File file = LittleFS.open(_filename, "w");
    serializeJson(doc, file);
    file.close();
}

String SettingsManager::getJson() {
    JsonDocument doc;
    doc["micThreshold"] = _config.micThreshold;
    doc["t1Delay"] = _config.t1Delay;
    doc["t1Duration"] = _config.t1Duration;
    doc["t2Delay"] = _config.t2Delay;
    doc["t2Duration"] = _config.t2Duration;
    doc["t3Delay"] = _config.t3Delay;
    doc["t3Duration"] = _config.t3Duration;
    
    String output;
    serializeJson(doc, output);
    return output;
}

Config& SettingsManager::getConfig() {
    return _config;
}
