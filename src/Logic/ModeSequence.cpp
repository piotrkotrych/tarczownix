#include "ModeSequence.h"

ModeSequence::ModeSequence(Target* t1, Target* t2, Target* t3)
    : GameMode(t1, t2, t3),
      _state(SEQ_IDLE), _activeTargetId(0), _stateStartMs(0), _waitMs(0), _showMs(0),
      _t1Delay(0), _t1Duration(2000),
      _t2Delay(1000), _t2Duration(2000),
      _t3Delay(2000), _t3Duration(2000) {
}

int ModeSequence::clampMs(int value, int minValue, int maxValue) const {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

Target* ModeSequence::getTargetById(int id) {
    if (id == 1) return _t1;
    if (id == 2) return _t2;
    if (id == 3) return _t3;
    return nullptr;
}

void ModeSequence::setTimings(int t1Delay, int t1Duration, int t2Delay, int t2Duration, int t3Delay, int t3Duration) {
    _t1Delay = clampMs(t1Delay, 0, 60000);
    _t1Duration = clampMs(t1Duration, 100, 60000);
    _t2Delay = clampMs(t2Delay, 0, 60000);
    _t2Duration = clampMs(t2Duration, 100, 60000);
    _t3Delay = clampMs(t3Delay, 0, 60000);
    _t3Duration = clampMs(t3Duration, 100, 60000);
}

void ModeSequence::scheduleNextTarget() {
    _activeTargetId = random(1, 4);

    int delayLimit = 1000;
    if (_activeTargetId == 1) {
        delayLimit = _t1Delay;
        _showMs = (unsigned long)_t1Duration;
    } else if (_activeTargetId == 2) {
        delayLimit = _t2Delay;
        _showMs = (unsigned long)_t2Duration;
    } else {
        delayLimit = _t3Delay;
        _showMs = (unsigned long)_t3Duration;
    }

    const int safeDelayLimit = clampMs(delayLimit, 0, 60000);
    const int randomPart = (safeDelayLimit > 0) ? random(0, safeDelayLimit + 1) : 0;
    _waitMs = (unsigned long)(300 + randomPart);
    _stateStartMs = millis();
    _state = SEQ_WAIT_DELAY;
}

void ModeSequence::start() {
    _t1->hide();
    _t2->hide();
    _t3->hide();
    scheduleNextTarget();
}

void ModeSequence::stop() {
    _t1->stop();
    _t2->stop();
    _t3->stop();
    _state = SEQ_IDLE;
    _activeTargetId = 0;
}

void ModeSequence::update() {
    _t1->update();
    _t2->update();
    _t3->update();

    if (_state == SEQ_IDLE) {
        return;
    }

    const unsigned long elapsed = millis() - _stateStartMs;

    if (_state == SEQ_WAIT_DELAY) {
        if (elapsed >= _waitMs) {
            Target* target = getTargetById(_activeTargetId);
            if (target) {
                target->show();
            }
            _state = SEQ_SHOWING;
            _stateStartMs = millis();
        }
        return;
    }

    if (_state == SEQ_SHOWING) {
        Target* target = getTargetById(_activeTargetId);
        if (target && target->getState() == SHOWN) {
            _state = SEQ_VISIBLE;
            _stateStartMs = millis();
        } else if (target && target->getState() == ERROR) {
            stop();
        }
        return;
    }

    if (_state == SEQ_VISIBLE) {
        Target* target = getTargetById(_activeTargetId);
        if (elapsed >= _showMs && target) {
            target->hide();
            _state = SEQ_HIDING;
            _stateStartMs = millis();
        } else if (target && target->getState() == ERROR) {
            stop();
        }
        return;
    }

    if (_state == SEQ_HIDING) {
        Target* target = getTargetById(_activeTargetId);
        if (target && target->getState() == HIDDEN) {
            scheduleNextTarget();
        } else if (target && target->getState() == ERROR) {
            stop();
        }
    }
}

void ModeSequence::handleInput(int targetId, String cmd) {
    (void)targetId;
    if (cmd == "stop") {
        stop();
    } else if (cmd == "arm" || cmd == "start") {
        // stop() parks the mode in SEQ_IDLE; this is how the run is picked back up.
        start();
    } else if (cmd == "reset") {
        _t1->reset();
        _t2->reset();
        _t3->reset();
        _state = SEQ_IDLE;
        _activeTargetId = 0;
    }
}
