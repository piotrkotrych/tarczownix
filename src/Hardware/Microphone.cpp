#include "Microphone.h"
#include "../Logic/DebugLogger.h"

Microphone::Microphone(int adcPin)
    : _adcPin(adcPin), _threshold(2000), _callbackMux(portMUX_INITIALIZER_UNLOCKED), _taskHandle(nullptr),
      _lastTriggerTime(0), _baseline(2048), _lastValue(0), _lastPeak(0), _lastUpdateMs(0) {
}

void Microphone::begin() {
    if (_taskHandle != nullptr) {
        return;
    }

    pinMode(_adcPin, INPUT);

    // ESP32 Arduino defaults to 12-bit in many cores, but set explicitly.
    analogReadResolution(12);
    // Wider input range (useful when microphone bias is near mid-supply).
    analogSetPinAttenuation(_adcPin, ADC_11db);

    // Prime baseline with a few samples
    long sum = 0;
    const int warmupSamples = 32;
    for (int i = 0; i < warmupSamples; i++) {
        sum += analogRead(_adcPin);
        delay(2);
    }
    _baseline = (int)(sum / warmupSamples);

    xTaskCreatePinnedToCore(
        _micTask,
        "MicTask",
        3072,
        this,
        1,
        &_taskHandle,
        0
    );
}

void Microphone::setCallback(GunshotCallback callback) {
    portENTER_CRITICAL(&_callbackMux);
    _callback = callback;
    portEXIT_CRITICAL(&_callbackMux);
}

void Microphone::setThreshold(int threshold) {
    _threshold = threshold;
}

void Microphone::_micTask(void* parameter) {
    Microphone* mic = (Microphone*)parameter;
    while (true) {
        mic->_processAudio();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void Microphone::_processAudio() {
    // Sample a short window and compute max deviation from baseline.
    int peak = 0;
    const int samples = 128;
    int lastValue = 0;
    for (int i = 0; i < samples; i++) {
        const int value = analogRead(_adcPin);
        lastValue = value;

        // Track slowly moving baseline (mic bias) via simple low-pass filter.
        _baseline = (_baseline * 31 + value) / 32;

        const int deviation = abs(value - _baseline);
        if (deviation > peak) {
            peak = deviation;
        }
    }

    _lastValue = lastValue;
    _lastPeak = peak;
    _lastUpdateMs = millis();

    if (peak > _threshold) {
        if (millis() - _lastTriggerTime > DEBOUNCE_MS) {
            _lastTriggerTime = millis();
            GunshotCallback callbackCopy;
            portENTER_CRITICAL(&_callbackMux);
            callbackCopy = _callback;
            portEXIT_CRITICAL(&_callbackMux);
            DebugLogger::instance().log("Mic trigger peak=%d threshold=%d baseline=%d", peak, _threshold, _baseline);
            if (callbackCopy) {
                callbackCopy();
            }
        }
    }
}
