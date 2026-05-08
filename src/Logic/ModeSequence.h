#pragma once
#include "GameMode.h"

class ModeSequence : public GameMode {
public:
    ModeSequence(Target* t1, Target* t2, Target* t3);
    void start() override;
    void stop() override;
    void update() override;
    void handleInput(int targetId, String cmd) override;
    void setTimings(int t1Delay, int t1Duration, int t2Delay, int t2Duration, int t3Delay, int t3Duration);

private:
    enum SequenceState { SEQ_IDLE, SEQ_WAIT_DELAY, SEQ_SHOWING, SEQ_VISIBLE, SEQ_HIDING };

    int clampMs(int value, int minValue, int maxValue) const;
    Target* getTargetById(int id);
    void scheduleNextTarget();

    SequenceState _state;
    int _activeTargetId;
    unsigned long _stateStartMs;
    unsigned long _waitMs;
    unsigned long _showMs;

    int _t1Delay;
    int _t1Duration;
    int _t2Delay;
    int _t2Duration;
    int _t3Delay;
    int _t3Duration;
};
