#include "ModeCompetition.h"

ModeCompetition::ModeCompetition(Target* t1, Target* t2, Target* t3) 
    : GameMode(t1, t2, t3), _state(WAITING_START) {
}

void ModeCompetition::start() {
    _t1->hide();
    _t2->hide();
    _t3->hide();
    _state = WAITING_MIC;
    Serial.println("Competition Mode: Waiting for Gunshot...");
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

    if (_state == RUNNING_SEQUENCE) {
        unsigned long elapsed = millis() - _sequenceStartTime;

        // Target 1
        if (elapsed > T1_DELAY && !_t1Shown) {
            _t1->show();
            _t1Shown = true;
        }
        if (elapsed > (T1_DELAY + T1_DURATION) && !_t1Hidden) {
            _t1->hide();
            _t1Hidden = true;
        }

        // Target 2
        if (elapsed > T2_DELAY && !_t2Shown) {
            _t2->show();
            _t2Shown = true;
        }
        if (elapsed > (T2_DELAY + T2_DURATION) && !_t2Hidden) {
            _t2->hide();
            _t2Hidden = true;
        }

        // Target 3
        if (elapsed > T3_DELAY && !_t3Shown) {
            _t3->show();
            _t3Shown = true;
        }
        if (elapsed > (T3_DELAY + T3_DURATION) && !_t3Hidden) {
            _t3->hide();
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
    // Ignore manual input in competition mode, or maybe allow "stop"
    if (cmd == "stop") {
        stop();
    }
}

void ModeCompetition::onGunshot() {
    if (_state == WAITING_MIC) {
        _state = RUNNING_SEQUENCE;
        _sequenceStartTime = millis();
        _t1Shown = false; _t1Hidden = false;
        _t2Shown = false; _t2Hidden = false;
        _t3Shown = false; _t3Hidden = false;
        Serial.println("Competition Mode: Gunshot Detected! Sequence Started.");
    }
}
