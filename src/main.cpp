#include "Arduino.h"
#include "PCF8574.h"
#include <WiFi.h>
#include <WiFiAP.h>
#include <WebServer.h>

// --- Constants ---
#define DEBUG_MODE 1            // Set to 1 to enable detailed debugging
const char* ssid = "ESP32-AccessPoint";
const char* password = "password";
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

// --- Web Server ---
WebServer server(80);

// --- Input Handling ---
bool debouncedInputStates[6] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
bool lastInputStates[6] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
unsigned long lastInputChangeTime[6] = {0, 0, 0, 0, 0, 0};

// --- Timing ---
unsigned long lastDebugPrint = 0;
unsigned long sequenceStartTime = 0;

// --- Function Declarations ---
void setupHardware();
void setupWiFi();
void updateInputs();
void handleRoot();
void handleToggleRelay();
void handleStartSequence();
void processStateMachine();
void printDebugInfo();

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== TARCZOWNIX Control System ===");
  
  setupHardware();
  setupWiFi();
  
  // Set up web server routes
  server.on("/", handleRoot);
  server.on("/toggle", handleToggleRelay);
  server.on("/start", handleStartSequence);
  server.begin();
  
  Serial.println("System initialization complete");
}

void loop() {
  // 1. Handle web client requests
  server.handleClient();
  
  // 2. Update input states with debouncing (separated from relay control)
  updateInputs();
  
  // 3. Process state machine based on debounced inputs (not direct reads)
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

void setupWiFi() {
  Serial.println("Setting up WiFi access point...");
  WiFi.softAP(ssid, password);
  
  // Configure static IP
  IPAddress local_ip(192, 168, 1, 111);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  
  Serial.print("Access point IP: ");
  Serial.println(WiFi.softAPIP());
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

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<title>TARCZOWNIX Control</title>";
  html += "<style>";
  html += "body{font-family:Arial;text-align:center;margin:0;padding:20px;background-color:#f5f5f5;}";
  html += ".container{max-width:800px;margin:0 auto;background-color:white;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
  html += ".status{margin:20px 0;padding:15px;border:1px solid #ddd;border-radius:5px;}";
  html += ".on{color:green;font-weight:bold;}";
  html += ".off{color:red;}";
  html += "button{background-color:#4CAF50;border:none;color:white;padding:10px 20px;text-align:center;";
  html += "text-decoration:none;display:inline-block;font-size:16px;margin:4px 2px;cursor:pointer;border-radius:5px;}";
  html += ".startbtn{background-color:#007bff;padding:12px 25px;font-size:18px;}";
  html += "table{width:100%;border-collapse:collapse;}";
  html += "th,td{padding:10px;text-align:left;border-bottom:1px solid #ddd;}";
  html += "</style>";
  html += "</head><body><div class=\"container\">";
  html += "<h1>TARCZOWNIX Control System</h1>";
  
  // Sequence Status
  html += "<div class=\"status\"><h2>Sequence Status</h2>";
  html += "<p>Current state: ";
  switch (currentState) {
    case IDLE:
      html += "<span>IDLE</span>";
      break;
    case RELAY0_ACTIVE_WAITING:
      html += "<span class=\"on\">RELAY 0 ACTIVE - Waiting for Input 0</span>";
      break;
    case RELAY1_ACTIVE_WAITING:
      html += "<span class=\"on\">RELAY 1 ACTIVE - Waiting for Input 1</span>";
      break;
  }
  html += "</p>";
  html += "<a href=\"/start\"><button class=\"startbtn\">Start Sequence</button></a>";
  html += "</div>";
  
  // Input status table
  html += "<div class=\"status\"><h2>Input Status</h2><table>";
  html += "<tr><th>Input</th><th>State</th></tr>";
  for (int i = 0; i < 6; i++) {
    html += "<tr><td>Input P" + String(i) + "</td>";
    String spanClass = debouncedInputStates[i] ? "off" : "on";
    String status = debouncedInputStates[i] ? "INACTIVE" : "ACTIVE";
    html += "<td><span class=\"" + spanClass + "\">" + status + "</span></td></tr>";
  }
  html += "</table></div>";
  
  // Relay status and control table
  html += "<div class=\"status\"><h2>Relay Control</h2><table>";
  html += "<tr><th>Relay</th><th>State</th><th>Action</th></tr>";
  for (int i = 0; i < 6; i++) {
    int value = relays.digitalRead(i);
    html += "<tr><td>Relay P" + String(i) + "</td>";
    String statusClass = (value == 0) ? "on" : "off";
    String statusText = (value == 0) ? "ON" : "OFF";
    html += "<td><span class=\"" + statusClass + "\">" + statusText + "</span></td>";
    html += "<td><a href=\"/toggle?relay=" + String(i) + "\"><button>Toggle</button></a></td></tr>";
  }
  html += "</table></div>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleToggleRelay() {
  String relayParam = server.arg("relay");
  int relayPin = relayParam.toInt();
  
  if (relayPin >= 0 && relayPin < 6) {
    // Toggle the relay state
    int currentState = relays.digitalRead(relayPin);
    relays.digitalWrite(relayPin, !currentState);
    Serial.printf("ACTION: Toggled relay %d to %s\n", relayPin, currentState ? "ON" : "OFF");
  }
  
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleStartSequence() {
  // Reset to start of sequence
  currentState = RELAY0_ACTIVE_WAITING;
  relays.digitalWrite(0, LOW);  // Turn on relay 0 (LOW = ON)
  relays.digitalWrite(1, HIGH); // Make sure relay 1 is OFF
  sequenceStartTime = millis();
  
  Serial.println("ACTION: Starting sequence - Relay 0 activated");
  
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void printDebugInfo() {
  Serial.println("\n--- DEBUG INFO ---");
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
  
  Serial.println("-----------------");
}