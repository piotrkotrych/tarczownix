#pragma once
#include <Arduino.h>
#include <PCF8574.h>

class InputManager {
public:
    InputManager(PCF8574* pcf);
    // Configures the expander pins and brings up the I2C device.
    // Must be called before update(); returns false if the expander did not answer.
    bool begin();
    void update();
    bool isActive(int pin);
    uint8_t getRaw() const { return _lastRaw; }
    uint8_t getStable() const { return _stableState; }

private:
    PCF8574* _pcf;
    uint8_t _lastRaw;
    uint8_t _stableState;
    unsigned long _lastDebounceTime;
    const unsigned long DEBOUNCE_DELAY = 25;
};
