#include "Arduino.h"
#include "PCF8574.h"
// #include <AsyncTCP.h>
#include <WiFi.h>
#include <WiFiAP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>

#define ANALOG_A1 36

// Motor control state structure for better organization
struct MotorState {
  unsigned long delayStartTime = 0;
  bool waitingForInput = false;
  bool isActive = false;
  int currentRandomDelay = 0; // Store the current random delay for this relay
};

// Array of motor states for cleaner code
MotorState motorStates[6];

// System state tracking
enum SystemState {
  SYSTEM_STOPPED,
  SYSTEM_RUNNING,
  SYSTEM_ERROR
};
SystemState systemState = SYSTEM_STOPPED;

// Operating modes
enum OperatingMode {
  MODE_SEQUENCE,  // Original sequence mode
  MODE_CUSTOM     // Custom program mode
};
OperatingMode currentMode = MODE_SEQUENCE;

// Custom program structures - redesigned for motor pairs
struct MotorPairBlock {
  int pairId;           // 0, 1, or 2 (for pairs 0-1, 2-3, 4-5)
  int delayMs;          // Delay when first input is triggered  
  bool waitForInput;    // Whether to wait for input before proceeding to next block
};

struct PairProgram {
  String name;
  bool loopEnabled;
  bool waitForMicTrigger; // Whether this program waits for mic trigger to start
  float micThreshold;     // Mic threshold for this program (if waitForMicTrigger is true)
  int blockCount;
  MotorPairBlock blocks[10]; // Maximum 10 pair blocks per program
};

// Pair execution states
enum PairState {
  PAIR_IDLE,
  PAIR_FIRST_RELAY,     // First relay activated, waiting for input
  PAIR_DELAY,           // Delay period after first input
  PAIR_SECOND_RELAY,    // Second relay activated, waiting for input
  PAIR_COMPLETE         // Block finished, ready for next
};

// Current pair-based program execution state
PairProgram currentPairProgram;
int currentBlockIndex = 0;
bool pairProgramRunning = false;
bool waitingForCustomMicTrigger = false; // Whether we're waiting for mic to start custom program
PairState currentPairState = PAIR_IDLE;
int activePairId = -1;
unsigned long pairDelayStartTime = 0;
unsigned long pairStateStartTime = 0;

// --- Hardware ---
PCF8574 inputs(0x22, 4, 15);   // Input expander, address 0x22
PCF8574 relays(0x24, 4, 15);   // Relay expander, address 0x24

Preferences preferences;

// --- WiFi Credentials ---
const char* ssid = "ESP32-Access-Point"; // SSID for the access point
const char* password = "pass"; // Password for the access point


// Separate delay configurations for each relay
int minDelayRelay[6] = {1000, 1000, 1000, 1000, 1000, 1000}; // Default min delay for each relay
int maxDelayRelay[6] = {5000, 5000, 5000, 5000, 5000, 5000}; // Default max delay for each relay

// Safety timeout configuration (in milliseconds)
int safetyTimeoutMs = 1000; // Default 1 second timeout

// Microphone dB threshold configuration
float micDbThreshold = 50.0; // Default 50 dBA threshold

// --- Web Server ---
AsyncWebServer server(80); // Create a web server on port 80

// --- Function Declarations ---
int getRandomDelay(int relay);
void saveRelayDelays();
void loadRelayDelays();
void saveSafetyTimeout();
void loadSafetyTimeout();
void saveMicDbThreshold();
void loadMicDbThreshold();

// Custom program functions - updated for pair-based programming
bool savePairProgram(const String& programName, const String& programData);
String loadPairProgram(const String& programName);
String getPairProgramList();
bool deletePairProgram(const String& programName);
bool validatePairProgramSafety(const String& programData);
void executePairProgram();
void stopPairProgram();
int getPairFirstRelay(int pairId);
int getPairSecondRelay(int pairId);

// Add these global variables at the top with your other variables
unsigned long inputTimeoutStart[6] = {0}; // Track when each relay was turned on
bool inputTimeoutActive[6] = {false};     // Track which relays are waiting for input
String lastErrorMessage = "";             // Store last error for web display
unsigned long lastErrorTime = 0;         // When the last error occurred

// Enhanced I2C communication with error checking
bool safeRelayWrite(int pin, int value) {
  bool success = relays.digitalWrite(pin, value);
  if (!success) {
    lastErrorMessage = "I2C communication error with relay " + String(pin);
    lastErrorTime = millis();
    Serial.println("ERROR: " + lastErrorMessage);
    systemState = SYSTEM_ERROR;
  }
  return success;
}

bool safeInputRead(int pin) {
  // Add debouncing for more reliable input reading
  static unsigned long lastReadTime[6] = {0};
  static bool lastState[6] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
  
  unsigned long currentTime = millis();
  if (currentTime - lastReadTime[pin] < 50) { // 50ms debounce
    return lastState[pin];
  }
  
  lastReadTime[pin] = currentTime;
  bool currentState = inputs.digitalRead(pin);
  
  // Only update if state actually changed
  if (currentState != lastState[pin]) {
    lastState[pin] = currentState;
    Serial.println("Input " + String(pin) + " state changed to " + (currentState ? "HIGH" : "LOW"));
  }
  
  return currentState;
}

// Motor safety check - prevent both relays in a pair from being on simultaneously
bool checkMotorSafety() {
  for (int pair = 0; pair < 3; pair++) {
    int relay1 = pair * 2;
    int relay2 = pair * 2 + 1;
    
    if (relays.digitalRead(relay1) == LOW && relays.digitalRead(relay2) == LOW) {
      lastErrorMessage = "SAFETY ERROR: Both relays " + String(relay1) + " and " + String(relay2) + " are ON simultaneously!";
      lastErrorTime = millis();
      Serial.println("CRITICAL ERROR: " + lastErrorMessage);
      
      // Emergency shutdown
      for (int i = 0; i < 6; i++) {
        relays.digitalWrite(i, HIGH);
      }
      systemState = SYSTEM_ERROR;
      return false;
    }
  }
  return true;
}

// Function to start timeout monitoring for a specific relay
void startInputTimeout(int relayNumber) {
  inputTimeoutStart[relayNumber] = millis();
  inputTimeoutActive[relayNumber] = true;
  Serial.println("Started timeout monitoring for relay " + String(relayNumber));
}

// Function to stop timeout monitoring (call when input is detected)
void stopInputTimeout(int relayNumber) {
  inputTimeoutActive[relayNumber] = false;
  Serial.println("Input detected for relay " + String(relayNumber) + " - timeout cleared");
}

// Function to check all active timeouts
void checkInputTimeouts() {
  unsigned long currentTime = millis();
  
  for (int i = 0; i < 6; i++) {
    if (inputTimeoutActive[i]) {
      // Check if safety timeout has passed
      if (currentTime - inputTimeoutStart[i] >= safetyTimeoutMs) {
        // Timeout occurred - turn off all relays
        for (int j = 0; j < 6; j++) {
          relays.digitalWrite(j, HIGH); // Turn off all relays
        }
        
        // Reset all motor states
        for (int j = 0; j < 6; j++) {
          motorStates[j].waitingForInput = false;
          motorStates[j].isActive = false;
          motorStates[j].delayStartTime = 0;
        }
        
        // Clear all timeout monitoring
        for (int k = 0; k < 6; k++) {
          inputTimeoutActive[k] = false;
        }
        
        // Create error message
        lastErrorMessage = "Relay " + String(i) + " did not reach input " + String(i) + " before " + String(safetyTimeoutMs) + "ms timeout";
        lastErrorTime = currentTime;
        
        Serial.println("TIMEOUT ERROR: " + lastErrorMessage);
        break; // Exit loop since we've handled the timeout
      }
    }
  }
}

// Function to get last error for web display
String getLastError() {
  if (lastErrorTime > 0) {
    unsigned long timeSinceError = (millis() - lastErrorTime) / 1000; // Convert to seconds
    return lastErrorMessage + " (occurred " + String(timeSinceError) + " seconds ago)";
  }
  return "No recent errors";
}

