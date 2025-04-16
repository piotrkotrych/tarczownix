#include <Arduino.h>
#include <Wire.h> // Include the Wire library for I2C communication
#include <PCF8574.h> // Include the xreef/PCF8574 library
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// --- Hardware Configuration ---
#define PCF_ADDRESS_RELAYS 0x24 // I2C Address for the RELAY PCF8574
#define PCF_ADDRESS_INPUTS 0x22 // I2C Address for the INPUT PCF8574

const int PAIR_COUNT = 3;
// These arrays now refer to pin numbers (0-7) on their *respective* PCF chips
const int RELAY_PINS[PAIR_COUNT * 2] = {0, 1, 2, 3, 4, 5}; // Pins 0-5 on the RELAY PCF (0x24)
const int INPUT_PINS[PAIR_COUNT * 2] = {0, 1, 2, 3, 4, 5}; // Pins 0-5 on the INPUT PCF (0x22)

// --- Test Configuration ---
// NOTE: Using placeholder values. Integrate webserver config if needed.
const int MIN_DELAY_MS = 1500; // Minimum random delay in milliseconds
const int MAX_DELAY_MS = 4000; // Maximum random delay in milliseconds

// --- Global Objects (using xreef/PCF8574 library) ---
PCF8574 pcf_relays(PCF_ADDRESS_RELAYS, 4, 15); // PCF object for relays
PCF8574 pcf_inputs(PCF_ADDRESS_INPUTS, 4, 15); // PCF object for inputs
SemaphoreHandle_t i2cMutex; // Mutex for thread-safe I2C bus access

// --- Task Data Structure ---
struct MotorTaskData {
    int pairIndex;         // Index (0, 1, 2)
    int relayA;            // Pin number for relay A on pcf_relays
    int relayB;            // Pin number for relay B on pcf_relays
    int inputA;            // Pin number for input A on pcf_inputs
    int inputB;            // Pin number for input B on pcf_inputs
    bool activeRelayA;     // Which relay is currently active (or should be)
};

// Global array to hold runtime data for all pairs
MotorTaskData motorTaskData[PAIR_COUNT];

// --- Thread-Safe PCF8574 Functions ---
// IMPORTANT: Assume relays are ACTIVE LOW (LOW = ON, HIGH = OFF)
// IMPORTANT: Assume inputs are pulled HIGH (INPUT_PULLUP), LOW when pressed.

// Writes ONLY to the RELAY PCF8574 (0x24)
void pcfWriteRelay(uint8_t pin, uint8_t value) {
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
        pcf_relays.digitalWrite(pin, value); // Correct usage for xreef/PCF8574
        xSemaphoreGive(i2cMutex);
    } else {
        Serial.printf("ERROR: Failed to get I2C mutex for RELAY write on pin %d\n", pin);
    }
}

// Reads ONLY from the INPUT PCF8574 (0x22)
uint8_t pcfReadInput(uint8_t pin) {
    uint8_t value = HIGH; // Default to non-pressed state (HIGH due to pull-up)
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
        value = pcf_inputs.digitalRead(pin); // Correct usage for xreef/PCF8574
        xSemaphoreGive(i2cMutex);
    } else {
         Serial.printf("ERROR: Failed to get I2C mutex for INPUT read on pin %d\n", pin);
    }
    return value;
}

// Turns the specified relay OFF (using pcf_relays)
void stopRelay(int relayPin) {
    pcfWriteRelay(relayPin, HIGH); // HIGH turns OFF Active LOW relays
}

// Turns the specified relay ON (using pcf_relays)
void startRelay(int relayPin) {
    pcfWriteRelay(relayPin, LOW); // LOW turns ON Active LOW relays
}

// Checks if the specified input pin is LOW (pressed) (using pcf_inputs)
bool isInputPressed(int inputPin) {
    // Read the input value and check if it's LOW
    return (pcfReadInput(inputPin) == LOW);
}

