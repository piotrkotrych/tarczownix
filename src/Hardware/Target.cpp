#include "Target.h"
#include "../Logic/DebugLogger.h"

Target::Target(RelayManager* relays, InputManager* inputs, int showRelay, int hideRelay, int sensorShown, int sensorHidden)
    : _relays(relays), _inputs(inputs), _showRelay(showRelay), _hideRelay(hideRelay), 
    _sensorShown(sensorShown), _sensorHidden(sensorHidden), _state(HIDDEN), _moveStartTime(0),
    _pendingMove(false), _pendingShow(false), _pendingStartTime(0), _timeoutMs(5000), _deadtimeMs(50) {
}

void Target::setTimeoutMs(unsigned long timeoutMs) {
    if (timeoutMs < 500) {
        timeoutMs = 500;
    }
    _timeoutMs = timeoutMs;
}

void Target::setDeadtimeMs(unsigned long deadtimeMs) {
    if (deadtimeMs > 500) {
        deadtimeMs = 500;
    }
    _deadtimeMs = deadtimeMs;
}

void Target::show() {
    if (_state == ERROR) return;
    if (_inputs->isActive(_sensorShown)) {
        stop();
        _state = SHOWN;
        DebugLogger::instance().log("Target show: already at shown sensor");
        return;
    }
    
    // Ensure HideRelay is OFF
    _relays->set(_hideRelay, false);
    _relays->commit();
    
    _pendingMove = true;
    _pendingShow = true;
    _pendingStartTime = millis();
    DebugLogger::instance().log("Target show: queued");
}

void Target::hide() {
    if (_state == ERROR) return;
    if (_inputs->isActive(_sensorHidden)) {
        stop();
        _state = HIDDEN;
        DebugLogger::instance().log("Target hide: already at hidden sensor");
        return;
    }

    // Ensure ShowRelay is OFF
    _relays->set(_showRelay, false);
    _relays->commit();
    
    _pendingMove = true;
    _pendingShow = false;
    _pendingStartTime = millis();
    DebugLogger::instance().log("Target hide: queued");
}

void Target::stop() {
    const bool wasMoving = (_state == MOVING_SHOW || _state == MOVING_HIDE || _pendingMove);
    _pendingMove = false;
    _relays->set(_showRelay, false);
    _relays->set(_hideRelay, false);
    _relays->commit();

    if (_inputs->isActive(_sensorShown)) {
        _state = SHOWN;
    } else if (_inputs->isActive(_sensorHidden)) {
        _state = HIDDEN;
    } else if (wasMoving) {
        _state = STOPPED;
    }

    if (wasMoving) {
        DebugLogger::instance().log("Target stop: relays off");
    }
}

void Target::update() {
    if (_pendingMove) {
        if (millis() - _pendingStartTime >= _deadtimeMs) {
            if (_pendingShow) {
                _relays->set(_showRelay, true);
                _relays->commit();
                _state = MOVING_SHOW;
                DebugLogger::instance().log("Target show: relay on");
            } else {
                _relays->set(_hideRelay, true);
                _relays->commit();
                _state = MOVING_HIDE;
                DebugLogger::instance().log("Target hide: relay on");
            }
            _moveStartTime = millis();
            _pendingMove = false;
        }
        return;
    }

    if (_state == MOVING_SHOW) {
        if (_inputs->isActive(_sensorShown)) {
            stop();
            _state = SHOWN;
            DebugLogger::instance().log("Target show: sensor reached");
        } else if (millis() - _moveStartTime > _timeoutMs) {
            stop();
            _state = ERROR;
            DebugLogger::instance().log("Target show: timeout -> ERROR");
        }
    } else if (_state == MOVING_HIDE) {
        if (_inputs->isActive(_sensorHidden)) {
            stop();
            _state = HIDDEN;
            DebugLogger::instance().log("Target hide: sensor reached");
        } else if (millis() - _moveStartTime > _timeoutMs) {
            stop();
            _state = ERROR;
            DebugLogger::instance().log("Target hide: timeout -> ERROR");
        }
    }
}

TargetState Target::getState() {
    return _state;
}