// Function to clear error message
void clearLastError() {
  lastErrorMessage = "";
  lastErrorTime = 0;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting setup...");

  // Initialize SPIFFS for program storage
  if (!SPIFFS.begin(true)) {
    Serial.println("Failed to initialize SPIFFS!");
    while (1);
  }
  Serial.println("SPIFFS initialized successfully");

  // Initialize Preferences
  preferences.begin("relayDelays", false);

  // Load saved relay delays
  loadRelayDelays();

  // Load saved safety timeout
  loadSafetyTimeout();

  // Load saved microphone dB threshold
  loadMicDbThreshold();

  pinMode(ANALOG_A1, INPUT);

  // set inputs to pull-up mode
  inputs.pinMode(0, INPUT); // Set pin 0 as input
  inputs.pinMode(1, INPUT); // Set pin 1 as input
  inputs.pinMode(2, INPUT); // Set pin 2 as input
  inputs.pinMode(3, INPUT); // Set pin 3 as input
  inputs.pinMode(4, INPUT); // Set pin 4 as input
  inputs.pinMode(5, INPUT); // Set pin 5 as input

  // Initialize the input expander
  if (!inputs.begin()) {
    Serial.println("Failed to initialize inputs expander!");
    while (1); // Halt the program
  }

  // set relays to output mode
  relays.pinMode(0, OUTPUT); // Set pin 0 as output
  relays.pinMode(1, OUTPUT); // Set pin 1 as output
  relays.pinMode(2, OUTPUT); // Set pin 2 as output
  relays.pinMode(3, OUTPUT); // Set pin 3 as output
  relays.pinMode(4, OUTPUT); // Set pin 4 as output
  relays.pinMode(5, OUTPUT); // Set pin 5 as output

  // Initialize the relay expander
  if (!relays.begin()) {
    Serial.println("Failed to initialize relay expander!");
    while (1); // Halt the program
  }

  // Set all relays to off initially
  relays.digitalWrite(0, HIGH); // Set relay 0 to off
  relays.digitalWrite(1, HIGH); // Set relay 1 to off
  relays.digitalWrite(2, HIGH); // Set relay 2 to off
  relays.digitalWrite(3, HIGH); // Set relay 3 to off
  relays.digitalWrite(4, HIGH); // Set relay 4 to off
  relays.digitalWrite(5, HIGH); // Set relay 5 to off

  Serial.println("Setup complete. Waiting for input...");

  // setup wifi access point
  WiFi.softAP(ssid, password);
  IPAddress local_ip(192, 168, 1, 111);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);

  // Add a new endpoint to handle delay configuration
  server.on("/set-delay", HTTP_GET, [](AsyncWebServerRequest *request) {
    String message = "";
    bool hasError = false;

    // Check if parameters exist
    if (request->hasParam("relay") && request->hasParam("min") && request->hasParam("max")) {
      int relay = request->getParam("relay")->value().toInt();
      int newMinDelay = request->getParam("min")->value().toInt();
      int newMaxDelay = request->getParam("max")->value().toInt();

      // Validate the relay number
      if (relay < 0 || relay >= 6) {
        message = "Error: Invalid relay number";
        hasError = true;
      } else if (newMinDelay < 100) {
        message = "Error: Minimum delay cannot be less than 100ms";
        hasError = true;
      } else if (newMaxDelay > 20000) {
        message = "Error: Maximum delay cannot exceed 20000ms (20 seconds)";
        hasError = true;
      } else if (newMinDelay >= newMaxDelay) {
        message = "Error: Minimum delay must be less than maximum delay";
        hasError = true;
      } else {
        // Update the delay for the specified relay
        minDelayRelay[relay] = newMinDelay;
        maxDelayRelay[relay] = newMaxDelay;

        // Save updated delays to flash
        saveRelayDelays();

        message = "Relay " + String(relay) + " delay updated: Min=" + String(newMinDelay) + "ms, Max=" + String(newMaxDelay) + "ms";
        Serial.println(message);
      }
    } else {
      message = "Error: Missing parameters";
      hasError = true;
    }

    // Return response with redirect
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='3;url=/' />"; // Redirect after 3 seconds
    html += "<title>Settings Updated</title><style>";
    html += "body { font-family: Arial, sans-serif; text-align: center; margin-top: 100px; }";
    html += ".success { color: green; }";
    html += ".error { color: red; }";
    html += "</style></head><body>";
    html += "<h2 class='" + String(hasError ? "error" : "success") + "'>" + message + "</h2>";
    html += "<p>Redirecting back to home page...</p>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  // Add endpoint to handle safety timeout configuration
  server.on("/set-safety-timeout", HTTP_GET, [](AsyncWebServerRequest *request) {
    String message = "";
    bool hasError = false;

    // Check if timeout parameter exists
    if (request->hasParam("timeout")) {
      int newTimeout = request->getParam("timeout")->value().toInt();

      // Validate the timeout value
      if (newTimeout < 500) {
        message = "Error: Safety timeout cannot be less than 500ms";
        hasError = true;
      } else if (newTimeout > 10000) {
        message = "Error: Safety timeout cannot exceed 10000ms (10 seconds)";
        hasError = true;
      } else {
        // Update the safety timeout
        safetyTimeoutMs = newTimeout;

        // Save updated timeout to flash
        saveSafetyTimeout();

        message = "Safety timeout updated to " + String(newTimeout) + "ms";
        Serial.println(message);
      }
    } else {
      message = "Error: Missing timeout parameter";
      hasError = true;
    }

    // Return response with redirect
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='3;url=/' />"; // Redirect after 3 seconds
    html += "<title>Safety Timeout Updated</title><style>";
    html += "body { font-family: Arial, sans-serif; text-align: center; margin-top: 100px; }";
    html += ".success { color: green; }";
    html += ".error { color: red; }";
    html += "</style></head><body>";
    html += "<h2 class='" + String(hasError ? "error" : "success") + "'>" + message + "</h2>";
    html += "<p>Redirecting back to home page...</p>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  // Add endpoint for microphone dB threshold configuration
  server.on("/set-mic-threshold", HTTP_GET, [](AsyncWebServerRequest *request) {
    String message;
    bool hasError = false;

    if (request->hasParam("threshold")) {
      float newThreshold = request->getParam("threshold")->value().toFloat();

      if (newThreshold < 20.0) {
        message = "Error: Microphone threshold cannot be less than 20 dBA";
        hasError = true;
      } else if (newThreshold > 120.0) {
        message = "Error: Microphone threshold cannot exceed 120 dBA";
        hasError = true;
      } else {
        // Update the microphone threshold
        micDbThreshold = newThreshold;

        // Save updated threshold to flash
        saveMicDbThreshold();

        message = "Microphone threshold updated to " + String(newThreshold, 1) + " dBA";
        Serial.println(message);
      }
    } else {
      message = "Error: Missing threshold parameter";
      hasError = true;
    }

    // Return response with redirect
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='3;url=/' />"; // Redirect after 3 seconds
    html += "<title>Microphone Threshold Updated</title><style>";
    html += "body { font-family: Arial, sans-serif; text-align: center; margin-top: 100px; }";
    html += ".success { color: green; }";
    html += ".error { color: red; }";
    html += "</style></head><body>";
    html += "<h2 class='" + String(hasError ? "error" : "success") + "'>" + message + "</h2>";
    html += "<p>Redirecting back to home page...</p>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  // Update the root route to include delay configuration form
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    // Remove auto-refresh for better UX with program selector
    html += "<title>TARCZOWNIX Control</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; text-align: center; margin: 20px; background-color: #f5f5f5; }";
    html += ".container { max-width: 800px; margin: 0 auto; padding: 20px; border: 1px solid #ddd; border-radius: 10px; background-color: white; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }";
    html += "h1 { color: #333; margin-bottom: 30px; }";
    html += ".btn { background-color: #4CAF50; border: none; color: white; padding: 15px 32px; ";
    html += "text-align: center; text-decoration: none; display: inline-block; font-size: 16px; ";
    html += "margin: 10px 5px; cursor: pointer; border-radius: 8px; transition: all 0.3s ease; }";
    html += ".btn:hover { background-color: #45a049; transform: translateY(-2px); }";
    html += ".btn-stop { background-color: #f44336; }";
    html += ".btn-stop:hover { background-color: #da190b; }";
    html += ".btn-clear { background-color: #ff9800; }";
    html += ".btn-clear:hover { background-color: #e68900; }";
    html += ".form-group { margin: 15px 0; text-align: left; }";
    html += "input[type=number] { padding: 10px; width: 100px; border-radius: 4px; border: 1px solid #ccc; }";
    html += "label { display: inline-block; width: 120px; text-align: right; margin-right: 10px; font-weight: bold; }";
    html += ".card { border: 1px solid #ddd; border-radius: 8px; padding: 20px; margin: 20px 0; background-color: #f9f9f9; }";
    html += ".status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin: 20px 0; }";
    html += ".status-item { background-color: white; padding: 15px; border-radius: 8px; border-left: 4px solid #4CAF50; }";
    html += ".status-on { border-left-color: #f44336; background-color: #ffe6e6; }";
    html += ".status-off { border-left-color: #4CAF50; background-color: #e6ffe6; }";
    html += ".motor-pair { background-color: #e3f2fd; border: 1px solid #1976d2; margin: 10px 0; padding: 15px; border-radius: 8px; }";
    html += ".warning { color: #ff6b35; font-weight: bold; }";
    html += ".success { color: #4CAF50; font-weight: bold; }";
    html += ".relay-config { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 20px; }";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<h1>TARCZOWNIX Motor Control System</h1>";
    html += "<button onclick='location.reload()' class='btn' style='background-color: #2196f3; margin-bottom: 20px;'>🔄 Refresh Status</button>";

    // System Status Overview
    html += "<div class='card'>";
    html += "<h2>System Status</h2>";
    html += "<p><strong>System State:</strong> ";
    switch(systemState) {
      case SYSTEM_STOPPED: html += "<span style='color: #ff9800;'>STOPPED</span>"; break;
      case SYSTEM_RUNNING: html += "<span style='color: #4CAF50;'>RUNNING</span>"; break;
      case SYSTEM_ERROR: html += "<span style='color: #f44336;'>ERROR</span>"; break;
    }
    html += "</p>";
    html += "<p><strong>Operating Mode:</strong> ";
    html += "<span style='color: #2196f3;'>" + String(currentMode == MODE_SEQUENCE ? "SEQUENCE" : "CUSTOM PROGRAM") + "</span>";
    html += "</p>";
    html += "<p><strong>Microphone Threshold:</strong> <span id='mic-threshold'>" + String(micDbThreshold, 1) + " dBA</span></p>";
    if (currentMode == MODE_CUSTOM && waitingForCustomMicTrigger) {
      html += "<p><strong>Status:</strong> <span style='color: #ff9800;'>Waiting for mic trigger (" + String(currentPairProgram.micThreshold, 1) + " dBA)</span></p>";
      html += "<p><strong>Program Ready:</strong> " + currentPairProgram.name + "</p>";
    } else if (currentMode == MODE_CUSTOM && pairProgramRunning) {
      html += "<p><strong>Current Program:</strong> " + currentPairProgram.name + "</p>";
      html += "<p><strong>Program Block:</strong> " + String(currentBlockIndex + 1) + " / " + String(currentPairProgram.blockCount) + "</p>";
      html += "<p><strong>Pair State:</strong> ";
      switch(currentPairState) {
        case PAIR_IDLE: html += "Idle"; break;
        case PAIR_FIRST_RELAY: html += "First Relay Active"; break;
        case PAIR_DELAY: html += "Delay Period"; break;
        case PAIR_SECOND_RELAY: html += "Second Relay Active"; break;
        case PAIR_COMPLETE: html += "Block Complete"; break;
      }
      html += "</p>";
    }
    html += "<p><strong>Last Error:</strong> " + getLastError() + "</p>";
    if (lastErrorTime > 0) {
      html += "<a href='/clear-error' class='btn btn-clear'>Clear Error</a>";
    }
    html += "</div>";

    // Mode Selection
    html += "<div class='card'>";
    html += "<h2>Operating Mode</h2>";
    html += "<a href='/set-mode?mode=sequence' class='btn " + String(currentMode == MODE_SEQUENCE ? "btn-stop" : "") + "'>Sequence Mode</a>";
    html += "<a href='/set-mode?mode=custom' class='btn " + String(currentMode == MODE_CUSTOM ? "btn-stop" : "") + "'>Custom Program Mode</a>";
    html += "<a href='/program-editor' class='btn btn-clear'>Program Editor</a>";
    html += "</div>";

    // Relay control section with better visualization
    html += "<div class='card'>";
    if (currentMode == MODE_SEQUENCE) {
      html += "<h2>Sequence Control</h2>";
      html += "<a href='/start' class='btn'>Start Sequence</a>";
      html += "<a href='/stop' class='btn btn-stop'>Stop Sequence</a>";
    } else {
      html += "<h2>Custom Program Control</h2>";
      if (pairProgramRunning) {
        html += "<a href='/stop-program' class='btn btn-stop'>Stop Program</a>";
      } else {
        html += "<p>Select a program to execute:</p>";
        // Add program selection here (this will be populated by JavaScript)
        html += "<div id='program-selector' style='margin: 10px 0;'></div>";
        html += "<script>";
        html += "let currentProgramList = null;";
        html += "function loadProgramSelector() {";
        html += "  fetch('/program-list').then(r=>r.json()).then(d=>{";
        html += "    const selector = document.getElementById('program-selector');";
        html += "    const currentSelect = document.getElementById('program-select');";
        html += "    const currentValue = currentSelect ? currentSelect.value : '';";
        html += "    ";
        html += "    if (JSON.stringify(d.programs) !== JSON.stringify(currentProgramList)) {";
        html += "      currentProgramList = d.programs;";
        html += "      let html='<select id=\"program-select\" style=\"padding:5px;margin:5px;\"><option value=\"\">Select Program...</option>';";
        html += "      d.programs.forEach(p=>html+=`<option value=\"${p}\">${p}</option>`);";
        html += "      html+='</select><button onclick=\"executeSelectedProgram()\" class=\"btn\">Execute Program</button>';";
        html += "      selector.innerHTML=html;";
        html += "      if (currentValue) document.getElementById('program-select').value = currentValue;";
        html += "    }";
        html += "  });";
        html += "}";
        html += "loadProgramSelector();";
        html += "function executeSelectedProgram(){";
        html += "const select=document.getElementById('program-select');";
        html += "if(select.value)window.location.href='/execute-program?name='+encodeURIComponent(select.value);";
        html += "else alert('Please select a program');";
        html += "}";
        html += "</script>";
      }
    }
    
    // Motor Pair Status
    html += "<h3>Motor Pair Status</h3>";
    for (int pair = 0; pair < 3; pair++) {
      int relay1 = pair * 2;
      int relay2 = pair * 2 + 1;
      html += "<div class='motor-pair'>";
      html += "<h4>Motor Pair " + String(pair + 1) + " (Relays " + String(relay1) + " & " + String(relay2) + ")</h4>";
      html += "<div style='display: flex; justify-content: space-around;'>";
      html += "<div class='status-item " + String(relays.digitalRead(relay1) == LOW ? "status-on" : "status-off") + "'>";
      html += "Relay " + String(relay1) + ": " + (relays.digitalRead(relay1) == LOW ? "ON" : "OFF");
      html += "</div>";
      html += "<div class='status-item " + String(relays.digitalRead(relay2) == LOW ? "status-on" : "status-off") + "'>";
      html += "Relay " + String(relay2) + ": " + (relays.digitalRead(relay2) == LOW ? "ON" : "OFF");
      html += "</div>";
      html += "</div>";
      html += "</div>";
    }
    html += "</div>";

    // Enhanced delay configuration
    html += "<div class='card'>";
    html += "<h2>Delay Configuration</h2>";
    html += "<div class='relay-config'>";
    for (int i = 0; i < 6; i++) {
      html += "<div style='border: 1px solid #ddd; padding: 15px; border-radius: 8px; background-color: white;'>";
      html += "<h3>Relay " + String(i) + "</h3>";
      html += "<form action='/set-delay' method='get'>";
      html += "<input type='hidden' name='relay' value='" + String(i) + "'>";
      html += "<div class='form-group'>";
      html += "<label for='min'>Min Delay (ms):</label>";
      html += "<input type='number' id='min' name='min' min='100' max='10000' value='" + String(minDelayRelay[i]) + "' required>";
      html += "</div>";
      html += "<div class='form-group'>";
      html += "<label for='max'>Max Delay (ms):</label>";
      html += "<input type='number' id='max' name='max' min='100' max='20000' value='" + String(maxDelayRelay[i]) + "' required>";
      html += "</div>";
      html += "<input type='submit' class='btn' value='Save Settings' style='width: 100%;'>";
      html += "</form>";
      html += "<p><small>Current range: " + String(minDelayRelay[i]) + " - " + String(maxDelayRelay[i]) + " ms</small></p>";
      html += "</div>";
    }
    html += "</div>";
    html += "</div>";

    // Safety Timeout Configuration
    html += "<div class='card'>";
    html += "<h2>Safety Configuration</h2>";
    html += "<div style='border: 1px solid #ddd; padding: 15px; border-radius: 8px; background-color: white;'>";
    html += "<h3>Safety Timeout</h3>";
    html += "<p>Current timeout: <strong>" + String(safetyTimeoutMs) + "ms</strong></p>";
    html += "<p><small>If a relay doesn't receive its corresponding input signal within this time, all relays will be turned off for safety.</small></p>";
    html += "<form action='/set-safety-timeout' method='get'>";
    html += "<div class='form-group'>";
    html += "<label for='timeout'>Timeout (ms):</label>";
    html += "<input type='number' id='timeout' name='timeout' min='500' max='10000' value='" + String(safetyTimeoutMs) + "' required>";
    html += "</div>";
    html += "<input type='submit' class='btn' value='Update Safety Timeout' style='width: 100%;'>";
    html += "</form>";
    html += "</div>";
    html += "</div>";

    // Microphone Configuration
    html += "<div class='card'>";
    html += "<h2>Microphone Configuration</h2>";
    html += "<div style='border: 1px solid #ddd; padding: 15px; border-radius: 8px; background-color: white;'>";
    html += "<h3>Sound Trigger Threshold</h3>";
    html += "<p>Current threshold: <strong>" + String(micDbThreshold, 1) + " dBA</strong></p>";
    html += "<p><small>When sound level exceeds this threshold, the sequence mode will be triggered automatically.</small></p>";
    html += "<form action='/set-mic-threshold' method='get'>";
    html += "<div class='form-group'>";
    html += "<label for='threshold'>Threshold (dBA):</label>";
    html += "<input type='number' id='threshold' name='threshold' min='20' max='120' step='0.1' value='" + String(micDbThreshold, 1) + "' required>";
    html += "</div>";
    html += "<input type='submit' class='btn' value='Update Microphone Threshold' style='width: 100%;'>";
    html += "</form>";
    html += "</div>";
    
    // Add JavaScript to load configuration data
    html += "<script>";
    html += "function loadConfig() {";
    html += "  fetch('/config')";
    html += "    .then(response => response.json())";
    html += "    .then(config => {";
    html += "      // Update the displayed microphone threshold";
    html += "      const micElement = document.getElementById('mic-threshold');";
    html += "      if (micElement) {";
    html += "        micElement.textContent = config.micDbThreshold + ' dBA';";
    html += "      }";
    html += "    })";
    html += "    .catch(error => console.log('Failed to load config:', error));";
    html += "}";
    html += "// Load config when page loads";
    html += "window.addEventListener('DOMContentLoaded', loadConfig);";
    html += "</script>";
    html += "</div>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request) {
  String message;
  bool hasError = false;

  // Check if relays 0, 2, 4 are off before starting
  if (relays.digitalRead(0) == HIGH && relays.digitalRead(2) == HIGH && relays.digitalRead(4) == HIGH) {
    relays.digitalWrite(0, LOW); // Turn on relay 0
    relays.digitalWrite(2, LOW); // Turn on relay 2  
    relays.digitalWrite(4, LOW); // Turn on relay 4
    
    // Start timeout monitoring for the relays that are now on
    startInputTimeout(0);
    startInputTimeout(2);
    startInputTimeout(4);
    
    message = "Relay 0, 2, and 4 are now ON";
  } else {
    message = "Error: One or more relays are already ON";
    hasError = true;
  }

  // Return response with redirect
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta http-equiv='refresh' content='3;url=/' />"; 
  html += "<title>Start sequences</title><style>";
  html += "body { font-family: Arial, sans-serif; text-align: center; margin-top: 100px; }";
  html += ".success { color: green; }";
  html += ".error { color: red; }";
  html += "</style></head><body>";
  html += "<h2 class='" + String(hasError ? "error" : "success") + "'>" + message + "</h2>";
  html += "<p>Redirecting back to home page...</p>";
  html += "</body></html>";

  request->send(200, "text/html", html);
});

  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Turn off all relays
    for (int i = 0; i < 6; i++) {
      relays.digitalWrite(i, HIGH);
    }
    // Reset all motor states
    for (int i = 0; i < 6; i++) {
      motorStates[i].waitingForInput = false;
      motorStates[i].isActive = false; 
      motorStates[i].delayStartTime = 0;
    }
    systemState = SYSTEM_STOPPED;

    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='2;url=/' />";
    html += "<title>Stop Sequence</title><style>";
    html += "body { font-family: Arial, sans-serif; text-align: center; margin-top: 100px; }";
    html += ".success { color: green; }";
    html += "</style></head><body>";
    html += "<h2 class='success'>Sequence stopped. All relays OFF and variables reset.</h2>";
    html += "<p>Redirecting back to home page...</p>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  // Add this server endpoint in setup()
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"systemState\":" + String((int)systemState) + ",";
    json += "\"lastError\":\"" + getLastError() + "\",";
    json += "\"relayStates\":[";
    for (int i = 0; i < 6; i++) {
      json += String(relays.digitalRead(i) == LOW ? 1 : 0);
      if (i < 5) json += ",";
    }
    json += "],";
    json += "\"motorStates\":[";
    for (int i = 0; i < 6; i++) {
      json += "{\"waitingForInput\":" + String(motorStates[i].waitingForInput ? "true" : "false");
      json += ",\"isActive\":" + String(motorStates[i].isActive ? "true" : "false");
      json += ",\"delayRemaining\":" + String(motorStates[i].waitingForInput ? 
        max(0L, (long)motorStates[i].currentRandomDelay - (long)(millis() - motorStates[i].delayStartTime)) : 0);
      json += "}";
      if (i < 5) json += ",";
    }
    json += "],";
    json += "\"uptime\":" + String(millis()) + ",";
    json += "\"freeHeap\":" + String(ESP.getFreeHeap());
    json += "}";
    
    request->send(200, "application/json", json);
  });

  // Add endpoint for getting current configuration
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"relayDelays\":[";
    for (int i = 0; i < 6; i++) {
      json += "{\"relay\":" + String(i);
      json += ",\"minDelay\":" + String(minDelayRelay[i]);
      json += ",\"maxDelay\":" + String(maxDelayRelay[i]) + "}";
      if (i < 5) json += ",";
    }
    json += "],";
    json += "\"safetyTimeoutMs\":" + String(safetyTimeoutMs) + ",";
    json += "\"micDbThreshold\":" + String(micDbThreshold, 1);
    json += "}";
    
    request->send(200, "application/json", json);
  });

  // Add endpoint to clear errors
  server.on("/clear-error", HTTP_GET, [](AsyncWebServerRequest *request) {
    clearLastError();
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='2;url=/' />";
    html += "<title>Error Cleared</title></head><body>";
    html += "<h2>Error message cleared</h2>";
    html += "<p>Redirecting back to home page...</p>";
    html += "</body></html>";
    
    request->send(200, "text/html", html);
  });

  // Pair-Based Program Editor Route
  server.on("/program-editor", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>TARCZOWNIX - Pair Program Editor</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }";
    html += ".container { max-width: 1200px; margin: 0 auto; background-color: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }";
    html += ".editor-section { border: 2px dashed #ccc; padding: 20px; margin: 20px 0; min-height: 200px; border-radius: 8px; }";
    html += ".pair-item { background-color: #e3f2fd; border: 1px solid #1976d2; padding: 15px; margin: 10px; border-radius: 5px; cursor: grab; display: inline-block; min-width: 180px; text-align: center; }";
    html += ".pair-item:active { cursor: grabbing; }";
    html += ".pair-item h4 { margin: 5px 0; color: #1976d2; }";
    html += ".pair-item .relays { font-size: 14px; color: #666; }";
    html += ".block-item { background-color: #f1f8e9; border: 1px solid #689f38; padding: 15px; margin: 10px 0; border-radius: 8px; position: relative; }";
    html += ".block-controls { margin-top: 15px; }";
    html += ".block-controls input, .block-controls select { margin: 5px; padding: 8px; border: 1px solid #ddd; border-radius: 4px; }";
    html += ".btn { background-color: #4CAF50; border: none; color: white; padding: 10px 20px; text-decoration: none; display: inline-block; border-radius: 5px; cursor: pointer; margin: 5px; }";
    html += ".btn-danger { background-color: #f44336; }";
    html += ".btn-warning { background-color: #ff9800; }";
    html += ".btn-secondary { background-color: #6c757d; }";
    html += ".form-group { margin: 15px 0; }";
    html += ".form-group label { display: inline-block; width: 150px; font-weight: bold; }";
    html += ".safety-info { background-color: #e8f5e8; border: 2px solid #4CAF50; padding: 15px; margin: 20px 0; border-radius: 8px; }";
    html += ".program-list { background-color: #f9f9f9; padding: 15px; border-radius: 8px; margin: 20px 0; }";
    html += ".program-item { background-color: white; padding: 10px; margin: 5px 0; border-radius: 5px; border: 1px solid #ddd; display: flex; justify-content: space-between; align-items: center; }";
    html += "</style>";
    html += "<script>";
    html += "let draggedElement = null;";
    html += "let blockCounter = 0;";
    html += "let currentBlocks = [];";
    html += "let loadProgramListTimeout = null;"; // Debounce timeout
    
    // JavaScript for drag and drop functionality
    html += "function allowDrop(ev) { ev.preventDefault(); }";
    html += "function drag(ev) { draggedElement = ev.target; }";
    html += "function drop(ev) {";
    html += "  ev.preventDefault();";
    html += "  if (draggedElement && draggedElement.classList.contains('pair-item')) {";
    html += "    addBlock(draggedElement.getAttribute('data-pair'));";
    html += "  }";
    html += "}";
    
    // Function to add a pair block
    html += "function addBlock(pairId) {";
    html += "  const pairNames = ['Pair 0 (Relays 0-1)', 'Pair 1 (Relays 2-3)', 'Pair 2 (Relays 4-5)'];";
    html += "  const blockDiv = document.createElement('div');";
    html += "  blockDiv.className = 'block-item';";
    html += "  blockDiv.setAttribute('data-block', blockCounter);";
    html += "  blockDiv.innerHTML = `";
    html += "    <h4>Block ${blockCounter + 1}: ${pairNames[pairId]}</h4>";
    html += "    <p><strong>Operation:</strong> Activate first relay → Wait for input → Apply delay → Activate second relay → Wait for input → Next block</p>";
    html += "    <div class='block-controls'>";
    html += "      <label>Delay after first input (ms):</label>";
    html += "      <input type='number' min='100' max='60000' value='2000' onchange='updateBlock(${blockCounter})' id='delay-${blockCounter}'>";
    html += "      <br>";
    html += "      <label>Wait for second input:</label>";
    html += "      <select onchange='updateBlock(${blockCounter})' id='wait-${blockCounter}'>";
    html += "        <option value='true'>Yes (Recommended)</option>";
    html += "        <option value='false'>No</option>";
    html += "      </select>";
    html += "      <br>";
    html += "      <button class='btn btn-danger' onclick='removeBlock(${blockCounter})'>Remove Block</button>";
    html += "      <button class='btn btn-warning' onclick='moveBlockUp(${blockCounter})'>Move Up</button>";
    html += "      <button class='btn btn-warning' onclick='moveBlockDown(${blockCounter})'>Move Down</button>";
    html += "    </div>";
    html += "  `;";
    html += "  document.getElementById('program-blocks').appendChild(blockDiv);";
    html += "  currentBlocks.push({pairId: parseInt(pairId), delayMs: 2000, waitForInput: true, blockId: blockCounter});";
    html += "  blockCounter++;";
    html += "  updateBlockNumbers();";
    html += "}";
    
    // Other JavaScript functions
    html += "function removeBlock(blockId) {";
    html += "  document.querySelector(`[data-block='${blockId}']`).remove();";
    html += "  currentBlocks = currentBlocks.filter(block => block.blockId !== blockId);";
    html += "  updateBlockNumbers();";
    html += "}";
    
    html += "function updateBlock(blockId) {";
    html += "  const block = currentBlocks.find(b => b.blockId === blockId);";
    html += "  if (block) {";
    html += "    block.delayMs = parseInt(document.getElementById(`delay-${blockId}`).value);";
    html += "    block.waitForInput = document.getElementById(`wait-${blockId}`).value === 'true';";
    html += "  }";
    html += "}";
    
    html += "function updateBlockNumbers() {";
    html += "  const blocks = document.querySelectorAll('.block-item');";
    html += "  blocks.forEach((block, index) => {";
    html += "    const h4 = block.querySelector('h4');";
    html += "    const oldText = h4.textContent;";
    html += "    h4.textContent = oldText.replace(/Block \\d+:/, `Block ${index + 1}:`);";
    html += "  });";
    html += "}";
    
    html += "function moveBlockUp(blockId) {";
    html += "  const blockEl = document.querySelector(`[data-block='${blockId}']`);";
    html += "  const prevSibling = blockEl.previousElementSibling;";
    html += "  if (prevSibling) {";
    html += "    blockEl.parentNode.insertBefore(blockEl, prevSibling);";
    html += "    updateBlockNumbers();";
    html += "  }";
    html += "}";
    
    html += "function moveBlockDown(blockId) {";
    html += "  const blockEl = document.querySelector(`[data-block='${blockId}']`);";
    html += "  const nextSibling = blockEl.nextElementSibling;";
    html += "  if (nextSibling) {";
    html += "    blockEl.parentNode.insertBefore(nextSibling, blockEl);";
    html += "    updateBlockNumbers();";
    html += "  }";
    html += "}";
    
    html += "function clearProgram() {";
    html += "  if (confirm('Are you sure you want to clear the entire program?')) {";
    html += "    document.getElementById('program-blocks').innerHTML = '';";
    html += "    currentBlocks = [];";
    html += "    blockCounter = 0;";
    html += "  }";
    html += "}";
    
    html += "function toggleMicThreshold() {";
    html += "  const checkbox = document.getElementById('wait-mic-trigger');";
    html += "  const thresholdGroup = document.getElementById('mic-threshold-group');";
    html += "  thresholdGroup.style.display = checkbox.checked ? 'block' : 'none';";
    html += "}";
    
    html += "function saveProgram() {";
    html += "  const programName = document.getElementById('program-name').value.trim();";
    html += "  if (!programName) {";
    html += "    alert('Please enter a program name');";
    html += "    return;";
    html += "  }";
    html += "  if (currentBlocks.length === 0) {";
    html += "    alert('Please add at least one pair block');";
    html += "    return;";
    html += "  }";
    
    html += "  const programData = {";
    html += "    name: programName,";
    html += "    loopEnabled: document.getElementById('loop-enabled').checked,";
    html += "    waitForMicTrigger: document.getElementById('wait-mic-trigger').checked,";
    html += "    micThreshold: parseFloat(document.getElementById('mic-threshold').value) || " + String(micDbThreshold, 1) + ",";
    html += "    blocks: currentBlocks.map((block, index) => ({";
    html += "      pairId: block.pairId,";
    html += "      delayMs: block.delayMs,";
    html += "      waitForInput: block.waitForInput";
    html += "    }))";
    html += "  };";
    
    html += "  fetch('/save-program', {";
    html += "    method: 'POST',";
    html += "    headers: { 'Content-Type': 'application/json' },";
    html += "    body: JSON.stringify(programData)";
    html += "  }).then(response => {";
    html += "    if (response.ok) {";
    html += "      alert('Program saved successfully!');";
    html += "      loadProgramListDebounced();";
    html += "    } else {";
    html += "      alert('Failed to save program');";
    html += "    }";
    html += "  });";
    html += "}";
    
    html += "function loadProgramList() {";
    html += "  console.log('Loading program list...');";
    html += "  fetch('/program-list')";
    html += "    .then(response => {";
    html += "      console.log('Program list response status:', response.status);";
    html += "      return response.json();";
    html += "    })";
    html += "    .then(data => {";
    html += "      console.log('Program list data received:', data);";
    html += "      const listDiv = document.getElementById('program-list');";
    html += "      listDiv.innerHTML = '';";
    html += "      if (data.programs && data.programs.length > 0) {";
    html += "        data.programs.forEach(program => {";
    html += "        const item = document.createElement('div');";
    html += "        item.className = 'program-item';";
    html += "        item.innerHTML = `";
    html += "          <span>${program}</span>";
    html += "          <div>";
    html += "            <button class='btn btn-secondary' onclick='loadProgram(\"${program}\")'>Load</button>";
    html += "            <button class='btn btn-danger' onclick='deleteProgram(\"${program}\")'>Delete</button>";
    html += "            <button class='btn' onclick='executeProgram(\"${program}\")'>Execute</button>";
    html += "          </div>";
    html += "        `;";
    html += "        listDiv.appendChild(item);";
    html += "      });";
    html += "      } else {";
    html += "        listDiv.innerHTML = '<p>No saved programs found. Create and save a program to see it here.</p>';";
    html += "        console.log('No programs found in data');";
    html += "      }";
    html += "    })";
    html += "    .catch(error => {";
    html += "      console.error('Error loading program list:', error);";
    html += "      document.getElementById('program-list').innerHTML = '<p>Error loading programs</p>';";
    html += "    });";
    html += "}";
    
    html += "function loadProgramListDebounced() {";
    html += "  if (loadProgramListTimeout) clearTimeout(loadProgramListTimeout);";
    html += "  loadProgramListTimeout = setTimeout(loadProgramList, 300);"; // 300ms debounce
    html += "}";
    
    html += "function loadProgram(programName) {";
    html += "  fetch(`/load-program?name=${encodeURIComponent(programName)}`)";
    html += "    .then(response => response.text())";
    html += "    .then(data => {";
    html += "      try {";
    html += "        const program = JSON.parse(data);";
    html += "        document.getElementById('program-name').value = program.name;";
    html += "        document.getElementById('loop-enabled').checked = program.loopEnabled;";
    html += "        document.getElementById('wait-mic-trigger').checked = program.waitForMicTrigger || false;";
    html += "        document.getElementById('mic-threshold').value = program.micThreshold || " + String(micDbThreshold, 1) + ";";
    html += "        toggleMicThreshold();"; // Show/hide mic threshold input
    html += "        clearProgram();";
    html += "        if (program.blocks) {";
    html += "          program.blocks.forEach(block => {";
    html += "            addBlock(block.pairId);";
    html += "            const lastBlock = currentBlocks[currentBlocks.length - 1];";
    html += "            lastBlock.delayMs = block.delayMs;";
    html += "            lastBlock.waitForInput = block.waitForInput;";
    html += "            document.getElementById(`delay-${lastBlock.blockId}`).value = block.delayMs;";
    html += "            document.getElementById(`wait-${lastBlock.blockId}`).value = block.waitForInput;";
    html += "          });";
    html += "        }";
    html += "        alert('Program loaded successfully!');";
    html += "      } catch (e) {";
    html += "        alert('Failed to load program');";
    html += "      }";
    html += "    });";
    html += "}";
    
    html += "function deleteProgram(programName) {";
    html += "  if (confirm(`Are you sure you want to delete '${programName}'?`)) {";
    html += "    fetch(`/delete-program?name=${encodeURIComponent(programName)}`)";
    html += "      .then(response => {";
    html += "        if (response.ok) {";
    html += "          alert('Program deleted successfully!');";
    html += "          loadProgramListDebounced();";
    html += "        } else {";
    html += "          alert('Failed to delete program');";
    html += "        }";
    html += "      });";
    html += "  }";
    html += "}";
    
    html += "function executeProgram(programName) {";
    html += "  if (confirm(`Execute program '${programName}'?`)) {";
    html += "    window.location.href = `/execute-program?name=${encodeURIComponent(programName)}`;";
    html += "  }";
    html += "}";
    
    html += "window.onload = function() { loadProgramList(); };";
    html += "</script>";
    html += "</head><body>";
    
    html += "<div class='container'>";
    html += "<h1>Pair-Based Program Editor</h1>";
    html += "<a href='/' class='btn btn-secondary'>← Back to Home</a>";
    
    html += "<div class='safety-info'>";
    html += "<h3>How Pair Programs Work:</h3>";
    html += "<ul>";
    html += "<li><strong>Pair 0:</strong> Relays 0-1 (Motor pair 1)</li>";
    html += "<li><strong>Pair 1:</strong> Relays 2-3 (Motor pair 2)</li>";
    html += "<li><strong>Pair 2:</strong> Relays 4-5 (Motor pair 3)</li>";
    html += "</ul>";
    html += "<p><strong>Block Operation:</strong> First relay activates → Wait for input → Apply delay → Second relay activates → Wait for input → Move to next block</p>";
    html += "</div>";
    
    html += "<div class='form-group'>";
    html += "<label for='program-name'>Program Name:</label>";
    html += "<input type='text' id='program-name' placeholder='Enter program name' style='width: 200px; padding: 8px;'>";
    html += "</div>";
    
    html += "<div class='form-group'>";
    html += "<label for='loop-enabled'>Loop Program:</label>";
    html += "<input type='checkbox' id='loop-enabled'> <small>(Restart from beginning when complete)</small>";
    html += "</div>";
    
    html += "<div class='form-group'>";
    html += "<label for='wait-mic-trigger'>Wait for Microphone Trigger:</label>";
    html += "<input type='checkbox' id='wait-mic-trigger' onchange='toggleMicThreshold()'> <small>(Program waits for sound before starting)</small>";
    html += "</div>";
    
    html += "<div class='form-group' id='mic-threshold-group' style='display: none;'>";
    html += "<label for='mic-threshold'>Microphone Threshold (dBA):</label>";
    html += "<input type='number' id='mic-threshold' min='20' max='120' step='0.1' value='" + String(micDbThreshold, 1) + "' style='width: 100px; padding: 8px;'>";
    html += "<small> Current global threshold: " + String(micDbThreshold, 1) + " dBA</small>";
    html += "</div>";
    
    html += "<h3>Available Motor Pairs</h3>";
    html += "<p>Drag a motor pair below to add it to your program:</p>";
    html += "<div style='margin: 20px 0;'>";
    html += "<div class='pair-item' draggable='true' ondragstart='drag(event)' data-pair='0'>";
    html += "<h4>Pair 0</h4>";
    html += "<div class='relays'>Relays 0-1</div>";
    html += "</div>";
    html += "<div class='pair-item' draggable='true' ondragstart='drag(event)' data-pair='1'>";
    html += "<h4>Pair 1</h4>";
    html += "<div class='relays'>Relays 2-3</div>";
    html += "</div>";
    html += "<div class='pair-item' draggable='true' ondragstart='drag(event)' data-pair='2'>";
    html += "<h4>Pair 2</h4>";
    html += "<div class='relays'>Relays 4-5</div>";
    html += "</div>";
    html += "</div>";
    
    html += "<h3>Program Blocks</h3>";
    html += "<div class='editor-section' ondrop='drop(event)' ondragover='allowDrop(event)'>";
    html += "<p style='text-align: center; color: #666;'>Drop motor pairs here to build your program</p>";
    html += "<div id='program-blocks'></div>";
    html += "</div>";
    
    html += "<div style='margin: 20px 0;'>";
    html += "<button class='btn' onclick='saveProgram()'>Save Program</button>";
    html += "<button class='btn btn-warning' onclick='clearProgram()'>Clear All</button>";
    html += "</div>";
    
    html += "<div class='program-list'>";
    html += "<h3>Saved Programs</h3>";
    html += "<div id='program-list'></div>";
    html += "</div>";
    
    html += "</div>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  // Save program endpoint
  server.on("/save-program", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      String body = "";
      for (size_t i = 0; i < len; i++) {
        body += (char)data[i];
      }
      
      // Simple JSON parsing (since ArduinoJson might not be available)
      if (validatePairProgramSafety(body)) {
        // Extract program name from JSON manually
        int nameStart = body.indexOf("\"name\":\"") + 8;
        int nameEnd = body.indexOf("\"", nameStart);
        String programName = body.substring(nameStart, nameEnd);
        
        if (savePairProgram(programName, body)) {
          request->send(200, "text/plain", "Program saved successfully");
        } else {
          request->send(500, "text/plain", "Failed to save program");
        }
      } else {
        request->send(400, "text/plain", "Program violates safety rules");
      }
    });

  // Load program endpoint
  server.on("/load-program", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("name")) {
      String programName = request->getParam("name")->value();
      String programData = loadPairProgram(programName);
      if (programData.length() > 0) {
        request->send(200, "application/json", programData);
      } else {
        request->send(404, "text/plain", "Program not found");
      }
    } else {
      request->send(400, "text/plain", "Program name required");
    }
  });

  // Program list endpoint
  server.on("/program-list", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Program list endpoint called");
    String response = getPairProgramList();
    Serial.println("Sending response: " + response);
    request->send(200, "application/json", response);
  });

  // Delete program endpoint
  server.on("/delete-program", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("name")) {
      String programName = request->getParam("name")->value();
      if (deletePairProgram(programName)) {
        request->send(200, "text/plain", "Program deleted successfully");
      } else {
        request->send(500, "text/plain", "Failed to delete program");
      }
    } else {
      request->send(400, "text/plain", "Program name required");
    }
  });

  // Execute pair program endpoint
  server.on("/execute-program", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("name")) {
      String programName = request->getParam("name")->value();
      String programData = loadPairProgram(programName);
      if (programData.length() > 0) {
        // Parse JSON data to load pair program
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, programData);
        
        if (error) {
          request->send(400, "text/plain", "Invalid program format");
          return;
        }
        
        // Load program into currentPairProgram structure
        currentPairProgram.name = doc["name"].as<String>();
        currentPairProgram.loopEnabled = doc["loopEnabled"].as<bool>();
        currentPairProgram.waitForMicTrigger = doc["waitForMicTrigger"] | false; // Default to false
        currentPairProgram.micThreshold = doc["micThreshold"] | micDbThreshold; // Default to global mic threshold
        
        JsonArray blocks = doc["blocks"];
        currentPairProgram.blockCount = min((int)blocks.size(), 10); // Limit to 10 blocks
        
        for (int i = 0; i < currentPairProgram.blockCount; i++) {
          currentPairProgram.blocks[i].pairId = blocks[i]["pairId"];
          currentPairProgram.blocks[i].delayMs = blocks[i]["delayMs"];
          currentPairProgram.blocks[i].waitForInput = blocks[i]["waitForInput"];
        }
        
        // Start pair program execution
        currentMode = MODE_CUSTOM;
        
        // Check if program waits for mic trigger
        if (currentPairProgram.waitForMicTrigger) {
          waitingForCustomMicTrigger = true;
          pairProgramRunning = false; // Don't start yet, wait for mic
          Serial.println("Custom program loaded, waiting for mic trigger (threshold: " + String(currentPairProgram.micThreshold) + ")");
        } else {
          pairProgramRunning = true; // Start immediately
          waitingForCustomMicTrigger = false;
        }
        
        currentBlockIndex = 0;
        currentPairState = PAIR_IDLE;
        activePairId = -1;
        
        // Turn off all relays first
        for (int i = 0; i < 6; i++) {
          relays.digitalWrite(i, HIGH);
          inputTimeoutActive[i] = false;
        }
        
        Serial.println("Starting pair program: " + currentPairProgram.name);
        Serial.println("Blocks: " + String(currentPairProgram.blockCount));
        
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta http-equiv='refresh' content='3;url=/' />";
        html += "<title>Program Started</title></head><body>";
        html += "<h2>Pair Program '" + currentPairProgram.name + "' started successfully</h2>";
        html += "<p>Blocks: " + String(currentPairProgram.blockCount) + "</p>";
        html += "<p>Loop enabled: " + String(currentPairProgram.loopEnabled ? "Yes" : "No") + "</p>";
        html += "<p>Redirecting back to home page...</p>";
        html += "</body></html>";
        
        request->send(200, "text/html", html);
      } else {
        request->send(404, "text/plain", "Program not found");
      }
    } else {
      request->send(400, "text/plain", "Program name required");
    }
  });

  // Mode switching endpoint
  server.on("/set-mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("mode")) {
      String mode = request->getParam("mode")->value();
      if (mode == "sequence") {
        currentMode = MODE_SEQUENCE;
        stopPairProgram();
      } else if (mode == "custom") {
        currentMode = MODE_CUSTOM;
        // Stop sequence mode operations
        for (int i = 0; i < 6; i++) {
          relays.digitalWrite(i, HIGH);
          motorStates[i].waitingForInput = false;
          motorStates[i].isActive = false;
        }
        systemState = SYSTEM_STOPPED;
      }
      
      String html = "<!DOCTYPE html><html><head>";
      html += "<meta http-equiv='refresh' content='2;url=/' />";
      html += "<title>Mode Changed</title></head><body>";
      String modeText = mode;
      modeText.toUpperCase();
      html += "<h2>Mode changed to: " + modeText + "</h2>";
      html += "<p>Redirecting back to home page...</p>";
      html += "</body></html>";
      
      request->send(200, "text/html", html);
    } else {
      request->send(400, "text/plain", "Mode parameter required");
    }
  });

  // Stop custom program endpoint
  server.on("/stop-program", HTTP_GET, [](AsyncWebServerRequest *request) {
    stopPairProgram();
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='2;url=/' />";
    html += "<title>Program Stopped</title></head><body>";
    html += "<h2>Custom program stopped</h2>";
    html += "<p>Redirecting back to home page...</p>";
    html += "</body></html>";
    
    request->send(200, "text/html", html);
  });

  server.begin(); // Start the server

  
}

