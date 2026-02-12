#pragma once
#include <Arduino.h>
#include "RelayManager.h"
#include "InputManager.h"

enum TargetState { HIDDEN, MOVING_SHOW, SHOWN, MOVING_HIDE, ERROR };

class Target {
public:
    Target(RelayManager* relays, InputManager* inputs, int showRelay, int hideRelay, int sensorShown, int sensorHidden);
    void show();
    void hide();
    void stop();
    void update();
    void setTimeoutMs(unsigned long timeoutMs);
    void setDeadtimeMs(unsigned long deadtimeMs);
    TargetState getState();
    bool isPendingMove() const { return _pendingMove; }
    bool isPendingShow() const { return _pendingShow; }
    unsigned long getMoveStartTime() const { return _moveStartTime; }
    unsigned long getPendingStartTime() const { return _pendingStartTime; }

private:
    RelayManager* _relays;
    InputManager* _inputs;
    int _showRelay;
    int _hideRelay;
    int _sensorShown;
    int _sensorHidden;
    TargetState _state;
    unsigned long _moveStartTime;
    bool _pendingMove;
    bool _pendingShow;
    unsigned long _pendingStartTime;
    unsigned long _timeoutMs;
    unsigned long _deadtimeMs;
};
