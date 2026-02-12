#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

struct Config {
    int micThreshold;
    int t1Delay;
    int t1Duration;
    int t2Delay;
    int t2Duration;
    int t3Delay;
    int t3Duration;
    int targetTimeoutMs;
};

class SettingsManager {
public:
    SettingsManager();
    void begin();
    void load();
    void save();
    String getJson();
    Config& getConfig();

private:
    Config _config;
    const char* _filename = "/config.json";
};
