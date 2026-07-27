#include "Microphone.h"
#include "../Logic/DebugLogger.h"

Microphone::Microphone(int adcPin)
    : _adcPin(adcPin), _threshold(2000), _callback(nullptr), _taskHandle(nullptr),
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
    _callback = callback;
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
    // Sample a short window and measure how far it swings either side of the baseline.
    const int samples = 128;
    long sum = 0;
    int minValue = 4095;
    int maxValue = 0;
    int lastValue = 0;

    for (int i = 0; i < samples; i++) {
        const int value = analogRead(_adcPin);
        lastValue = value;
        sum += value;
        if (value < minValue) minValue = value;
        if (value > maxValue) maxValue = value;
    }

    const int baseline = _baseline;
    const int aboveBaseline = maxValue - baseline;
    const int belowBaseline = baseline - minValue;
    const int peak = (aboveBaseline > belowBaseline) ? aboveBaseline : belowBaseline;
    const int threshold = _threshold;

    _lastValue = lastValue;
    _lastPeak = peak;
    _lastUpdateMs = millis();

    // Track the microphone bias between windows, never inside one, and never across a
    // loud window: adapting per sample let the baseline chase the gunshot itself and
    // swallowed most of the peak it was supposed to measure.
    if (peak <= threshold) {
        const int windowMean = (int)(sum / samples);
        _baseline = (baseline * 15 + windowMean) / 16;
        return;
    }

    if (millis() - _lastTriggerTime <= DEBOUNCE_MS) {
        return;
    }
    _lastTriggerTime = millis();

    DebugLogger::instance().log("Mic trigger peak=%d threshold=%d baseline=%d", peak, threshold, baseline);
    const GunshotCallback callback = _callback;
    if (callback) {
        callback();
    }
}