void loop() {
  // Check for timeouts first
  checkInputTimeouts();
  
  
  // Check motor safety before processing
  if (!checkMotorSafety()) {
    return; // Stop processing if safety check fails
  }
  
  // Only process if system is not in error state
  if (systemState == SYSTEM_ERROR) {
    delay(1000); // Wait before retrying
    return;
  }
  
  // Test DFRobot Gravity sound level meter on analog pin 4 (GPIO 35)
  // Only check microphone when system is stopped (not running)
  if (systemState == SYSTEM_STOPPED) {
    static unsigned long lastMicCheck = 0;
    if (millis() - lastMicCheck >= 125) { // Check every 125ms as per DFRobot sample
      float voltageValue, dbValue;
      // ESP32 uses 12-bit ADC (0-4095) and 3.3V reference instead of Arduino's 10-bit (0-1023) and 5V
      voltageValue = analogRead(ANALOG_A1) / 4095.0 * 3.3; // Convert to voltage (ESP32 specific)
      dbValue = voltageValue * 50.0; // Convert voltage to decibel value as per DFRobot formula
      Serial.print("Sound Level: ");
      Serial.print(dbValue, 1);
      Serial.print(" dBA (");
      Serial.print(voltageValue, 2);
      Serial.println("V)");
      
      // Check if we're waiting for mic trigger for custom program
      if (waitingForCustomMicTrigger && currentMode == MODE_CUSTOM) {
        if (dbValue >= currentPairProgram.micThreshold) {
          Serial.println("Custom program mic trigger activated! dB: " + String(dbValue, 1) + " >= " + String(currentPairProgram.micThreshold, 1) + " (program threshold)");
          Serial.println("Starting custom program: " + currentPairProgram.name);
          waitingForCustomMicTrigger = false;
          pairProgramRunning = true;
        }
      }
      // Trigger sequence mode when sound level exceeds configured threshold (only if not in custom mode)
      else if (dbValue >= micDbThreshold && currentMode == MODE_SEQUENCE) {
        // Check if relays 0, 2, 4 are off before starting (same logic as web interface)
        if (relays.digitalRead(0) == HIGH && relays.digitalRead(2) == HIGH && relays.digitalRead(4) == HIGH) {
          Serial.println("Sound trigger activated! dB: " + String(dbValue, 1) + " >= " + String(micDbThreshold, 1) + " (threshold)");
          Serial.println("Starting sequence mode...");
          relays.digitalWrite(0, LOW); // Turn on relay 0
          relays.digitalWrite(2, LOW); // Turn on relay 2  
          relays.digitalWrite(4, LOW); // Turn on relay 4
          
          // Start timeout monitoring for the relays that are now on
          startInputTimeout(0);
          startInputTimeout(2);
          startInputTimeout(4);
          
          systemState = SYSTEM_RUNNING;
        } else {
          Serial.println("Sound trigger detected (dB: " + String(dbValue, 1) + ") but relays already active");
        }
      }
      
      lastMicCheck = millis();
    }
  }
  
  // Handle different operating modes
  if (currentMode == MODE_SEQUENCE) {
    // Original sequence mode logic
    // Process each relay pair
    for (int relayPair = 0; relayPair < 3; relayPair++) {
      int relay1 = relayPair * 2;     // 0, 2, 4
      int relay2 = relayPair * 2 + 1; // 1, 3, 5
      
      // Check relay1 (even numbered relays: 0, 2, 4)
      if(safeInputRead(relay1) == LOW && 
         !motorStates[relay1].waitingForInput && 
         relays.digitalRead(relay1) == LOW) {
        
        stopInputTimeout(relay1);
        Serial.println("Input " + String(relay1) + " is LOW, turning off relay " + String(relay1));
        safeRelayWrite(relay1, HIGH);
        motorStates[relay1].delayStartTime = millis();
        motorStates[relay1].waitingForInput = true;
        // Generate a new random delay on every input change
        motorStates[relay1].currentRandomDelay = getRandomDelay(relay1);
        Serial.println("Starting delay timer for relay " + String(relay1) + " with NEW random delay: " + String(motorStates[relay1].currentRandomDelay) + "ms");
        delay(10);
      }
      
      if (motorStates[relay1].waitingForInput && 
          millis() - motorStates[relay1].delayStartTime >= motorStates[relay1].currentRandomDelay) {
        Serial.println("Random delay of " + String(motorStates[relay1].currentRandomDelay) + "ms completed for relay " + String(relay1) + ", activating relay " + String(relay2));
        safeRelayWrite(relay2, LOW);
        startInputTimeout(relay2);
        motorStates[relay1].waitingForInput = false;
        systemState = SYSTEM_RUNNING;
        delay(10);
      }

      // Check relay2 (odd numbered relays: 1, 3, 5)  
      if(safeInputRead(relay2) == LOW && 
         !motorStates[relay2].waitingForInput && 
         relays.digitalRead(relay2) == LOW) {
        
        stopInputTimeout(relay2);
        Serial.println("Input " + String(relay2) + " is LOW, turning off relay " + String(relay2));
        safeRelayWrite(relay2, HIGH);
        motorStates[relay2].delayStartTime = millis();
        motorStates[relay2].waitingForInput = true;
        // Generate a new random delay on every input change
        motorStates[relay2].currentRandomDelay = getRandomDelay(relay2);
        Serial.println("Starting delay timer for relay " + String(relay2) + " with NEW random delay: " + String(motorStates[relay2].currentRandomDelay) + "ms");
        delay(10);
      }
      
      if (motorStates[relay2].waitingForInput && 
          millis() - motorStates[relay2].delayStartTime >= motorStates[relay2].currentRandomDelay) {
        Serial.println("Random delay of " + String(motorStates[relay2].currentRandomDelay) + "ms completed for relay " + String(relay2) + ", activating relay " + String(relay1));
        safeRelayWrite(relay1, LOW);
        startInputTimeout(relay1);
        motorStates[relay2].waitingForInput = false;
        systemState = SYSTEM_RUNNING;
        delay(10);
      }
    }
  } else if (currentMode == MODE_CUSTOM && pairProgramRunning) {
    // Pair-based program execution logic
    executePairProgram();
  }
  
  delay(10);
}

