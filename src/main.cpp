#include "Arduino.h"
#include "PCF8574.h"

// --- Constants ---
#define DEBUG_MODE 1            // Set to 1 to enable detailed debugging
const unsigned long DEBOUNCE_TIME = 100;  // Increased debounce time

// --- Hardware ---
PCF8574 inputs(0x22, 4, 15);   // Input expander, address 0x22
PCF8574 relays(0x24, 4, 15);   // Relay expander, address 0x24

// --- State Machine ---
enum SequenceState {
  IDLE,
  RELAY0_ACTIVE_WAITING,
  RELAY1_ACTIVE_WAITING
};
SequenceState currentState = IDLE;

// --- Input Handling ---
bool debouncedInputStates[6] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
bool lastInputStates[6] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
unsigned long lastInputChangeTime[6] = {0, 0, 0, 0, 0, 0};

// --- Timing ---
unsigned long lastDebugPrint = 0;
unsigned long sequenceStartTime = 0;

// --- Serial Command Handling ---
String serialCommand = "";
bool commandComplete = false;

// --- Function Declarations ---
void setupHardware();
void updateInputs();
void processStateMachine();
void processSerialCommand();
void printDebugInfo();
void printHelp();

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== TARCZOWNIX Control System ===");
  
  setupHardware();
  
  Serial.println("System initialization complete");
  printHelp();
}

void loop() {
  // 1. Process any serial commands
  if (Serial.available() > 0) {
    char inChar = (char)Serial.read();
    
    if (inChar == '\n' || inChar == '\r') {
      commandComplete = true;
    } else {
      serialCommand += inChar;
    }
  }
  
  if (commandComplete) {
    processSerialCommand();
    serialCommand = "";
    commandComplete = false;
  }
  
  // 2. Update input states with debouncing
  updateInputs();
  
  // 3. Process state machine based on debounced inputs
  processStateMachine();
  
  // 4. Print debug information periodically
  if (DEBUG_MODE && millis() - lastDebugPrint > 1000) {
    printDebugInfo();
    lastDebugPrint = millis();
  }
  
  // Short delay to prevent CPU overuse
  delay(5);
}

void setupHardware() {
  // Initialize relay outputs
  Serial.println("Initializing relays...");
  for (int i = 0; i < 6; i++) {
    relays.pinMode(i, OUTPUT);
    relays.digitalWrite(i, HIGH);  // All relays OFF (HIGH)
  }
  
  if (!relays.begin()) {
    Serial.println("ERROR: Relay I2C initialization failed!");
    while(1) delay(100);  // Critical error - halt
  }
  
  // Initialize inputs with pull-ups
  Serial.println("Initializing inputs...");
  for (int i = 0; i < 6; i++) {
    inputs.pinMode(i, INPUT);
    inputs.digitalWrite(i, HIGH);  // Enable pull-up resistors
  }
  
  if (!inputs.begin()) {
    Serial.println("ERROR: Input I2C initialization failed!");
    while(1) delay(100);  // Critical error - halt
  }
  
  Serial.println("Hardware initialization successful");
}

void updateInputs() {
  // Read current input states but apply debouncing
  for (int i = 0; i < 6; i++) {
    bool currentReading = inputs.digitalRead(i);
    
    // If state changed, reset the debounce timer
    if (currentReading != lastInputStates[i]) {
      lastInputChangeTime[i] = millis();
      lastInputStates[i] = currentReading;
    }
    
    // Only update the debounced state if the reading is stable for DEBOUNCE_TIME
    if ((millis() - lastInputChangeTime[i]) > DEBOUNCE_TIME) {
      if (debouncedInputStates[i] != currentReading) {
        debouncedInputStates[i] = currentReading;
        if (DEBUG_MODE) {
          Serial.printf("Input %d changed to %s\n", i, currentReading ? "INACTIVE" : "ACTIVE");
        }
      }
    }
  }
}

