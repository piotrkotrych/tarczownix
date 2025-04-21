#include <Arduino.h>
#include <Wire.h>      // Explicitly include Wire
#include <PCF8574.h>   // Include the xreef/PCF8574 library
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdlib.h>    // Required for random()

// --- Hardware Configuration ---
#define PCF_ADDRESS_RELAYS 0x24 // I2C Address for the RELAY PCF8574
#define PCF_ADDRESS_INPUTS 0x22 // I2C Address for the INPUT PCF8574
#define I2C_SDA_PIN 4           // Your SDA pin
#define I2C_SCL_PIN 15          // Your SCL pin

// --- Pin Configuration ---
const int PAIR_COUNT = 3;
const int RELAY_PINS[PAIR_COUNT * 2] = {0, 1, 2, 3, 4, 5}; // Pins on RELAY PCF (0x24)
const int INPUT_PINS[PAIR_COUNT * 2] = {0, 1, 2, 3, 4, 5}; // Pins on INPUT PCF (0x22)

// --- Timing Configuration ---
const int MIN_DELAY_MS = 1500; // Minimum delay after input trigger
const int MAX_DELAY_MS = 4000; // Maximum delay after input trigger

// --- Global Objects ---
PCF8574 pcf_relays(PCF_ADDRESS_RELAYS);
PCF8574 pcf_inputs(PCF_ADDRESS_INPUTS);
SemaphoreHandle_t i2cMutex; // Mutex for thread-safe I2C bus access

// --- Global Control Flag ---
volatile bool sequenceEnabled = false; // <<< ADDED: Start in disabled state

// --- Task Data Structure ---
struct MotorTaskData {
    int pairIndex;
    int relayA;
    int relayB;
    int inputA;
    int inputB;
    bool activeRelayA; // Tracks which relay (A or B) is the target for the next activation
};

// Global array to hold runtime data for all pairs
MotorTaskData motorTaskData[PAIR_COUNT];

// --- Thread-Safe PCF8574 Functions ---
void pcfWriteRelay(uint8_t pin, uint8_t value) {
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
        pcf_relays.digitalWrite(pin, value);
        xSemaphoreGive(i2cMutex);
    } else {
        Serial.printf("ERROR: Failed to get I2C mutex for RELAY write on pin %d\n", pin);
    }
}

uint8_t pcfReadInput(uint8_t pin) {
    uint8_t value = HIGH; // Default to not pressed
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
        value = pcf_inputs.digitalRead(pin);
        xSemaphoreGive(i2cMutex);
    } else {
         Serial.printf("ERROR: Failed to get I2C mutex for INPUT read on pin %d\n", pin);
    }
    return value;
}

// Helper function to stop a relay (set HIGH)
void stopRelay(int relayPin) {
    pcfWriteRelay(relayPin, HIGH);
}

// Helper function to start a relay (set LOW)
void startRelay(int relayPin) {
    pcfWriteRelay(relayPin, LOW);
}

// Helper function to check if an input is pressed (LOW)
bool isInputPressed(int inputPin) {
    return (pcfReadInput(inputPin) == LOW);
}