// Function to get a random delay in milliseconds for a specific relay
int getRandomDelay(int relay) {
    // Use ESP32's hardware random number generator
    uint32_t randomValue = esp_random();
    
    // Scale it to your min/max range
    int range = maxDelayRelay[relay] - minDelayRelay[relay];
    int delay = minDelayRelay[relay] + (randomValue % range);
    
    Serial.println("Random delay for relay " + String(relay) + ": " + String(delay) + "ms (range: " + String(minDelayRelay[relay]) + "-" + String(maxDelayRelay[relay]) + "ms)");
    
    return delay;
}

void saveRelayDelays() {
  preferences.begin("relayDelays", false); // Open preferences in read/write mode
  for (int i = 0; i < 6; i++) {
    preferences.putInt(("minDelay" + String(i)).c_str(), minDelayRelay[i]);
    preferences.putInt(("maxDelay" + String(i)).c_str(), maxDelayRelay[i]);
  }
  preferences.end(); // Close preferences
  Serial.println("Relay delays saved to flash.");
}

void loadRelayDelays() {
  preferences.begin("relayDelays", true); // Open preferences in read-only mode
  for (int i = 0; i < 6; i++) {
    minDelayRelay[i] = preferences.getInt(("minDelay" + String(i)).c_str(), minDelayRelay[i]); // Default to current value
    maxDelayRelay[i] = preferences.getInt(("maxDelay" + String(i)).c_str(), maxDelayRelay[i]); // Default to current value
  }
  preferences.end(); // Close preferences
  Serial.println("Relay delays loaded from flash.");
}

