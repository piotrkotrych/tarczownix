#pragma once
#include <Arduino.h>

class Microphone {
public:
    // Plain function pointer rather than std::function: the callback is read from the
    // sampling task, and copying a std::function there would mean allocating inside a
    // critical section.
    using GunshotCallback = void (*)();

    // Analog (ADC) microphone. Default pin is GPIO36 (ADC1_CH0) on many ESP32 boards.
    // ADC1 is required - ADC2 is unusable while WiFi is active.
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
    volatile int _threshold;
    // 32-bit aligned scalars: assignment is atomic on ESP32, so no lock is needed for
    // the single-writer/single-reader access pattern used here.
    GunshotCallback volatile _callback;
    TaskHandle_t _taskHandle;
    unsigned long _lastTriggerTime;
    static const unsigned long DEBOUNCE_MS = 200;
    volatile int _baseline;

    volatile int _lastValue;
    volatile int _lastPeak;
    volatile unsigned long _lastUpdateMs;

    static void _micTask(void* parameter);
    void _processAudio();
};
