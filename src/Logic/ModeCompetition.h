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
    void setTimings(int t1Delay, int t1Duration, int t2Delay, int t2Duration, int t3Delay, int t3Duration);
    void requestGunshot();
    void onGunshot();
    CompetitionState getState() const { return _state; }

private:
    int clampMs(int value, int minValue, int maxValue) const;

    CompetitionState _state;
    unsigned long _sequenceStartTime;

    int _t1Delay;
    int _t1Duration;
    int _t2Delay;
    int _t2Duration;
    int _t3Delay;
    int _t3Duration;
    
    bool _t1Shown, _t1Hidden;
    bool _t2Shown, _t2Hidden;
    bool _t3Shown, _t3Hidden;
    bool _gunshotPending;
};
