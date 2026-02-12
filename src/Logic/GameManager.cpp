#include "GameManager.h"
#include "DebugLogger.h"
#include "SettingsManager.h"

GameManager::GameManager(Target* t1, Target* t2, Target* t3) 
    : _t1(t1), _t2(t2), _t3(t3),
      _manualMode(t1, t2, t3), _compMode(t1, t2, t3), _seqMode(t1, t2, t3),
      _modeName("manual") {
    _currentMode = &_manualMode;
    _currentMode->start();
}

void GameManager::setMode(String modeName) {
    if (modeName != "manual" && modeName != "competition" && modeName != "sequence") {
        Serial.print("Unknown mode: ");
        Serial.println(modeName);
        DebugLogger::instance().log("Mode change ignored: %s", modeName.c_str());
        return;
    }

    if (_modeName == modeName) {
        return;
    }

    _currentMode->stop();
    if (modeName == "manual") {
        _currentMode = &_manualMode;
    } else if (modeName == "sequence") {
        _currentMode = &_seqMode;
    } else {
        _currentMode = &_compMode;
    }
    _modeName = modeName;
    _currentMode->start();
    DebugLogger::instance().log("Mode set: %s", modeName.c_str());
}

void GameManager::update() {
    _currentMode->update();
}

void GameManager::handleWebInput(int targetId, String cmd) {
    if (cmd.startsWith("mode:")) {
        String modeName = cmd.substring(5);
        modeName.trim();
        setMode(modeName);
        return;
    }

    if (cmd == "gunshot") {
        requestGunshot();
        return;
    }

    _currentMode->handleInput(targetId, cmd);
}

void GameManager::applyConfig(const Config& cfg) {
    _compMode.setTimings(cfg.t1Delay, cfg.t1Duration, cfg.t2Delay, cfg.t2Duration, cfg.t3Delay, cfg.t3Duration);
    _seqMode.setTimings(cfg.t1Delay, cfg.t1Duration, cfg.t2Delay, cfg.t2Duration, cfg.t3Delay, cfg.t3Duration);

    const unsigned long timeoutMs = (unsigned long)cfg.targetTimeoutMs;
    _t1->setTimeoutMs(timeoutMs);
    _t2->setTimeoutMs(timeoutMs);
    _t3->setTimeoutMs(timeoutMs);
}

void GameManager::requestGunshot() {
    if (_currentMode == &_compMode) {
        _compMode.requestGunshot();
    }
}

ModeCompetition* GameManager::getCompetitionMode() {
    return &_compMode;
}
