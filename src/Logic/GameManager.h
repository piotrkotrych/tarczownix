#pragma once
#include "GameMode.h"
#include "ModeManual.h"
#include "ModeCompetition.h"

class GameManager {
public:
    GameManager(Target* t1, Target* t2, Target* t3);
    void setMode(String modeName);
    void update();
    void handleWebInput(int targetId, String cmd);
    ModeCompetition* getCompetitionMode();
    const char* getModeName() const { return (_currentMode == &_manualMode) ? "manual" : "competition"; }

private:
    GameMode* _currentMode;
    ModeManual _manualMode;
    ModeCompetition _compMode;
};
