#include <Arduino.h>
#include <PCF8574.h>
#include "Hardware/InputManager.h"
#include "Hardware/RelayManager.h"
#include "Hardware/Target.h"
#include "Hardware/Microphone.h"
#include "Logic/GameManager.h"
#include "Logic/SettingsManager.h"
#include "Logic/DebugLogger.h"
#include "Network/NetworkManager.h"

 // --- Hardware Instances ---
 PCF8574 pcfInputs(0x22, 4, 15);
 PCF8574 pcfRelays(0x24, 4, 15);
 
 InputManager inputManager(&pcfInputs);
 RelayManager relayManager(&pcfRelays);
 Microphone microphone; // will initialize in setup with pin 36 (ANALOG_A1)
 
 // --- Targets ---
// Target 1: Show=Relay0, Hide=Relay1, SensorShown=Input0, SensorHidden=Input1
Target target1(&relayManager, &inputManager, 0, 1, 0, 1);
// Target 2: Show=Relay2, Hide=Relay3, SensorShown=Input2, SensorHidden=Input3
Target target2(&relayManager, &inputManager, 2, 3, 2, 3);
// Target 3: Show=Relay4, Hide=Relay5, SensorShown=Input4, SensorHidden=Input5
Target target3(&relayManager, &inputManager, 4, 5, 4, 5);

// --- Managers ---
GameManager gameManager(&target1, &target2, &target3);
NetworkManager networkManager;
SettingsManager settingsManager;
static bool hardwareReady = false;

static volatile bool gunshotPending = false;
static portMUX_TYPE gunshotMux = portMUX_INITIALIZER_UNLOCKED;

static void setGunshotPending() {
    portENTER_CRITICAL(&gunshotMux);
    gunshotPending = true;
    portEXIT_CRITICAL(&gunshotMux);
}

static bool takeGunshotPending() {
    bool pending = false;
    portENTER_CRITICAL(&gunshotMux);
    pending = gunshotPending;
    gunshotPending = false;
    portEXIT_CRITICAL(&gunshotMux);
    return pending;
}

static const char* toStateString(TargetState state) {
    switch (state) {
        case HIDDEN: return "HIDDEN";
        case MOVING_SHOW: return "MOVING_SHOW";
        case SHOWN: return "SHOWN";
        case MOVING_HIDE: return "MOVING_HIDE";
        case STOPPED: return "STOPPED";
        case ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("--- Tarczownix Modular v2.0 ---");

    // 1. Initialize Settings
    settingsManager.begin();
    
    // 2. Initialize I2C / PCF8574
    // The managers own the expander bring-up: PCF8574::begin() must run *after* the pin
    // modes are declared, otherwise the initial safe state is never written, inputs read
    // as permanently active, and begin() reports failure unconditionally.
    Wire.begin(4, 15);
    const bool inputsOk = inputManager.begin();
    if (!inputsOk) {
        Serial.println("ERROR: Input PCF8574 init failed!");
    }
    const bool relaysOk = relayManager.begin();
    if (!relaysOk) {
        Serial.println("ERROR: Relay PCF8574 init failed!");
    }
    hardwareReady = inputsOk && relaysOk;

    if (!hardwareReady) {
        DebugLogger::instance().log("Hardware init failed; movement commands disabled");
    }

     // 3. Initialize Microphone
     microphone.begin(); // initialize microphone (uses its default/configured ADC pin)
     microphone.setThreshold(settingsManager.getConfig().micThreshold);
     microphone.setCallback([]() {
         setGunshotPending();
     });

    // 4. Initialize Network
    networkManager.setSettingsManager(&settingsManager);
    networkManager.setMicrophone(&microphone);
    networkManager.setGameManager(&gameManager);
    networkManager.setTargets(&target1, &target2, &target3);
    networkManager.setInputManager(&inputManager);
    networkManager.setRelayManager(&relayManager);
    networkManager.begin();

    // 5. Initialize Game Manager
    gameManager.applyConfig(settingsManager.getConfig());
    // (Optional: set default mode)
    gameManager.setMode("manual"); // Start in manual mode by default

    Serial.println("Setup Complete.");
}

void loop() {
    // Update subsystems
    inputManager.update();
    // relayManager.update(); // RelayManager commits immediately in set(), but if we add buffering later...
    
    networkManager.update();

    Config pendingConfig;
    if (networkManager.takePendingConfig(pendingConfig)) {
        settingsManager.setConfig(pendingConfig);
        settingsManager.save();
        microphone.setThreshold(settingsManager.getConfig().micThreshold);
        gameManager.applyConfig(settingsManager.getConfig());
        DebugLogger::instance().log("Settings update applied");
    }

    int targetId = 0;
    String action;
    while (networkManager.getNextCommand(targetId, action)) {
        // With the expanders down, refuse anything that would energise a motor. Mode
        // changes, stop and reset stay available so the operator can still work the UI
        // and see what is wrong.
        if (!hardwareReady && (action == "show" || action == "hide")) {
            DebugLogger::instance().log("Movement command ignored: hardware unavailable");
            continue;
        }
        gameManager.handleWebInput(targetId, action);
    }

    if (takeGunshotPending()) {
        gameManager.requestGunshot();
    }

    gameManager.update();
    relayManager.commit();


    // Broadcast status on state change (and at least once per second)
    static TargetState last1 = (TargetState)-1;
    static TargetState last2 = (TargetState)-1;
    static TargetState last3 = (TargetState)-1;
    static unsigned long lastStatusSent = 0;
    static unsigned long lastDiagSent = 0;
    static unsigned long lastLogsSent = 0;
    static unsigned long lastLogSequence = 0;

    // Telemetry is only worth building when somebody is listening; otherwise the device
    // serialises diagnostics JSON twice a second forever with nowhere to send it.
    if (networkManager.hasClients()) {
        const TargetState s1 = target1.getState();
        const TargetState s2 = target2.getState();
        const TargetState s3 = target3.getState();

        const bool changed = (s1 != last1) || (s2 != last2) || (s3 != last3);
        if (changed || (millis() - lastStatusSent > 1000)) {
            JsonDocument doc;
            doc["1"] = toStateString(s1);
            doc["2"] = toStateString(s2);
            doc["3"] = toStateString(s3);
            String json;
            serializeJson(doc, json);
            networkManager.broadcastEvent("status", json);

            last1 = s1;
            last2 = s2;
            last3 = s3;
            lastStatusSent = millis();
        }

        if (millis() - lastDiagSent > 500) {
            networkManager.broadcastEvent("diagnostics", networkManager.getDiagnosticsJson());
            lastDiagSent = millis();
        }

        const unsigned long logSequence = DebugLogger::instance().getSequence();
        if (logSequence != lastLogSequence || (millis() - lastLogsSent > 2000)) {
            networkManager.broadcastEvent("logs", networkManager.getLogsJson());
            lastLogSequence = logSequence;
            lastLogsSent = millis();
        }
    } else {
        // Make sure the next client to connect gets a full snapshot straight away.
        last1 = (TargetState)-1;
        last2 = (TargetState)-1;
        last3 = (TargetState)-1;
    }

    delay(5); // Small yield
}