void saveSafetyTimeout() {
  preferences.begin("safetyConfig", false); // Open preferences in read/write mode
  preferences.putInt("timeoutMs", safetyTimeoutMs);
  preferences.end(); // Close preferences
  Serial.println("Safety timeout saved to flash: " + String(safetyTimeoutMs) + "ms");
}

void loadSafetyTimeout() {
  preferences.begin("safetyConfig", true); // Open preferences in read-only mode
  safetyTimeoutMs = preferences.getInt("timeoutMs", 1000); // Default to 1000ms if not found
  preferences.end(); // Close preferences
  Serial.println("Safety timeout loaded from flash: " + String(safetyTimeoutMs) + "ms");
}

void saveMicDbThreshold() {
  preferences.begin("micConfig", false); // Open preferences in read/write mode
  preferences.putFloat("dbThreshold", micDbThreshold);
  preferences.end(); // Close preferences
  Serial.println("Microphone dB threshold saved to flash: " + String(micDbThreshold, 1) + " dBA");
}

void loadMicDbThreshold() {
  preferences.begin("micConfig", true); // Open preferences in read-only mode
  micDbThreshold = preferences.getFloat("dbThreshold", 50.0); // Default to 50.0 dBA if not found
  preferences.end(); // Close preferences
  Serial.println("Microphone dB threshold loaded from flash: " + String(micDbThreshold, 1) + " dBA");
}