// --- Motor Control Task ---
void MotorTask(void* pvParameters) {
    MotorTaskData* data = (MotorTaskData*) pvParameters;
    int pairIdx = data->pairIndex;

    Serial.printf("Motor Task %d: Started for Relays [%d,%d], Inputs [%d,%d]\n",
                  pairIdx, data->relayA, data->relayB, data->inputA, data->inputB);

    // Initial state: Assume Relay A should be activated first.
    data->activeRelayA = true;

    while (true) {
        // --- Check if sequence is enabled ---
        if (!sequenceEnabled) {
            // Ensure relays for this pair are OFF if sequence is disabled
            stopRelay(data->relayA);
            stopRelay(data->relayB);
            // Wait and check again
            vTaskDelay(pdMS_TO_TICKS(500)); // Check enabled status periodically
            continue; // Skip the rest of the loop if not enabled
        }

        // --- Sequence is Enabled ---
        int currentRelay;
        int oppositeRelay;
        int currentInput;

        // Determine which relay/input pair should be active based on task data
        if (data->activeRelayA) {
            currentRelay = data->relayA;
            oppositeRelay = data->relayB;
            currentInput = data->inputA;
        } else {
            currentRelay = data->relayB;
            oppositeRelay = data->relayA;
            currentInput = data->inputB;
        }

        // --- Activate the current relay ---
        // Ensure the opposite is off before turning the current one on
        stopRelay(oppositeRelay);
        startRelay(currentRelay);
        Serial.printf("Task %d: Relay %c (Pin %d) ON. Waiting for Input %c (Pin %d)...\n",
                      pairIdx, (data->activeRelayA ? 'A' : 'B'), currentRelay,
                      (data->activeRelayA ? 'A' : 'B'), currentInput);

        // 1. Wait for the corresponding input to be pressed (go LOW)
        while (!isInputPressed(currentInput)) {
            // Also check if sequence got disabled while waiting
            if (!sequenceEnabled) {
                stopRelay(currentRelay); // Turn off relay if disabled mid-wait
                Serial.printf("Task %d: Sequence disabled while waiting for input %c.\n", pairIdx, (data->activeRelayA ? 'A' : 'B'));
                continue; // Restart the loop to check the flag
            }
            vTaskDelay(pdMS_TO_TICKS(50)); // Check every 50ms, yield CPU
        }
        Serial.printf("Task %d: Input %c (Pin %d) PRESSED.\n", pairIdx, (data->activeRelayA ? 'A' : 'B'), currentInput);

        // 2. Stop the current relay
        stopRelay(currentRelay);
        Serial.printf("Task %d: Relay %c (Pin %d) OFF.\n", pairIdx, (data->activeRelayA ? 'A' : 'B'), currentRelay);

        // 3. Wait for a random delay using global constants
        int delayMs = random(MIN_DELAY_MS, MAX_DELAY_MS + 1);
        Serial.printf("Task %d: Delaying for %d ms...\n", pairIdx, delayMs);

        // Check enabled flag periodically during the delay
        TickType_t delayTicks = pdMS_TO_TICKS(delayMs);
        TickType_t startTick = xTaskGetTickCount();
        bool delayInterrupted = false; // Flag to check if delay was cut short
        while ((xTaskGetTickCount() - startTick) < delayTicks) {
            if (!sequenceEnabled) {
                Serial.printf("Task %d: Sequence disabled during delay.\n", pairIdx);
                delayInterrupted = true;
                break; // Exit the delay loop
            }
            vTaskDelay(pdMS_TO_TICKS(50)); // Check flag roughly every 50ms
        }

        Serial.printf("Task %d: Switched direction. Next relay will be %c.\n", pairIdx, (data->activeRelayA ? 'A' : 'B'));
        Serial.println("----------------------------------------");

    } // End while(true) loop
} // End MotorTask function

