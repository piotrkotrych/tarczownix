#include "GameManager.h"
#include "DebugLogger.h"

GameManager::GameManager(Target* t1, Target* t2, Target* t3) 
    : _manualMode(t1, t2, t3), _compMode(t1, t2, t3) {
    _currentMode = &_manualMode;
    _currentMode->start();
}

void GameManager::setMode(String modeName) {
    if (modeName != "manual" && modeName != "competition") {
        Serial.print("Unknown mode: ");
        Serial.println(modeName);
        DebugLogger::instance().log("Mode change ignored: %s", modeName.c_str());
        return;
    }

    _currentMode->stop();
    if (modeName == "manual") {
        _currentMode = &_manualMode;
    } else {
        _currentMode = &_compMode;
    }
    _currentMode->start();
    DebugLogger::instance().log("Mode set: %s", modeName.c_str());
}

void GameManager::update() {
    _currentMode->update();
}

void GameManager::handleWebInput(int targetId, String cmd) {
    _currentMode->handleInput(targetId, cmd);
}

ModeCompetition* GameManager::getCompetitionMode() {
    return &_compMode;
}
