#include "RelayManager.h"

RelayManager::RelayManager(PCF8574* pcf) : _pcf(pcf), _shadowRegister(0xFF), _dirty(true) {
}

bool RelayManager::isValidPin(int pin) const {
    return pin >= 0 && pin < 8;
}

int RelayManager::pairedPin(int pin) const {
    return (pin % 2 == 0) ? (pin + 1) : (pin - 1);
}

void RelayManager::begin() {
    // Configure all expander pins as OUTPUT, default HIGH (relay off)
    for (uint8_t pin = 0; pin < 8; pin++) {
        _pcf->pinMode(pin, OUTPUT, HIGH);
    }
    _dirty = true;
    commit();
}

void RelayManager::set(int pin, bool active) {
    if (!isValidPin(pin)) {
        return;
    }

    uint8_t oldRegister = _shadowRegister;
    if (active) {
        const int pair = pairedPin(pin);
        if (isValidPin(pair)) {
            _shadowRegister |= (1 << pair); // Force paired relay OFF
        }
    }
    if (active) {
        _shadowRegister &= ~(1 << pin); // Clear bit for LOW (On)
    } else {
        _shadowRegister |= (1 << pin);  // Set bit for HIGH (Off)
    }
    
    if (oldRegister != _shadowRegister) {
        _dirty = true;
    }
}

void RelayManager::commit() {
    if (_dirty) {
#ifdef PCF8574_LOW_MEMORY
        _pcf->digitalWriteAll(_shadowRegister);
#else
        PCF8574::DigitalInput all;
        all.p0 = (_shadowRegister & (1 << 0)) ? HIGH : LOW;
        all.p1 = (_shadowRegister & (1 << 1)) ? HIGH : LOW;
        all.p2 = (_shadowRegister & (1 << 2)) ? HIGH : LOW;
        all.p3 = (_shadowRegister & (1 << 3)) ? HIGH : LOW;
        all.p4 = (_shadowRegister & (1 << 4)) ? HIGH : LOW;
        all.p5 = (_shadowRegister & (1 << 5)) ? HIGH : LOW;
        all.p6 = (_shadowRegister & (1 << 6)) ? HIGH : LOW;
        all.p7 = (_shadowRegister & (1 << 7)) ? HIGH : LOW;
        _pcf->digitalWriteAll(all);
#endif
        _dirty = false;
    }
}