// --- Motor Control Task ---
void MotorTask(void* pvParameters) {
    MotorTaskData* data = (MotorTaskData*) pvParameters;
    int pairIdx = data->pairIndex;

    Serial.printf("Motor Task %d: Started for Relays [%d,%d] (on 0x%02X), Inputs [%d,%d] (on 0x%02X)\n",
                  pairIdx, data->relayA, data->relayB, PCF_ADDRESS_RELAYS,
                  data->inputA, data->inputB, PCF_ADDRESS_INPUTS);

    // Initial state (activeRelayA = true) is set in setup() before task creation

    while (true) {
        int currentRelay;
        int oppositeRelay;
        int currentInput;

        // Determine which relay/input pair is currently active based on task data
        if (data->activeRelayA) {
            currentRelay = data->relayA;
            oppositeRelay = data->relayB;
            currentInput = data->inputA;
        } else {
            currentRelay = data->relayB;
            oppositeRelay = data->relayA;
            currentInput = data->inputB;
        }

        // The relay (currentRelay) should already be ON from the previous iteration or setup.
        Serial.printf("Task %d: Relay %d ON. Waiting for Input %d (Pin %d on 0x%02X)...\n",
                      pairIdx, currentRelay, (data->activeRelayA ? 'A' : 'B'), currentInput, PCF_ADDRESS_INPUTS);

        // 1. Wait for the corresponding input to be pressed (go LOW)
        while (!isInputPressed(currentInput)) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Check every 50ms, yield CPU
        }
        Serial.printf("Task %d: Input %d (Pin %d) PRESSED.\n", pairIdx, (data->activeRelayA ? 'A' : 'B'), currentInput);

        // 2. Stop the current relay
        stopRelay(currentRelay);
        Serial.printf("Task %d: Relay %d (Pin %d) OFF.\n", pairIdx, (data->activeRelayA ? 'A' : 'B'), currentRelay);

        // 3. Wait for a random delay
        int delayMs = random(MIN_DELAY_MS, MAX_DELAY_MS + 1);
        Serial.printf("Task %d: Delaying for %d ms...\n", pairIdx, delayMs);
        vTaskDelay(pdMS_TO_TICKS(delayMs));

        // 4. Switch direction state for the next iteration
        data->activeRelayA = !data->activeRelayA;

        // Determine the *next* relay to activate based on the new state
        int nextRelay = data->activeRelayA ? data->relayA : data->relayB;
        int nextOpposite = data->activeRelayA ? data->relayB : data->relayA;

        // 5. Activate the *next* relay
        stopRelay(nextOpposite); // Ensure the one that just turned off stays off (or was already off)
        startRelay(nextRelay);   // Turn on the next relay in the sequence
        Serial.printf("Task %d: Switched. Relay %d (Pin %d) ON for next cycle.\n", pairIdx, (data->activeRelayA ? 'A' : 'B'), nextRelay);
        Serial.println("----------------------------------------");

    } // End while(true) loop
}

