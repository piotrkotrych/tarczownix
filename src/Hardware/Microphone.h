#pragma once
#include <Arduino.h>
#include <functional>

class Microphone {
public:
    using GunshotCallback = std::function<void()>;

    // Analog (ADC) microphone. Default pin is GPIO36 (ADC1_CH0) on many ESP32 boards.
    explicit Microphone(int adcPin = 36);
    void begin();
    void setCallback(GunshotCallback callback);
    void setThreshold(int threshold);

    int getAdcPin() const { return _adcPin; }
    int getThreshold() const { return _threshold; }
    int getBaseline() const { return _baseline; }
    int getLastValue() const { return _lastValue; }
    int getLastPeak() const { return _lastPeak; }
    unsigned long getLastUpdateMs() const { return _lastUpdateMs; }

private:
    int _adcPin;
    int _threshold;
    GunshotCallback _callback;
    portMUX_TYPE _callbackMux;
    TaskHandle_t _taskHandle;
    unsigned long _lastTriggerTime;
    const unsigned long DEBOUNCE_MS = 200;
    int _baseline;

    volatile int _lastValue;
    volatile int _lastPeak;
    volatile unsigned long _lastUpdateMs;

    static void _micTask(void* parameter);
    void _processAudio();
};