// Custom program management functions
bool saveProgram(const String& programName, const String& programData) {
  String filename = "/programs/" + programName + ".json";
  
  // Create directory if it doesn't exist
  if (!SPIFFS.exists("/programs")) {
    File dir = SPIFFS.open("/programs", "w");
    if (dir) dir.close();
  }
  
  File file = SPIFFS.open(filename, "w");
  if (!file) {
    Serial.println("Failed to create program file: " + filename);
    return false;
  }
  
  file.print(programData);
  file.close();
  
  Serial.println("Program saved: " + programName);
  return true;
}

String loadProgram(const String& programName) {
  String filename = "/programs/" + programName + ".json";
  
  if (!SPIFFS.exists(filename)) {
    Serial.println("Program file not found: " + filename);
    return "";
  }
  
  File file = SPIFFS.open(filename, "r");
  if (!file) {
    Serial.println("Failed to open program file: " + filename);
    return "";
  }
  
  String content = file.readString();
  file.close();
  
  Serial.println("Program loaded: " + programName);
  return content;
}

String getProgramList() {
  String result = "{\"programs\":[";
  
  File root = SPIFFS.open("/programs");
  if (!root || !root.isDirectory()) {
    result += "]}";
    return result;
  }
  
  File file = root.openNextFile();
  bool first = true;
  
  while (file) {
    if (!file.isDirectory()) {
      String filename = file.name();
      if (filename.endsWith(".json")) {
        if (!first) result += ",";
        filename.replace(".json", "");
        filename.replace("/programs/", "");
        result += "\"" + filename + "\"";
        first = false;
      }
    }
    file = root.openNextFile();
  }
  
  result += "]}";
  return result;
}

