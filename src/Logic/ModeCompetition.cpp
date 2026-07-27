#include "ModeCompetition.h"
#include "DebugLogger.h"

ModeCompetition::ModeCompetition(Target* t1, Target* t2, Target* t3)
    : GameMode(t1, t2, t3), _state(WAITING_START), _sequenceStartTime(0),
      _t1Delay(0), _t1Duration(2000),
      _t2Delay(1000), _t2Duration(2000),
      _t3Delay(2000), _t3Duration(2000),
      _t1Shown(false), _t1HideIssued(false), _t1Hidden(false),
      _t2Shown(false), _t2HideIssued(false), _t2Hidden(false),
      _t3Shown(false), _t3HideIssued(false), _t3Hidden(false),
      _t1ShownAt(0), _t2ShownAt(0), _t3ShownAt(0),
      _gunshotPending(false) {
}

int ModeCompetition::clampMs(int value, int minValue, int maxValue) const {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

void ModeCompetition::setTimings(int t1Delay, int t1Duration, int t2Delay, int t2Duration, int t3Delay, int t3Duration) {
    _t1Delay = clampMs(t1Delay, 0, 60000);
    _t1Duration = clampMs(t1Duration, 100, 60000);
    _t2Delay = clampMs(t2Delay, 0, 60000);
    _t2Duration = clampMs(t2Duration, 100, 60000);
    _t3Delay = clampMs(t3Delay, 0, 60000);
    _t3Duration = clampMs(t3Duration, 100, 60000);
}

bool ModeCompetition::anyTargetInError() const {
    return _t1->getState() == ERROR || _t2->getState() == ERROR || _t3->getState() == ERROR;
}

void ModeCompetition::arm() {
    _t1->hide();
    _t2->hide();
    _t3->hide();
    _gunshotPending = false;
    _t1Shown = false; _t1HideIssued = false; _t1Hidden = false;
    _t2Shown = false; _t2HideIssued = false; _t2Hidden = false;
    _t3Shown = false; _t3HideIssued = false; _t3Hidden = false;
    _t1ShownAt = 0; _t2ShownAt = 0; _t3ShownAt = 0;
    _state = WAITING_MIC;
    Serial.println("Competition Mode: Waiting for Gunshot...");
    DebugLogger::instance().log("Competition armed: waiting for gunshot");
}

void ModeCompetition::start() {
    arm();
}

void ModeCompetition::stop() {
    _t1->stop();
    _t2->stop();
    _t3->stop();
    _state = WAITING_START;
}

void ModeCompetition::update() {
    _t1->update();
    _t2->update();
    _t3->update();

    if (_gunshotPending) {
        _gunshotPending = false;
        onGunshot();
    }

    if (_state == RUNNING_SEQUENCE) {
        // A target that latched ERROR will never report SHOWN/HIDDEN again, so without
        // this the run would sit in RUNNING_SEQUENCE forever.
        if (anyTargetInError()) {
            stop();
            DebugLogger::instance().log("Competition aborted: target in ERROR");
            return;
        }

        unsigned long elapsed = millis() - _sequenceStartTime;

        if (elapsed > (unsigned long)_t1Delay && !_t1Shown) {
            _t1->show();
            _t1Shown = true;
        }
        if (_t1Shown && _t1ShownAt == 0 && _t1->getState() == SHOWN) {
            _t1ShownAt = millis();
        }
        if (_t1ShownAt > 0 && millis() - _t1ShownAt >= (unsigned long)_t1Duration && !_t1HideIssued) {
            _t1->hide();
            _t1HideIssued = true;
        }
        if (_t1HideIssued && _t1->getState() == HIDDEN) {
            _t1Hidden = true;
        }

        if (elapsed > (unsigned long)_t2Delay && !_t2Shown) {
            _t2->show();
            _t2Shown = true;
        }
        if (_t2Shown && _t2ShownAt == 0 && _t2->getState() == SHOWN) {
            _t2ShownAt = millis();
        }
        if (_t2ShownAt > 0 && millis() - _t2ShownAt >= (unsigned long)_t2Duration && !_t2HideIssued) {
            _t2->hide();
            _t2HideIssued = true;
        }
        if (_t2HideIssued && _t2->getState() == HIDDEN) {
            _t2Hidden = true;
        }

        if (elapsed > (unsigned long)_t3Delay && !_t3Shown) {
            _t3->show();
            _t3Shown = true;
        }
        if (_t3Shown && _t3ShownAt == 0 && _t3->getState() == SHOWN) {
            _t3ShownAt = millis();
        }
        if (_t3ShownAt > 0 && millis() - _t3ShownAt >= (unsigned long)_t3Duration && !_t3HideIssued) {
            _t3->hide();
            _t3HideIssued = true;
        }
        if (_t3HideIssued && _t3->getState() == HIDDEN) {
            _t3Hidden = true;
        }
        
        // Check if finished
        if (_t1Hidden && _t2Hidden && _t3Hidden) {
            _state = FINISHED;
            Serial.println("Competition Mode: Sequence Finished");
        }
    }
}

void ModeCompetition::handleInput(int targetId, String cmd) {
    (void)targetId;
    // Manual show/hide is deliberately ignored here; only run control is accepted.
    if (cmd == "stop") {
        stop();
    } else if (cmd == "arm" || cmd == "start") {
        // Without this there is no way back to WAITING_MIC after a run finishes or is
        // stopped, short of switching modes away and back again.
        arm();
    } else if (cmd == "reset") {
        _t1->reset();
        _t2->reset();
        _t3->reset();
        _state = WAITING_START;
    }
}

void ModeCompetition::requestGunshot() {
    _gunshotPending = true;
}

void ModeCompetition::onGunshot() {
    if (_state == WAITING_MIC) {
        _state = RUNNING_SEQUENCE;
        _sequenceStartTime = millis();
        _t1Shown = false; _t1HideIssued = false; _t1Hidden = false;
        _t2Shown = false; _t2HideIssued = false; _t2Hidden = false;
        _t3Shown = false; _t3HideIssued = false; _t3Hidden = false;
        _t1ShownAt = 0; _t2ShownAt = 0; _t3ShownAt = 0;
        Serial.println("Competition Mode: Gunshot Detected! Sequence Started.");
    }
}