void processStateMachine() {
  switch (currentState) {
    case RELAY0_ACTIVE_WAITING:
      // Wait for debounced input 0 to be ACTIVE (LOW)
      if (debouncedInputStates[0] == LOW) {
        // Turn off relay 0 and turn on relay 1
        relays.digitalWrite(0, HIGH);  // OFF
        relays.digitalWrite(1, LOW);   // ON
        currentState = RELAY1_ACTIVE_WAITING;
        Serial.println("STATE: Input 0 activated → Moving to RELAY1_ACTIVE_WAITING");
      }
      break;
      
    case RELAY1_ACTIVE_WAITING:
      // Wait for debounced input 1 to be ACTIVE (LOW)
      if (debouncedInputStates[1] == LOW) {
        // Turn off relay 1 and return to IDLE
        relays.digitalWrite(1, HIGH);  // OFF
        currentState = IDLE;
        Serial.println("STATE: Input 1 activated → Returning to IDLE");
      }
      break;
      
    case IDLE:
      // Nothing to do in idle state
      break;
  }
}

void processSerialCommand() {
  serialCommand.trim();
  Serial.println("Command: " + serialCommand);
  
  if (serialCommand.equalsIgnoreCase("help")) {
    printHelp();
  }
  else if (serialCommand.equalsIgnoreCase("status")) {
    printDebugInfo();
  }
  else if (serialCommand.equalsIgnoreCase("start")) {
    // Reset to start of sequence
    currentState = RELAY0_ACTIVE_WAITING;
    relays.digitalWrite(0, LOW);  // Turn on relay 0 (LOW = ON)
    relays.digitalWrite(1, HIGH); // Make sure relay 1 is OFF
    sequenceStartTime = millis();
    
    Serial.println("ACTION: Starting sequence - Relay 0 activated");
  }
  else if (serialCommand.startsWith("toggle ")) {
    int relayPin = serialCommand.substring(7).toInt();
    
    if (relayPin >= 0 && relayPin < 6) {
      // Toggle the relay state
      int currentState = relays.digitalRead(relayPin);
      relays.digitalWrite(relayPin, !currentState);
      Serial.printf("ACTION: Toggled relay %d to %s\n", relayPin, currentState ? "ON" : "OFF");
    } else {
      Serial.println("ERROR: Invalid relay number. Must be 0-5.");
    }
  }
  else if (serialCommand.equalsIgnoreCase("stop")) {
    // Stop any sequence and return to idle
    currentState = IDLE;
    // Turn off all relays
    for (int i = 0; i < 6; i++) {
      relays.digitalWrite(i, HIGH);  // All relays OFF (HIGH)
    }
    Serial.println("ACTION: Stopped sequence - All relays off");
  }
  else {
    Serial.println("ERROR: Unknown command. Type 'help' for available commands.");
  }
}

void printDebugInfo() {
  Serial.println("\n--- SYSTEM STATUS ---");
  Serial.printf("State: %s\n", 
    currentState == IDLE ? "IDLE" : 
    currentState == RELAY0_ACTIVE_WAITING ? "RELAY0_ACTIVE_WAITING" : 
    "RELAY1_ACTIVE_WAITING");
  
  Serial.print("Raw Inputs: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%d ", inputs.digitalRead(i));
  }
  Serial.println();
  
  Serial.print("Debounced: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%d ", debouncedInputStates[i]);
  }
  Serial.println();
  
  Serial.print("Relays: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%d ", relays.digitalRead(i));
  }
  Serial.println();
  
  Serial.println("-------------------");
}

void printHelp() {
  Serial.println("\n=== AVAILABLE COMMANDS ===");
  Serial.println("help    - Show this help message");
  Serial.println("status  - Show system status");
  Serial.println("start   - Start the sequence");
  Serial.println("stop    - Stop any running sequence");
  Serial.println("toggle X - Toggle relay X (0-5)");
  Serial.println("=========================");
}