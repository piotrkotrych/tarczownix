#include "InputManager.h"

InputManager::InputManager(PCF8574* pcf) : _pcf(pcf), _lastRaw(0xFF), _stableState(0xFF), _lastDebounceTime(0) {
}

bool InputManager::begin() {
    // Pin modes must be declared BEFORE PCF8574::begin(): the library snapshots the
    // pull-up mask there to seed its read buffer. Initialising in the other order leaves
    // that buffer at 0, which makes every input read as permanently active.
    for (uint8_t pin = 0; pin < 8; pin++) {
        _pcf->pinMode(pin, INPUT_PULLUP);
    }

    return _pcf->begin();
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
