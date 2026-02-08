#pragma once
#include <Arduino.h>
#include <PCF8574.h>

class RelayManager {
public:
    RelayManager(PCF8574* pcf);
    void begin();
    void set(int pin, bool active);
    void commit();
    uint8_t getShadowRegister() const { return _shadowRegister; }

private:
    bool isValidPin(int pin) const;
    int pairedPin(int pin) const;

    PCF8574* _pcf;
    uint8_t _shadowRegister;
    bool _dirty;
};