// --- Setup Function ---
void setup() {
    Serial.begin(115200);
    while (!Serial); // Wait for serial connection
    randomSeed(analogRead(0)); // Seed random number generator
    Serial.println("\n\nESP32 Motor Logic (No Web Server) Starting...");

    // --- Initialize I2C Bus ---
    Serial.printf("Initializing I2C on SDA=%d, SCL=%d... ", I2C_SDA_PIN, I2C_SCL_PIN);
    bool wireOk = Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (!wireOk) {
        Serial.println("Failed!");
        Serial.println("FATAL: Wire.begin() failed. Check I2C pins? Halting.");
        while(1) { vTaskDelay(portMAX_DELAY); }
    }
    Serial.println("OK");

    // --- Create I2C Mutex ---
    i2cMutex = xSemaphoreCreateMutex();
    if (i2cMutex == NULL) {
        Serial.println("FATAL: Failed to create I2C Mutex! Halting.");
        while(1) { vTaskDelay(portMAX_DELAY); }
    }
    Serial.println("I2C Mutex Created.");

    // --- Configure PCF Pins (BEFORE begin()) ---
    Serial.print("Configuring PCF8574 Pins... ");
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
        // Configure all relay pins as OUTPUT and set HIGH (OFF)
        for (int i = 0; i < PAIR_COUNT * 2; i++) {
            pcf_relays.pinMode(RELAY_PINS[i], OUTPUT);
            pcf_relays.digitalWrite(RELAY_PINS[i], HIGH); // Initialize OFF
        }
        // Configure all input pins as INPUT
        for (int i = 0; i < PAIR_COUNT * 2; i++) {
            pcf_inputs.pinMode(INPUT_PINS[i], INPUT); // Use INPUT, ensure external pullups if needed
        }
        xSemaphoreGive(i2cMutex);
        Serial.println("OK (Relays OFF, Inputs as INPUT)");
    } else {
        Serial.println("Failed!");
        Serial.println("FATAL: Failed to get I2C mutex for pin configuration! Halting.");
        while(1) { vTaskDelay(portMAX_DELAY); }
    }

    // --- Initialize PCF8574 Chips (AFTER pin config) ---
    Serial.print("Initializing PCF8574 chips... ");
    bool relayPcfOk = pcf_relays.begin();
    bool inputPcfOk = pcf_inputs.begin();
    if (!relayPcfOk || !inputPcfOk) {
         Serial.println("Failed!");
         Serial.printf(" Relay PCF (0x%02X): %s\n", PCF_ADDRESS_RELAYS, relayPcfOk ? "OK" : "FAILED");
         Serial.printf(" Input PCF (0x%02X): %s\n", PCF_ADDRESS_INPUTS, inputPcfOk ? "OK" : "FAILED");
         Serial.println("FATAL: Halting due to failed PCF chip initialization.");
         Serial.println("Check: Wiring, I2C Addresses, Pull-up Resistors.");
         while(1) { vTaskDelay(portMAX_DELAY); }
    }
     Serial.println("OK");

    // --- Relays are initialized OFF. Tasks will control activation. ---
    Serial.println("Relays initialized OFF.");

    // --- Create Motor Tasks ---
    Serial.println("Creating motor tasks...");
    for (int i = 0; i < PAIR_COUNT; i++) {
        // Populate task data
        motorTaskData[i].pairIndex = i;
        motorTaskData[i].relayA = RELAY_PINS[i * 2];
        motorTaskData[i].relayB = RELAY_PINS[i * 2 + 1];
        motorTaskData[i].inputA = INPUT_PINS[i * 2];
        motorTaskData[i].inputB = INPUT_PINS[i * 2 + 1];
        // activeRelayA will be set to true inside the task initially

        char taskName[20];
        snprintf(taskName, sizeof(taskName), "MotorTask%d", i);

        BaseType_t taskCreated = xTaskCreatePinnedToCore(
            MotorTask,        // Task function
            taskName,         // Task name
            4096,             // Stack size
            &motorTaskData[i], // Task parameter
            1,                // Task priority
            NULL,             // Task handle
            i % 2             // Core pinning
        );

        if (taskCreated != pdPASS) {
            Serial.printf("FATAL: Failed to create Motor Task %d! Error Code: %d\n", i, taskCreated);
             while(1) { vTaskDelay(portMAX_DELAY); } // Halt
        } else {
             Serial.printf(" Motor Task %d created successfully on Core %d.\n", i, i % 2);
        }
    }

    Serial.println("\nSetup complete. All motor tasks created.");
    Serial.println("Tasks will now activate relays and wait for inputs.");
    Serial.println("========================================");
}

// --- Loop Function (Example: Enable sequence via Serial) ---
void loop() {
    // Example: Check Serial input to enable/disable the sequence
    if (Serial.available() > 0) {
        char command = Serial.read();
        if (command == 's' || command == 'S') {
            if (!sequenceEnabled) {
                Serial.println("COMMAND: Enabling sequence!");
                sequenceEnabled = true;
            } else {
                 Serial.println("COMMAND: Sequence already enabled.");
            }
        } else if (command == 'x' || command == 'X') {
             if (sequenceEnabled) {
                Serial.println("COMMAND: Disabling sequence!");
                sequenceEnabled = false;
                // Tasks will stop themselves and turn off relays
            } else {
                 Serial.println("COMMAND: Sequence already disabled.");
            }
        }
    }

    // The main loop doesn't need to do much else with FreeRTOS
    vTaskDelay(pdMS_TO_TICKS(100)); // Small delay
}