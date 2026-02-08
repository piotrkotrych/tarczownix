#pragma once
#include "GameMode.h"

enum CompetitionState { WAITING_START, WAITING_MIC, RUNNING_SEQUENCE, FINISHED };

class ModeCompetition : public GameMode {
public:
    ModeCompetition(Target* t1, Target* t2, Target* t3);
    void start() override;
    void stop() override;
    void update() override;
    void handleInput(int targetId, String cmd) override;
    void onGunshot();
    CompetitionState getState() const { return _state; }

private:
    CompetitionState _state;
    unsigned long _sequenceStartTime;
    
    // Delays and durations (hardcoded for now, or loaded from settings later)
    const int T1_DELAY = 0;
    const int T1_DURATION = 2000;
    const int T2_DELAY = 1000;
    const int T2_DURATION = 2000;
    const int T3_DELAY = 2000;
    const int T3_DURATION = 2000;
    
    bool _t1Shown, _t1Hidden;
    bool _t2Shown, _t2Hidden;
    bool _t3Shown, _t3Hidden;
};
