#include "InputManager.h"

InputManager::InputManager(PCF8574* pcf) : _pcf(pcf), _lastRaw(0xFF), _stableState(0xFF), _lastDebounceTime(0) {
}

void InputManager::begin() {
    for (uint8_t pin = 0; pin < 8; pin++) {
        _pcf->pinMode(pin, INPUT_PULLUP);
    }
}

void InputManager::update() {
    uint8_t raw = 0;

#ifdef PCF8574_LOW_MEMORY
    raw = _pcf->digitalReadAll();
#else
    const auto all = _pcf->digitalReadAll();
    raw |= (all.p0 == HIGH ? 1 : 0) << 0;
    raw |= (all.p1 == HIGH ? 1 : 0) << 1;
    raw |= (all.p2 == HIGH ? 1 : 0) << 2;
    raw |= (all.p3 == HIGH ? 1 : 0) << 3;
    raw |= (all.p4 == HIGH ? 1 : 0) << 4;
    raw |= (all.p5 == HIGH ? 1 : 0) << 5;
    raw |= (all.p6 == HIGH ? 1 : 0) << 6;
    raw |= (all.p7 == HIGH ? 1 : 0) << 7;
#endif
    if (raw != _lastRaw) {
        _lastDebounceTime = millis();
    }
    _lastRaw = raw;

    if ((millis() - _lastDebounceTime) > DEBOUNCE_DELAY) {
        _stableState = raw;
    }
}

bool InputManager::isActive(int pin) {
    if (pin < 0 || pin > 7) {
        return false;
    }
    // Active LOW
    return !(_stableState & (1 << pin));
}