// --- Setup Function ---
void setup() {
    Serial.begin(115200);
    while (!Serial); // Wait for serial connection (optional)
    randomSeed(analogRead(0)); // Seed random number generator early
    Serial.println("\n\nESP32 Motor Logic Test (Dual PCF8574) Starting...");

    // --- Initialize I2C ---
    Wire.begin(); // Initialize I2C bus (SDA, SCL default pins)

    // --- Initialize BOTH PCF8574 chips ---
    pcf_relays.begin(); // Initialize relay expander
    pcf_inputs.begin(); // Initialize input expander

    // Check connections using Wire library communication check
    Wire.beginTransmission(PCF_ADDRESS_RELAYS);
    byte error_relays = Wire.endTransmission();
    bool relayPcfOk = (error_relays == 0);

    Wire.beginTransmission(PCF_ADDRESS_INPUTS);
    byte error_inputs = Wire.endTransmission();
    bool inputPcfOk = (error_inputs == 0);

    Serial.printf("Relay PCF8574 (0x%02X) responded: %s (Error Code: %d)\n", PCF_ADDRESS_RELAYS, relayPcfOk ? "YES" : "NO", error_relays);
    Serial.printf("Input PCF8574 (0x%02X) responded: %s (Error Code: %d)\n", PCF_ADDRESS_INPUTS, inputPcfOk ? "YES" : "NO", error_inputs);

    // Halt if any PCF chip did not respond
    if (!relayPcfOk || !inputPcfOk) {
        Serial.println("FATAL: Halting due to non-responsive PCF chip(s). Check wiring & addresses.");
        while(1) { vTaskDelay(portMAX_DELAY); } // Halt execution indefinitely
    }

    // --- Create I2C Mutex ---
    i2cMutex = xSemaphoreCreateMutex();
    if (i2cMutex == NULL) {
        Serial.println("FATAL: Failed to create I2C Mutex!");
        while(1) { vTaskDelay(portMAX_DELAY); } // Halt
    }
    Serial.println("I2C Mutex Created.");

    // Configure PCF pins *after* creating mutex and confirming connection
    Serial.println("Configuring PCF8574 Pins...");
    for (int i = 0; i < PAIR_COUNT * 2; i++) {
        // Configure relay pins on the RELAY PCF (0x24)
        pcf_relays.pinMode(RELAY_PINS[i], OUTPUT); // Set pin as OUTPUT
        pcfWriteRelay(RELAY_PINS[i], HIGH);       // Initialize relay to OFF (Active LOW)

        // Configure input pins on the INPUT PCF (0x22)
        pcf_inputs.pinMode(INPUT_PINS[i], INPUT_PULLUP); // Set pin as INPUT with pull-up
    }
     Serial.println("PCF8574 Pins Configured.");

    // --- Start Initial Relays Immediately ---
    Serial.println("Starting initial relays (Motor A for each pair)...");
    for (int i = 0; i < PAIR_COUNT; i++) {
        int relayA = RELAY_PINS[i * 2];
        int relayB = RELAY_PINS[i * 2 + 1];
        // Ensure B is OFF first (redundant given initialization, but safe)
        pcfWriteRelay(relayB, HIGH);
        // Turn A ON
        pcfWriteRelay(relayA, LOW);
        Serial.printf("  Pair %d: Relay A (Pin %d) turned ON.\n", i, relayA);
    }
    Serial.println("Initial relays started.");
    // --- End Start Initial Relays ---


    // --- Create Motor Tasks ---
    Serial.println("Creating motor tasks...");
    for (int i = 0; i < PAIR_COUNT; i++) {
        // Populate task data
        motorTaskData[i].pairIndex = i;
        motorTaskData[i].relayA = RELAY_PINS[i * 2];
        motorTaskData[i].relayB = RELAY_PINS[i * 2 + 1];
        motorTaskData[i].inputA = INPUT_PINS[i * 2];
        motorTaskData[i].inputB = INPUT_PINS[i * 2 + 1];
        motorTaskData[i].activeRelayA = true; // Reflects the initial state set above

        char taskName[20];
        snprintf(taskName, sizeof(taskName), "MotorTask%d", i);

        // Create the task
        BaseType_t taskCreated = xTaskCreatePinnedToCore(
            MotorTask,        // Task function
            taskName,         // Task name
            4096,             // Stack size (consider increasing if complex operations are added)
            &motorTaskData[i], // Task parameter
            1,                // Task priority (1 is low)
            NULL,             // Task handle (not stored)
            i % 2             // Core pinning (alternates between 0 and 1)
        );

        if (taskCreated != pdPASS) {
            Serial.printf("FATAL: Failed to create Motor Task %d! Error Code: %d\n", i, taskCreated);
             while(1) { vTaskDelay(portMAX_DELAY); } // Halt
        } else {
             Serial.printf(" Motor Task %d created successfully on Core %d.\n", i, i % 2);
        }
    }

    Serial.println("\nSetup complete. All motor tasks running.");
    Serial.println("Monitor the output below. Press the corresponding input button to trigger state changes.");
    Serial.println("========================================");
}

// --- Loop Function (Unused with FreeRTOS) ---
void loop() {
    // The main logic runs in FreeRTOS tasks, so this loop is not used.
    // Sleep indefinitely to yield CPU time to other tasks.
    vTaskDelay(portMAX_DELAY);
}