bool deleteProgram(const String& programName) {
  String filename = "/programs/" + programName + ".json";
  
  if (!SPIFFS.exists(filename)) {
    Serial.println("Program file not found: " + filename);
    return false;
  }
  
  bool success = SPIFFS.remove(filename);
  if (success) {
    Serial.println("Program deleted: " + programName);
  } else {
    Serial.println("Failed to delete program: " + programName);
  }
  
  return success;
}

bool validateProgramSafety(const String& programData) {
  // Parse JSON to validate program safety
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, programData);
  
  if (error) {
    Serial.println("Invalid JSON format in program");
    return false;
  }
  
  if (!doc["steps"].is<JsonArray>()) {
    Serial.println("Program missing steps array");
    return false;
  }
  
  JsonArray steps = doc["steps"];
  
  // Check each step for safety violations
  for (size_t i = 0; i < steps.size(); i++) {
    JsonObject step = steps[i];
    
    if (!step["relay"].is<int>() || !step["delay"].is<int>() || !step["waitForInput"].is<bool>()) {
      Serial.println("Invalid step format at index " + String(i));
      return false;
    }
    
    int relay = step["relay"];
    if (relay < 0 || relay >= 6) {
      Serial.println("Invalid relay number " + String(relay) + " at step " + String(i));
      return false;
    }
    
    int delay = step["delay"];
    if (delay < 100 || delay > 60000) {
      Serial.println("Invalid delay " + String(delay) + " at step " + String(i));
      return false;
    }
    
    // Check for consecutive steps with relays from same motor pair
    if (i > 0) {
      JsonObject prevStep = steps[i-1];
      int prevRelay = prevStep["relay"];
      
      // Check if current and previous relays are from same motor pair
      if ((relay / 2) == (prevRelay / 2) && relay != prevRelay) {
        Serial.println("Safety violation: Consecutive relays " + String(prevRelay) + " and " + String(relay) + " from same motor pair");
        return false;
      }
    }
  }
  
  Serial.println("Program safety validation passed");
  return true;
}

