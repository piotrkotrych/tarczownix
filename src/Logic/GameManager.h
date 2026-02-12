#pragma once
#include "GameMode.h"
#include "ModeManual.h"
#include "ModeCompetition.h"
#include "ModeSequence.h"

struct Config;

class GameManager {
public:
    GameManager(Target* t1, Target* t2, Target* t3);
    void setMode(String modeName);
    void update();
    void handleWebInput(int targetId, String cmd);
    void applyConfig(const Config& cfg);
    void requestGunshot();
    ModeCompetition* getCompetitionMode();
    const char* getModeName() const { return _modeName.c_str(); }

private:
    GameMode* _currentMode;
    Target* _t1;
    Target* _t2;
    Target* _t3;
    ModeManual _manualMode;
    ModeCompetition _compMode;
    ModeSequence _seqMode;
    String _modeName;
};
