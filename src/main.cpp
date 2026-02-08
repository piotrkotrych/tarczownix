#include <Arduino.h>
#include <PCF8574.h>
#include "Hardware/InputManager.h"
#include "Hardware/RelayManager.h"
#include "Hardware/Target.h"
#include "Hardware/Microphone.h"
#include "Logic/GameManager.h"
#include "Logic/SettingsManager.h"
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

static const char* toStateString(TargetState state) {
    switch (state) {
        case HIDDEN: return "HIDDEN";
        case MOVING_SHOW: return "MOVING_SHOW";
        case SHOWN: return "SHOWN";
        case MOVING_HIDE: return "MOVING_HIDE";
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
    Wire.begin(4, 15);
    if (!pcfInputs.begin()) {
        Serial.println("ERROR: Input PCF8574 init failed!");
    }
    if (!pcfRelays.begin()) {
        Serial.println("ERROR: Relay PCF8574 init failed!");
    }

    inputManager.begin();
    relayManager.begin();

     // 3. Initialize Microphone
     microphone.begin(); // initialize microphone (uses its default/configured ADC pin)
     microphone.setThreshold(settingsManager.getConfig().micThreshold);
     microphone.setCallback([]() {
         // Trigger competition mode gunshot event
         if (gameManager.getCompetitionMode()) {
             gameManager.getCompetitionMode()->onGunshot();
         }
     });

    // 4. Initialize Network
    networkManager.setSettingsManager(&settingsManager);
    networkManager.setMicrophone(&microphone);
    networkManager.setGameManager(&gameManager);
    networkManager.setTargets(&target1, &target2, &target3);
    networkManager.setInputManager(&inputManager);
    networkManager.setRelayManager(&relayManager);
    networkManager.begin();
    networkManager.setCommandCallback([](int targetId, String action) {
        gameManager.handleWebInput(targetId, action);
    });

    // 5. Initialize Game Manager
    // (Optional: set default mode)
    gameManager.setMode("manual"); // Start in manual mode by default

    Serial.println("Setup Complete.");
}

void loop() {
    // Update subsystems
    inputManager.update();
    // relayManager.update(); // RelayManager commits immediately in set(), but if we add buffering later...
    
    networkManager.update();
    gameManager.update();
    
    // Broadcast status on state change (and at least once per second)
    static TargetState last1 = (TargetState)-1;
    static TargetState last2 = (TargetState)-1;
    static TargetState last3 = (TargetState)-1;
    static unsigned long lastStatusSent = 0;

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
        networkManager.broadcastStatus(json);

        last1 = s1;
        last2 = s2;
        last3 = s3;
        lastStatusSent = millis();
    }

    delay(5); // Small yield
}