// Helper functions for pair-based operations
int getPairFirstRelay(int pairId) {
  return pairId * 2; // Returns 0, 2, 4
}

int getPairSecondRelay(int pairId) {
  return pairId * 2 + 1; // Returns 1, 3, 5
}

void executePairProgram() {
  if (!pairProgramRunning || currentPairProgram.blockCount == 0) {
    return;
  }

  MotorPairBlock& currentBlock = currentPairProgram.blocks[currentBlockIndex];
  
  // Declare variables outside switch to avoid scope issues
  int firstRelay, secondRelay;
  
  switch (currentPairState) {
    case PAIR_IDLE:
      // Start the current block - activate first relay of the pair
      activePairId = currentBlock.pairId;
      firstRelay = getPairFirstRelay(activePairId);
      
      // Safety check: ensure no relay from same pair is already on
      secondRelay = getPairSecondRelay(activePairId);
      if (relays.digitalRead(secondRelay) == LOW) {
        // Safety violation - stop program
        Serial.println("Safety violation: Relay " + String(secondRelay) + " already active in pair " + String(activePairId));
        stopPairProgram();
        return;
      }
      
      // Activate first relay
      safeRelayWrite(firstRelay, LOW);
      startInputTimeout(firstRelay);
      currentPairState = PAIR_FIRST_RELAY;
      pairStateStartTime = millis();
      systemState = SYSTEM_RUNNING;
      
      Serial.println("Pair program block " + String(currentBlockIndex) + ": Starting pair " + String(activePairId) + ", activated relay " + String(firstRelay));
      break;
      
    case PAIR_FIRST_RELAY:
      // Wait for input on first relay
      firstRelay = getPairFirstRelay(activePairId);
      if (safeInputRead(firstRelay) == LOW && relays.digitalRead(firstRelay) == LOW) {
        // Input detected on first relay
        stopInputTimeout(firstRelay);
        safeRelayWrite(firstRelay, HIGH);
        
        // Start delay period
        currentPairState = PAIR_DELAY;
        pairDelayStartTime = millis();
        
        Serial.println("Pair " + String(activePairId) + ": First relay input detected, starting delay of " + String(currentBlock.delayMs) + "ms");
      }
      break;
      
    case PAIR_DELAY:
      // Wait for delay to complete
      if (millis() - pairDelayStartTime >= currentBlock.delayMs) {
        // Delay complete, activate second relay
        secondRelay = getPairSecondRelay(activePairId);
        safeRelayWrite(secondRelay, LOW);
        startInputTimeout(secondRelay);
        currentPairState = PAIR_SECOND_RELAY;
        
        Serial.println("Pair " + String(activePairId) + ": Delay complete, activated relay " + String(secondRelay));
      }
      break;
      
    case PAIR_SECOND_RELAY:
      // Wait for input on second relay
      secondRelay = getPairSecondRelay(activePairId);
      if (safeInputRead(secondRelay) == LOW && relays.digitalRead(secondRelay) == LOW) {
        // Input detected on second relay
        stopInputTimeout(secondRelay);
        safeRelayWrite(secondRelay, HIGH);
        currentPairState = PAIR_COMPLETE;
        
        Serial.println("Pair " + String(activePairId) + ": Second relay input detected, block complete");
      }
      break;
      
    case PAIR_COMPLETE:
      // Move to next block
      currentBlockIndex++;
      
      if (currentBlockIndex >= currentPairProgram.blockCount) {
        // Program complete
        if (currentPairProgram.loopEnabled) {
          currentBlockIndex = 0; // Loop back to start
          currentPairState = PAIR_IDLE;
          Serial.println("Pair program loop: restarting from block 0");
        } else {
          stopPairProgram(); // Program complete
          Serial.println("Pair program completed");
          return;
        }
      } else {
        // Move to next block
        currentPairState = PAIR_IDLE;
        Serial.println("Moving to next block: " + String(currentBlockIndex));
      }
      break;
  }
}

void stopPairProgram() {
  pairProgramRunning = false;
  currentPairState = PAIR_IDLE;
  currentBlockIndex = 0;
  activePairId = -1;
  
  // Turn off all relays
  for (int i = 0; i < 6; i++) {
    relays.digitalWrite(i, HIGH);
    inputTimeoutActive[i] = false;
  }
  
  systemState = SYSTEM_STOPPED;
  Serial.println("Pair program stopped");
}

bool checkRelayPairSafety(int relay1, int relay2) {
  // Check if two relays are from the same motor pair
  int pair1 = relay1 / 2;
  int pair2 = relay2 / 2;
  
  return pair1 != pair2; // Safe if from different pairs
}

// Pair program management functions - basic implementations
bool savePairProgram(const String& programName, const String& programData) {
  String filename = "/program_" + programName + ".json";
  
  Serial.println("Saving pair program to: " + filename);
  
  File file = SPIFFS.open(filename, "w");
  if (!file) {
    Serial.println("Failed to create program file: " + filename);
    return false;
  }
  
  file.print(programData);
  file.close();
  
  Serial.println("Pair program saved: " + programName);
  return true;
}

String loadPairProgram(const String& programName) {
  String filename = "/program_" + programName + ".json";
  
  if (!SPIFFS.exists(filename)) {
    Serial.println("Pair program file not found: " + filename);
    return "";
  }
  
  File file = SPIFFS.open(filename, "r");
  if (!file) {
    Serial.println("Failed to open pair program file: " + filename);
    return "";
  }
  
  String content = file.readString();
  file.close();
  
  Serial.println("Pair program loaded: " + programName);
  return content;
}

String getPairProgramList() {
  // Return JSON object with programs array for compatibility with frontend
  String json = "{\"programs\":[";
  bool first = true;
  
  Serial.println("Getting pair program list...");
  
  File root = SPIFFS.open("/");
  if (root && root.isDirectory()) {
    Serial.println("SPIFFS root directory found");
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String filename = file.name();
        Serial.println("Found file: " + filename);
        // Look for files starting with "program_" and ending with ".json"
        Serial.println("Checking if '" + filename + "' starts with 'program_': " + String(filename.startsWith("program_")));
        Serial.println("Checking if '" + filename + "' ends with '.json': " + String(filename.endsWith(".json")));
        if (filename.startsWith("program_") && filename.endsWith(".json")) {
          Serial.println("File matches criteria!");
          if (!first) json += ",";
          // Extract program name from filename (remove "program_" and ".json")
          String programName = filename.substring(8); // Remove "program_" (8 characters)
          programName.replace(".json", ""); // Remove ".json"
          Serial.println("Extracted program name: '" + programName + "'");
          json += "\"" + programName + "\"";
          first = false;
          Serial.println("Added program: " + programName);
        } else {
          Serial.println("File does not match criteria, skipping");
        }
      }
      file = root.openNextFile();
    }
    root.close();
  } else {
    Serial.println("SPIFFS root directory not found or not accessible");
  }
  
  json += "]}";
  Serial.println("Program list JSON: " + json);
  return json;
}

bool deletePairProgram(const String& programName) {
  String filename = "/program_" + programName + ".json";
  
  if (!SPIFFS.exists(filename)) {
    Serial.println("Pair program file not found: " + filename);
    return false;
  }
  
  bool success = SPIFFS.remove(filename);
  if (success) {
    Serial.println("Pair program deleted: " + programName);
  } else {
    Serial.println("Failed to delete pair program: " + programName);
  }
  
  return success;
}

bool validatePairProgramSafety(const String& programData) {
  // Parse JSON to validate pair program safety
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, programData);
  
  if (error) {
    Serial.println("Invalid JSON format in pair program");
    return false;
  }
  
  if (!doc["blocks"].is<JsonArray>()) {
    Serial.println("Pair program missing blocks array");
    return false;
  }
  
  JsonArray blocks = doc["blocks"];
  
  // Check each block for safety violations
  for (size_t i = 0; i < blocks.size(); i++) {
    JsonObject block = blocks[i];
    
    if (!block["pairId"].is<int>() || !block["delayMs"].is<int>() || !block["waitForInput"].is<bool>()) {
      Serial.println("Invalid block format at index " + String(i));
      return false;
    }
    
    int pairId = block["pairId"];
    if (pairId < 0 || pairId > 2) {
      Serial.println("Invalid pair ID " + String(pairId) + " at block " + String(i) + " (must be 0-2)");
      return false;
    }
    
    int delayMs = block["delayMs"];
    if (delayMs < 100 || delayMs > 60000) {
      Serial.println("Invalid delay " + String(delayMs) + " at block " + String(i) + " (must be 100-60000ms)");
      return false;
    }
  }
  
  Serial.println("Pair program safety validation passed");
  return true;
}