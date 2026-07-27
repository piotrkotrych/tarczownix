#pragma once
#include <Arduino.h>
#include <PCF8574.h>

class RelayManager {
public:
    RelayManager(PCF8574* pcf);
    // Configures the expander pins and brings up the I2C device.
    // Must be called before any set()/commit(); returns false if the expander did not answer.
    bool begin();
    void set(int pin, bool active);
    bool commit();
    uint8_t getShadowRegister() const { return _shadowRegister; }
    bool lastWriteOk() const { return _lastWriteOk; }

private:
    bool isValidPin(int pin) const;
    int pairedPin(int pin) const;

    PCF8574* _pcf;
    uint8_t _shadowRegister;
    bool _dirty;
    bool _lastWriteOk;
    unsigned long _lastFailureLogMs;
};
