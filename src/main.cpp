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
  MODE_CUSTOM,    // Custom program mode
  MODE_ZAWODY,    // Competition mode
  MODE_MANUAL     // Manual mode
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

// Competition mode structures
struct CompetitionSettings {
  float micTriggerThreshold;
  int target1DelayStart;  // Delay before target 1 starts (ms)
  int target1ShowTime;    // Time to show target 1 (ms)
  int target1HideTime;    // Time to hide target 1 (ms)
  int target2DelayStart;  // Delay before target 2 starts (ms)
  int target2ShowTime;    // Time to show target 2 (ms)
  int target2HideTime;    // Time to hide target 2 (ms)
  int target3DelayStart;  // Delay before target 3 starts (ms)
  int target3ShowTime;    // Time to show target 3 (ms)
  int target3HideTime;    // Time to hide target 3 (ms)
  int repetitions;        // How many times each target should appear
};

// Competition state tracking
struct CompetitionState {
  bool isRunning;
  bool waitingForMicTrigger;
  bool initialized;      // Targets in hidden position
  unsigned long timerStart;
  int currentCycle;      // Current repetition cycle
  int target1Count;      // How many times target 1 appeared
  int target2Count;      // How many times target 2 appeared
  int target3Count;      // How many times target 3 appeared
  bool target1Active;    // Is target 1 currently showing
  bool target2Active;    // Is target 2 currently showing
  bool target3Active;    // Is target 3 currently showing
  unsigned long target1StateTime;
  unsigned long target2StateTime;
  unsigned long target3StateTime;
};

// Manual mode state
struct ManualState {
  bool target1Moving;
  bool target2Moving;
  bool target3Moving;
  unsigned long target1MoveStart;
  unsigned long target2MoveStart;
  unsigned long target3MoveStart;
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

// Competition mode variables
CompetitionSettings competitionSettings = {50.0, 0, 2000, 2000, 1000, 2000, 2000, 2000, 2000, 2000, 3}; // Default settings with delays
CompetitionState competitionState = {false, false, false, 0, 0, 0, 0, 0, false, false, false, 0, 0, 0};

// Manual mode variables
ManualState manualState = {false, false, false, 0, 0, 0};

// --- Hardware ---
PCF8574 inputs(0x22, 4, 15);   // Input expander, address 0x22
PCF8574 relays(0x24, 4, 15);   // Relay expander, address 0x24

Preferences preferences;

// --- WiFi Credentials ---
const char* ssid = "TARCZOWNIX"; // SSID for the access point
const char* password = "password"; // Password for the access point


// Separate delay configurations for each relay
int minDelayRelay[6] = {1000, 1000, 1000, 1000, 1000, 1000}; // Default min delay for each relay
int maxDelayRelay[6] = {5000, 5000, 5000, 5000, 5000, 5000}; // Default max delay for each relay

// Safety timeout configuration (in milliseconds)
int safetyTimeoutMs = 1000; // Default 1 second timeout

// Microphone dB threshold configuration (no longer used for sequence mode)
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

// Competition mode functions
bool isTargetHidden(int targetId);
bool isTargetShown(int targetId);
bool initializeTargetsToHidden();
void startCompetitionMode();
void executeCompetitionMode();
void handleCompetitionTarget(int targetId, unsigned long elapsedTime, unsigned long delayStart, int showTime, int hideTime, int& targetCount, bool& targetActive, unsigned long& stateTime);
void showTarget(int targetId);
void hideTarget(int targetId);
void stopCompetitionMode();
void saveCompetitionSettings();
void loadCompetitionSettings();

// Manual mode functions
void executeManualMode();
void manualShowTarget(int targetId);
void manualHideTarget(int targetId);
void manualStopTarget(int targetId);
String getTargetStatus();

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
  
  // Use longer timeout for competition mode
  int timeoutMs = (currentMode == MODE_ZAWODY) ? 5000 : safetyTimeoutMs; // 5 seconds for competition, normal for others
  
  for (int i = 0; i < 6; i++) {
    if (inputTimeoutActive[i]) {
      // Check if safety timeout has passed
      if (currentTime - inputTimeoutStart[i] >= timeoutMs) {
        
        if (currentMode == MODE_ZAWODY) {
          // In competition mode, just stop the specific relay that timed out
          relays.digitalWrite(i, HIGH); // Turn off the specific relay
          inputTimeoutActive[i] = false; // Clear timeout for this relay
          
          // Debug information for the timeout
          Serial.println("DEBUG: Relay " + String(i) + " timeout - checking target position:");
          if (i == 0) { // Target 1 show relay
            Serial.println("DEBUG: Target 1 - Input 0 (shown): " + String(safeInputRead(0)) + ", Input 1 (hidden): " + String(safeInputRead(1)));
          } else if (i == 1) { // Target 1 hide relay
            Serial.println("DEBUG: Target 1 - Input 0 (shown): " + String(safeInputRead(0)) + ", Input 1 (hidden): " + String(safeInputRead(1)));
          } else if (i == 2) { // Target 2 show relay
            Serial.println("DEBUG: Target 2 - Input 2 (shown): " + String(safeInputRead(2)) + ", Input 3 (hidden): " + String(safeInputRead(3)));
          } else if (i == 3) { // Target 2 hide relay
            Serial.println("DEBUG: Target 2 - Input 2 (shown): " + String(safeInputRead(2)) + ", Input 3 (hidden): " + String(safeInputRead(3)));
          } else if (i == 4) { // Target 3 show relay
            Serial.println("DEBUG: Target 3 - Input 4 (shown): " + String(safeInputRead(4)) + ", Input 5 (hidden): " + String(safeInputRead(5)));
          } else if (i == 5) { // Target 3 hide relay
            Serial.println("DEBUG: Target 3 - Input 4 (shown): " + String(safeInputRead(4)) + ", Input 5 (hidden): " + String(safeInputRead(5)));
          }
          
          // Create warning message but don't stop competition
          lastErrorMessage = "Competition: Target relay " + String(i) + " timed out after " + String(timeoutMs) + "ms - continuing";
          lastErrorTime = currentTime;
          
          Serial.println("TIMEOUT WARNING: " + lastErrorMessage);
          // Continue with competition, don't break
          
        } else {
          // In other modes, stop everything as before
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
          
          // Create error message with appropriate timeout value
          lastErrorMessage = "Relay " + String(i) + " did not reach input " + String(i) + " before " + String(timeoutMs) + "ms timeout";
          lastErrorTime = currentTime;
          
          Serial.println("TIMEOUT ERROR: " + lastErrorMessage);
          break; // Exit loop since we've handled the timeout
        }
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

  // Load competition settings
  loadCompetitionSettings();

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

  // Add endpoint for microphone dB threshold configuration (used by custom programs)
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

        message = "Microphone threshold updated to " + String(newThreshold, 1) + " dBA (for custom programs)";
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
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    // Remove auto-refresh for better UX with program selector
    html += "<title>TARCZOWNIX Control</title>";
    html += "<style>";
    html += "* { box-sizing: border-box; }";
    html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Arial, sans-serif; ";
    html += "text-align: center; margin: 0; padding: 10px; background-color: #f5f5f5; ";
    html += "font-size: 16px; line-height: 1.4; }";
    html += ".container { max-width: 1000px; margin: 0 auto; padding: 15px; border: 1px solid #ddd; ";
    html += "border-radius: 10px; background-color: white; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }";
    html += "h1 { color: #333; margin: 0 0 20px 0; font-size: clamp(1.5rem, 4vw, 2.5rem); }";
    html += "h2 { color: #333; margin: 0 0 15px 0; font-size: clamp(1.2rem, 3vw, 1.8rem); }";
    html += ".btn { background-color: #4CAF50; border: none; color: white; ";
    html += "padding: 12px 20px; text-align: center; text-decoration: none; ";
    html += "display: inline-block; font-size: 16px; margin: 8px 4px; ";
    html += "cursor: pointer; border-radius: 8px; transition: all 0.3s ease; ";
    html += "min-width: 120px; touch-action: manipulation; }";
    html += "@media (max-width: 768px) { .btn { display: block; width: 100%; margin: 8px 0; padding: 15px; } }";
    html += ".btn:hover, .btn:focus { background-color: #45a049; transform: translateY(-1px); outline: none; }";
    html += ".btn:active { transform: translateY(0); }";
    html += ".btn-stop { background-color: #f44336; }";
    html += ".btn-stop:hover, .btn-stop:focus { background-color: #da190b; }";
    html += ".btn-clear { background-color: #ff9800; }";
    html += ".btn-clear:hover, .btn-clear:focus { background-color: #e68900; }";
    html += ".form-group { margin: 15px 0; text-align: left; }";
    html += "input[type=number], select { padding: 12px; min-width: 120px; border-radius: 4px; ";
    html += "border: 1px solid #ccc; font-size: 16px; }";
    html += "@media (max-width: 768px) { input[type=number], select { width: 100%; } }";
    html += "label { display: inline-block; margin-right: 10px; font-weight: bold; }";
    html += "@media (max-width: 768px) { label { display: block; margin-bottom: 5px; } }";
    html += ".card { border: 1px solid #ddd; border-radius: 8px; padding: 15px; margin: 15px 0; ";
    html += "background-color: #f9f9f9; }";
    html += ".status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); ";
    html += "gap: 15px; margin: 20px 0; }";
    html += "@media (max-width: 768px) { .status-grid { grid-template-columns: 1fr; gap: 10px; } }";
    html += ".status-item { background-color: white; padding: 15px; border-radius: 8px; ";
    html += "border-left: 4px solid #4CAF50; }";
    html += ".status-on { border-left-color: #f44336; background-color: #ffe6e6; }";
    html += ".status-off { border-left-color: #4CAF50; background-color: #e6ffe6; }";
    html += ".motor-pair { background-color: #e3f2fd; border: 1px solid #1976d2; ";
    html += "margin: 10px 0; padding: 15px; border-radius: 8px; }";
    html += ".warning { color: #ff6b35; font-weight: bold; }";
    html += ".success { color: #4CAF50; font-weight: bold; }";
    html += ".relay-config { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 20px; }";
    html += "@media (max-width: 768px) { .relay-config { grid-template-columns: 1fr; } }";
    html += "table { width: 100%; border-collapse: collapse; margin: 15px 0; }";
    html += "th, td { padding: 8px; text-align: left; border: 1px solid #ddd; }";
    html += "@media (max-width: 768px) { th, td { padding: 6px; font-size: 14px; } }";
    html += "th { background-color: #f2f2f2; font-weight: bold; }";
    html += ".button-group { display: flex; flex-wrap: wrap; gap: 8px; justify-content: center; }";
    html += "@media (max-width: 768px) { .button-group { flex-direction: column; } }";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<h1>TARCZOWNIX Motor Control System</h1>";
    html += "<button onclick='location.reload()' class='btn' style='background-color: #2196f3; margin-bottom: 20px;'>&#8635; Refresh Status</button>";

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
    switch(currentMode) {
      case MODE_SEQUENCE: 
        html += "<span style='color: #2196f3;'>SEQUENCE</span>"; 
        break;
      case MODE_CUSTOM: 
        html += "<span style='color: #2196f3;'>CUSTOM PROGRAM</span>"; 
        break;
      case MODE_ZAWODY: 
        html += "<span style='color: #9c27b0;'>COMPETITION</span>"; 
        break;
      case MODE_MANUAL: 
        html += "<span style='color: #ff5722;'>MANUAL</span>"; 
        break;
    }
    html += "</p>";
    html += "<p><strong>Custom Program Mic Threshold:</strong> <span id='mic-threshold'>" + String(micDbThreshold, 1) + " dBA</span></p>";
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
    } else if (currentMode == MODE_ZAWODY) {
      if (competitionState.waitingForMicTrigger) {
        html += "<p><strong>Status:</strong> <span style='color: #ff9800;'>Competition ready - waiting for mic trigger (" + String(competitionSettings.micTriggerThreshold, 1) + " dBA)</span></p>";
      } else if (competitionState.isRunning) {
        html += "<p><strong>Status:</strong> <span style='color: #4CAF50;'>Competition running</span></p>";
        html += "<p><strong>Target Progress:</strong> T1:" + String(competitionState.target1Count) + "/" + String(competitionSettings.repetitions) + 
                " T2:" + String(competitionState.target2Count) + "/" + String(competitionSettings.repetitions) + 
                " T3:" + String(competitionState.target3Count) + "/" + String(competitionSettings.repetitions) + "</p>";
      } else {
        html += "<p><strong>Status:</strong> <span style='color: #757575;'>Competition mode inactive</span></p>";
      }
    } else if (currentMode == MODE_MANUAL) {
      html += "<p><strong>Status:</strong> <span style='color: #ff5722;'>Manual control active</span></p>";
    }
    html += "<p><strong>Last Error:</strong> " + getLastError() + "</p>";
    if (lastErrorTime > 0) {
      html += "<a href='/clear-error' class='btn btn-clear'>Clear Error</a>";
    }
    html += "</div>";

    // Mode Selection
    html += "<div class='card'>";
    html += "<h2>Operating Mode</h2>";
    html += "<div class='button-group'>";
    html += "<a href='/set-mode?mode=sequence' class='btn " + String(currentMode == MODE_SEQUENCE ? "btn-stop" : "") + "'>Sequence Mode</a>";
    html += "<a href='/set-mode?mode=custom' class='btn " + String(currentMode == MODE_CUSTOM ? "btn-stop" : "") + "'>Custom Program Mode</a>";
    html += "<a href='/start-competition' class='btn " + String(currentMode == MODE_ZAWODY ? "btn-stop" : "") + "' style='background-color: #9c27b0;'>Competition Mode</a>";
    html += "<a href='/start-manual' class='btn " + String(currentMode == MODE_MANUAL ? "btn-stop" : "") + "' style='background-color: #ff5722;'>Manual Mode</a>";
    html += "</div>";
    html += "<div class='button-group' style='margin-top: 15px;'>";
    html += "<a href='/program-editor' class='btn btn-clear'>Program Editor</a>";
    html += "<a href='/competition-settings' class='btn btn-clear' style='background-color: #673ab7;'>Competition Settings</a>";
    html += "</div>";
    html += "</div>";

    // Relay control section with better visualization
    html += "<div class='card'>";
    if (currentMode == MODE_SEQUENCE) {
      html += "<h2>Sequence Control</h2>";
      html += "<div class='button-group'>";
      html += "<a href='/start' class='btn'>Start Sequence</a>";
      html += "<a href='/stop' class='btn btn-stop'>Stop Sequence</a>";
      html += "</div>";
    } else if (currentMode == MODE_CUSTOM) {
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
    } else if (currentMode == MODE_ZAWODY) {
      html += "<h2>Competition Control</h2>";
      if (competitionState.isRunning) {
        html += "<a href='/stop-competition' class='btn btn-stop'>Stop Competition</a>";
      } else if (competitionState.waitingForMicTrigger) {
        html += "<p><span style='color: #ff9800;'>Competition initialized - waiting for mic trigger</span></p>";
        html += "<a href='/stop-competition' class='btn btn-stop'>Cancel Competition</a>";
      } else {
        html += "<a href='/start-competition' class='btn' style='background-color: #9c27b0;'>Start Competition</a>";
      }
    } else if (currentMode == MODE_MANUAL) {
      html += "<h2>Manual Target Control</h2>";
      html += "<div id='manual-controls'>";
      for (int target = 1; target <= 3; target++) {
        html += "<div style='border: 1px solid #ccc; margin: 10px 0; padding: 15px; border-radius: 8px;'>";
        html += "<h3>Target " + String(target) + "</h3>";
        html += "<div class='button-group' style='margin-bottom: 10px;'>";
        html += "<button onclick='manualControl(" + String(target) + ", \"show\")' class='btn'>Show</button>";
        html += "<button onclick='manualControl(" + String(target) + ", \"hide\")' class='btn btn-clear'>Hide</button>";
        html += "<button onclick='manualControl(" + String(target) + ", \"stop\")' class='btn btn-stop'>Stop</button>";
        html += "</div>";
        html += "<div id='target" + String(target) + "-status' style='font-size: 14px; color: #666; padding: 8px; background-color: #f9f9f9; border-radius: 4px;'>Loading status...</div>";
        html += "</div>";
      }
      html += "</div>";
      html += "<script>";
      html += "function manualControl(target, action) {";
      html += "  fetch('/manual-' + action + '?target=' + target)";
      html += "    .then(r => r.text())";
      html += "    .then(d => console.log(d))";
      html += "    .catch(e => alert('Error: ' + e));";
      html += "  setTimeout(updateTargetStatus, 100);";
      html += "}";
      html += "function updateTargetStatus() {";
      html += "  fetch('/target-status')";
      html += "    .then(r => r.json())";
      html += "    .then(d => {";
      html += "      for (let i = 1; i <= 3; i++) {";
      html += "        const targetKey = 'target' + i;";
      html += "        const status = d[targetKey];";
      html += "        let text = '';";
      html += "        if (status.moving) text = 'Moving...';";
      html += "        else if (status.shown) text = 'Shown';";
      html += "        else if (status.hidden) text = 'Hidden';";
      html += "        else text = 'Unknown position';";
      html += "        document.getElementById(targetKey + '-status').textContent = 'Status: ' + text;";
      html += "      }";
      html += "    })";
      html += "    .catch(e => console.error('Status update error:', e));";
      html += "}";
      html += "updateTargetStatus();";
      html += "setInterval(updateTargetStatus, 2000);";
      html += "</script>";
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

    // Microphone Configuration (for Custom Programs)
    html += "<div class='card'>";
    html += "<h2>Microphone Configuration</h2>";
    html += "<div style='border: 1px solid #ddd; padding: 15px; border-radius: 8px; background-color: white;'>";
    html += "<h3>Sound Trigger Threshold</h3>";
    html += "<p>Current threshold: <strong>" + String(micDbThreshold, 1) + " dBA</strong></p>";
    html += "<p><small>This threshold is used for custom programs that have microphone trigger enabled. Competition mode has its own threshold setting.</small></p>";
    html += "<form action='/set-mic-threshold' method='get'>";
    html += "<div class='form-group'>";
    html += "<label for='threshold'>Threshold (dBA):</label>";
    html += "<input type='number' id='threshold' name='threshold' min='20' max='120' step='0.1' value='" + String(micDbThreshold, 1) + "' required>";
    html += "</div>";
    html += "<input type='submit' class='btn' value='Update Microphone Threshold' style='width: 100%;'>";
    html += "</form>";
    html += "</div>";
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
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>TARCZOWNIX - Target Program Editor</title>";
    html += "<style>";
    html += "* { box-sizing: border-box; }";
    html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Arial, sans-serif; ";
    html += "margin: 0; padding: 15px; background-color: #f5f5f5; font-size: 16px; line-height: 1.4; }";
    html += ".container { max-width: 1200px; margin: 0 auto; background-color: white; ";
    html += "padding: 20px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }";
    html += "h1 { text-align: center; color: #333; margin-bottom: 20px; font-size: clamp(1.5rem, 4vw, 2.2rem); }";
    html += ".editor-section { border: 2px dashed #ccc; padding: 20px; margin: 20px 0; ";
    html += "min-height: 200px; border-radius: 8px; }";
    html += ".target-item { background-color: #e3f2fd; border: 1px solid #1976d2; ";
    html += "padding: 15px; margin: 10px; border-radius: 5px; cursor: grab; ";
    html += "display: inline-block; min-width: 180px; text-align: center; }";
    html += "@media (max-width: 768px) { .target-item { display: block; width: 100%; margin: 10px 0; } }";
    html += ".target-item:active { cursor: grabbing; }";
    html += ".target-item h4 { margin: 5px 0; color: #1976d2; }";
    html += ".target-item .relays { font-size: 14px; color: #666; }";
    html += ".block-item { background-color: #f1f8e9; border: 1px solid #689f38; ";
    html += "padding: 15px; margin: 10px 0; border-radius: 8px; position: relative; }";
    html += ".block-controls { margin-top: 15px; }";
    html += ".block-controls input, .block-controls select { margin: 5px; padding: 8px; ";
    html += "border: 1px solid #ddd; border-radius: 4px; font-size: 16px; }";
    html += "@media (max-width: 768px) { .block-controls input, .block-controls select { width: 100%; margin: 5px 0; } }";
    html += ".btn { background-color: #4CAF50; border: none; color: white; ";
    html += "padding: 12px 20px; text-decoration: none; display: inline-block; ";
    html += "border-radius: 5px; cursor: pointer; margin: 5px; font-size: 16px; ";
    html += "transition: all 0.3s ease; touch-action: manipulation; }";
    html += "@media (max-width: 768px) { .btn { display: block; width: 100%; margin: 5px 0; text-align: center; } }";
    html += ".btn:hover, .btn:focus { transform: translateY(-1px); outline: none; }";
    html += ".btn-danger { background-color: #f44336; }";
    html += ".btn-danger:hover { background-color: #da190b; }";
    html += ".btn-warning { background-color: #ff9800; }";
    html += ".btn-warning:hover { background-color: #e68900; }";
    html += ".btn-secondary { background-color: #6c757d; }";
    html += ".btn-secondary:hover { background-color: #5a6268; }";
    html += ".form-group { margin: 15px 0; }";
    html += ".form-group label { display: block; margin-bottom: 5px; font-weight: bold; }";
    html += "@media (min-width: 769px) { .form-group label { display: inline-block; width: 150px; margin-bottom: 0; } }";
    html += ".safety-info { background-color: #e8f5e8; border: 2px solid #4CAF50; ";
    html += "padding: 15px; margin: 20px 0; border-radius: 8px; }";
    html += ".program-list { background-color: #f9f9f9; padding: 15px; border-radius: 8px; margin: 20px 0; }";
    html += ".program-item { background-color: white; padding: 15px; margin: 5px 0; ";
    html += "border-radius: 5px; border: 1px solid #ddd; }";
    html += "@media (min-width: 769px) { .program-item { display: flex; justify-content: space-between; align-items: center; } }";
    html += ".button-group { display: flex; gap: 10px; flex-wrap: wrap; margin-top: 15px; }";
    html += "@media (max-width: 768px) { .button-group { flex-direction: column; gap: 0; } }";
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
    html += "  if (draggedElement && draggedElement.classList.contains('target-item')) {";
    html += "    addBlock(draggedElement.getAttribute('data-pair'));";
    html += "  }";
    html += "}";
    
    // Function to add a target block
    html += "function addBlock(pairId) {";
    html += "  const targetNames = ['Target 0 (Relays 0-1)', 'Target 1 (Relays 2-3)', 'Target 2 (Relays 4-5)'];";
    html += "  const blockDiv = document.createElement('div');";
    html += "  blockDiv.className = 'block-item';";
    html += "  blockDiv.setAttribute('data-block', blockCounter);";
    html += "  blockDiv.innerHTML = `";
    html += "    <h4>Block ${blockCounter + 1}: ${targetNames[pairId]}</h4>";
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
    html += "<h3>How Target Programs Work:</h3>";
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
    
    html += "<h3>Available Motor Targets</h3>";
    html += "<p>Drag a motor target below to add it to your program:</p>";
    html += "<div style='margin: 20px 0;'>";
    html += "<div class='target-item' draggable='true' ondragstart='drag(event)' data-pair='0'>";
    html += "<h4>Target 0</h4>";
    html += "<div class='relays'>Relays 0-1</div>";
    html += "</div>";
    html += "<div class='target-item' draggable='true' ondragstart='drag(event)' data-pair='1'>";
    html += "<h4>Target 1</h4>";
    html += "<div class='relays'>Relays 2-3</div>";
    html += "</div>";
    html += "<div class='target-item' draggable='true' ondragstart='drag(event)' data-pair='2'>";
    html += "<h4>Target 2</h4>";
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
        html += "<h2>Target Program '" + currentPairProgram.name + "' started successfully</h2>";
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
      
      // Stop all current operations first
      stopPairProgram();
      stopCompetitionMode();
      for (int i = 0; i < 6; i++) {
        relays.digitalWrite(i, HIGH);
        motorStates[i].waitingForInput = false;
        motorStates[i].isActive = false;
        inputTimeoutActive[i] = false;
      }
      systemState = SYSTEM_STOPPED;
      
      if (mode == "sequence") {
        currentMode = MODE_SEQUENCE;
      } else if (mode == "custom") {
        currentMode = MODE_CUSTOM;
      } else if (mode == "competition") {
        currentMode = MODE_ZAWODY;
      } else if (mode == "manual") {
        currentMode = MODE_MANUAL;
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

  // Competition mode endpoints
  server.on("/start-competition", HTTP_GET, [](AsyncWebServerRequest *request) {
    startCompetitionMode();
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='3;url=/' />";
    html += "<title>Competition Started</title></head><body>";
    html += "<h2>Competition mode initialized</h2>";
    html += "<p>Targets moving to hidden position. Waiting for mic trigger...</p>";
    html += "<p>Redirecting back to home page...</p>";
    html += "</body></html>";
    
    request->send(200, "text/html", html);
  });

  server.on("/stop-competition", HTTP_GET, [](AsyncWebServerRequest *request) {
    stopCompetitionMode();
    currentMode = MODE_SEQUENCE; // Return to sequence mode
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='2;url=/' />";
    html += "<title>Competition Stopped</title></head><body>";
    html += "<h2>Competition mode stopped</h2>";
    html += "<p>Redirecting back to home page...</p>";
    html += "</body></html>";
    
    request->send(200, "text/html", html);
  });

  server.on("/set-competition-settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    String message = "";
    bool hasError = false;

    if (request->hasParam("micThreshold") && request->hasParam("t1Show") && 
        request->hasParam("t1Hide") && request->hasParam("t2Show") && 
        request->hasParam("t2Hide") && request->hasParam("t3Show") && 
        request->hasParam("t3Hide") && request->hasParam("repetitions") &&
        request->hasParam("t1Delay") && request->hasParam("t2Delay") && 
        request->hasParam("t3Delay")) {
      
      float micThreshold = request->getParam("micThreshold")->value().toFloat();
      int t1Show = request->getParam("t1Show")->value().toInt();
      int t1Hide = request->getParam("t1Hide")->value().toInt();
      int t2Show = request->getParam("t2Show")->value().toInt();
      int t2Hide = request->getParam("t2Hide")->value().toInt();
      int t3Show = request->getParam("t3Show")->value().toInt();
      int t3Hide = request->getParam("t3Hide")->value().toInt();
      int repetitions = request->getParam("repetitions")->value().toInt();
      int t1Delay = request->getParam("t1Delay")->value().toInt();
      int t2Delay = request->getParam("t2Delay")->value().toInt();
      int t3Delay = request->getParam("t3Delay")->value().toInt();

      // Validate parameters
      if (micThreshold < 20.0 || micThreshold > 120.0) {
        message = "Error: Mic threshold must be between 20-120 dBA";
        hasError = true;
      } else if (t1Show < 500 || t1Show > 30000 || t1Hide < 500 || t1Hide > 30000 ||
                 t2Show < 500 || t2Show > 30000 || t2Hide < 500 || t2Hide > 30000 ||
                 t3Show < 500 || t3Show > 30000 || t3Hide < 500 || t3Hide > 30000) {
        message = "Error: Show/hide times must be between 500-30000ms";
        hasError = true;
      } else if (repetitions < 1 || repetitions > 20) {
        message = "Error: Repetitions must be between 1-20";
        hasError = true;
      } else if (t1Delay < 0 || t1Delay > 30000 || t2Delay < 0 || t2Delay > 30000 || 
                 t3Delay < 0 || t3Delay > 30000) {
        message = "Error: Start delays must be between 0-30000ms";
        hasError = true;
      } else {
        // Update settings
        competitionSettings.micTriggerThreshold = micThreshold;
        competitionSettings.target1ShowTime = t1Show;
        competitionSettings.target1HideTime = t1Hide;
        competitionSettings.target2ShowTime = t2Show;
        competitionSettings.target2HideTime = t2Hide;
        competitionSettings.target3ShowTime = t3Show;
        competitionSettings.target3HideTime = t3Hide;
        competitionSettings.repetitions = repetitions;
        competitionSettings.target1DelayStart = t1Delay;
        competitionSettings.target2DelayStart = t2Delay;
        competitionSettings.target3DelayStart = t3Delay;
        
        saveCompetitionSettings();
        message = "Competition settings updated successfully";
      }
    } else {
      message = "Error: Missing parameters";
      hasError = true;
    }

    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='3;url=/' />";
    html += "<title>Competition Settings</title></head><body>";
    html += "<h2 style='color:" + String(hasError ? "red" : "green") + "'>" + message + "</h2>";
    html += "<p>Redirecting back to home page...</p>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  // Manual mode endpoints
  server.on("/start-manual", HTTP_GET, [](AsyncWebServerRequest *request) {
    currentMode = MODE_MANUAL;
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='2;url=/' />";
    html += "<title>Manual Mode</title></head><body>";
    html += "<h2>Manual mode activated</h2>";
    html += "<p>You can now control targets manually</p>";
    html += "<p>Redirecting back to home page...</p>";
    html += "</body></html>";
    
    request->send(200, "text/html", html);
  });

  server.on("/manual-show", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("target")) {
      int target = request->getParam("target")->value().toInt();
      if (target >= 1 && target <= 3) {
        manualShowTarget(target - 1); // Convert to 0-based index
        request->send(200, "text/plain", "Target " + String(target) + " showing");
      } else {
        request->send(400, "text/plain", "Invalid target number");
      }
    } else {
      request->send(400, "text/plain", "Missing target parameter");
    }
  });

  server.on("/manual-hide", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("target")) {
      int target = request->getParam("target")->value().toInt();
      if (target >= 1 && target <= 3) {
        manualHideTarget(target - 1); // Convert to 0-based index
        request->send(200, "text/plain", "Target " + String(target) + " hiding");
      } else {
        request->send(400, "text/plain", "Invalid target number");
      }
    } else {
      request->send(400, "text/plain", "Missing target parameter");
    }
  });

  server.on("/manual-stop", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("target")) {
      int target = request->getParam("target")->value().toInt();
      if (target >= 1 && target <= 3) {
        manualStopTarget(target - 1); // Convert to 0-based index
        request->send(200, "text/plain", "Target " + String(target) + " stopped");
      } else {
        request->send(400, "text/plain", "Invalid target number");
      }
    } else {
      request->send(400, "text/plain", "Missing target parameter");
    }
  });

  server.on("/target-status", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", getTargetStatus());
  });

  // Competition settings page
  server.on("/competition-settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>Competition Settings</title>";
    html += "<style>";
    html += "* { box-sizing: border-box; }";
    html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Arial, sans-serif; ";
    html += "margin: 0; padding: 15px; background-color: #f5f5f5; font-size: 16px; line-height: 1.4; }";
    html += ".container { max-width: 600px; margin: 0 auto; background-color: white; ";
    html += "border-radius: 10px; padding: 20px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }";
    html += "h1 { color: #333; margin: 0 0 20px 0; text-align: center; font-size: clamp(1.5rem, 4vw, 2.2rem); }";
    html += "h3 { color: #555; margin: 25px 0 15px 0; border-bottom: 2px solid #e0e0e0; padding-bottom: 5px; }";
    html += ".form-group { margin: 20px 0; }";
    html += "label { display: block; margin-bottom: 8px; font-weight: bold; color: #333; }";
    html += "input { width: 100%; padding: 12px; border: 1px solid #ccc; border-radius: 6px; ";
    html += "font-size: 16px; transition: border-color 0.3s ease; }";
    html += "input:focus { outline: none; border-color: #4CAF50; box-shadow: 0 0 5px rgba(76, 175, 80, 0.3); }";
    html += ".btn { background-color: #4CAF50; color: white; padding: 15px 30px; ";
    html += "border: none; border-radius: 6px; cursor: pointer; font-size: 16px; ";
    html += "text-decoration: none; display: inline-block; margin: 10px 8px; ";
    html += "transition: all 0.3s ease; touch-action: manipulation; }";
    html += "@media (max-width: 768px) { .btn { display: block; width: 100%; margin: 10px 0; text-align: center; } }";
    html += ".btn:hover, .btn:focus { background-color: #45a049; transform: translateY(-1px); outline: none; }";
    html += ".btn:active { transform: translateY(0); }";
    html += ".btn-secondary { background-color: #757575; }";
    html += ".btn-secondary:hover, .btn-secondary:focus { background-color: #616161; }";
    html += ".settings-grid { display: grid; gap: 20px; }";
    html += "@media (min-width: 769px) { .settings-grid { grid-template-columns: 1fr 1fr; } }";
    html += ".settings-section { background-color: #fafafa; padding: 15px; border-radius: 6px; border: 1px solid #e0e0e0; }";
    html += ".button-group { display: flex; gap: 10px; margin-top: 20px; }";
    html += "@media (max-width: 768px) { .button-group { flex-direction: column; gap: 0; } }";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>Competition Settings</h1>";
    html += "<form action='/set-competition-settings' method='get'>";
    html += "<div class='form-group'>";
    html += "<label for='micThreshold'>Microphone Trigger Threshold (dBA):</label>";
    html += "<input type='number' id='micThreshold' name='micThreshold' min='20' max='120' step='0.1' value='" + String(competitionSettings.micTriggerThreshold, 1) + "'>";
    html += "</div>";
    
    html += "<div class='form-group'>";
    html += "<label for='repetitions'>Number of Repetitions:</label>";
    html += "<input type='number' id='repetitions' name='repetitions' min='1' max='20' value='" + String(competitionSettings.repetitions) + "'>";
    html += "</div>";
    
    html += "<h3>Target Timing Settings</h3>";
    html += "<div class='settings-grid'>";
    
    // Target 1 settings
    html += "<div class='settings-section'>";
    html += "<h4 style='margin-top: 0; color: #4CAF50;'>Target 1</h4>";
    html += "<div class='form-group'>";
    html += "<label for='t1Show'>Show Time (ms):</label>";
    html += "<input type='number' id='t1Show' name='t1Show' min='500' max='30000' value='" + String(competitionSettings.target1ShowTime) + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='t1Hide'>Hide Time (ms):</label>";
    html += "<input type='number' id='t1Hide' name='t1Hide' min='500' max='30000' value='" + String(competitionSettings.target1HideTime) + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='t1Delay'>Start Delay (ms):</label>";
    html += "<input type='number' id='t1Delay' name='t1Delay' min='0' max='30000' value='" + String(competitionSettings.target1DelayStart) + "'>";
    html += "</div>";
    html += "</div>";
    
    // Target 2 settings
    html += "<div class='settings-section'>";
    html += "<h4 style='margin-top: 0; color: #2196F3;'>Target 2</h4>";
    html += "<div class='form-group'>";
    html += "<label for='t2Show'>Show Time (ms):</label>";
    html += "<input type='number' id='t2Show' name='t2Show' min='500' max='30000' value='" + String(competitionSettings.target2ShowTime) + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='t2Hide'>Hide Time (ms):</label>";
    html += "<input type='number' id='t2Hide' name='t2Hide' min='500' max='30000' value='" + String(competitionSettings.target2HideTime) + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='t2Delay'>Start Delay (ms):</label>";
    html += "<input type='number' id='t2Delay' name='t2Delay' min='0' max='30000' value='" + String(competitionSettings.target2DelayStart) + "'>";
    html += "</div>";
    html += "</div>";
    
    // Target 3 settings
    html += "<div class='settings-section'>";
    html += "<h4 style='margin-top: 0; color: #FF9800;'>Target 3</h4>";
    html += "<div class='form-group'>";
    html += "<label for='t3Show'>Show Time (ms):</label>";
    html += "<input type='number' id='t3Show' name='t3Show' min='500' max='30000' value='" + String(competitionSettings.target3ShowTime) + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='t3Hide'>Hide Time (ms):</label>";
    html += "<input type='number' id='t3Hide' name='t3Hide' min='500' max='30000' value='" + String(competitionSettings.target3HideTime) + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='t3Delay'>Start Delay (ms):</label>";
    html += "<input type='number' id='t3Delay' name='t3Delay' min='0' max='30000' value='" + String(competitionSettings.target3DelayStart) + "'>";
    html += "</div>";
    html += "</div>";
    
    html += "</div>"; // End settings-grid
    
    html += "<div class='button-group'>";
    html += "<button type='submit' class='btn'>Save Settings</button>";
    html += "<a href='/' class='btn btn-secondary'>Back to Home</a>";
    html += "</div>";
    html += "</form></div></body></html>";
    
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
  // Check microphone only when waiting for competition mic trigger or custom program mic trigger
  if ((currentMode == MODE_ZAWODY && competitionState.waitingForMicTrigger) || 
      (waitingForCustomMicTrigger && currentMode == MODE_CUSTOM)) {
    static unsigned long lastMicCheck = 0;
    static bool debugPrinted = false;
    
    // Debug info once when condition is met
    if (!debugPrinted && currentMode == MODE_ZAWODY) {
      Serial.println("DEBUG: Mic checking active for competition - systemState=" + String(systemState) + ", waitingForMicTrigger=" + String(competitionState.waitingForMicTrigger));
      debugPrinted = true;
    }
    
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
      // Check if we're waiting for mic trigger for competition mode
      else if (competitionState.waitingForMicTrigger && currentMode == MODE_ZAWODY) {
        Serial.println("DEBUG: Checking competition mic trigger - dB=" + String(dbValue, 1) + ", threshold=" + String(competitionSettings.micTriggerThreshold, 1));
        if (dbValue >= competitionSettings.micTriggerThreshold) {
          Serial.println("Competition mic trigger activated! dB: " + String(dbValue, 1) + " >= " + String(competitionSettings.micTriggerThreshold, 1) + " (competition threshold)");
          Serial.println("Starting competition...");
          competitionState.waitingForMicTrigger = false;
          competitionState.isRunning = true;
          competitionState.timerStart = millis();
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
  } else if (currentMode == MODE_ZAWODY) {
    // Competition mode logic
    // Check for mic trigger if waiting
    if (competitionState.waitingForMicTrigger) {
      // Microphone checking is handled in the main mic section above
      // When mic trigger is detected, it will set waitingForMicTrigger to false
      // and start the competition
    }
    
    if (competitionState.isRunning) {
      executeCompetitionMode();
    }
  } else if (currentMode == MODE_MANUAL) {
    // Manual mode logic
    executeManualMode();
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

// ========== COMPETITION MODE FUNCTIONS ==========

// Target position detection functions
bool isTargetHidden(int targetId) {
  // Check limit switches 1,3,5 for hidden position
  int limitSwitch = targetId * 2 + 1; // 1,3,5
  bool result = safeInputRead(limitSwitch) == LOW;
  // Debug logging for target 1 (targetId 0) issues
  if (targetId == 0) {
    Serial.println("DEBUG: Target 1 hidden check - Input " + String(limitSwitch) + " = " + String(safeInputRead(limitSwitch)) + " (hidden=" + String(result) + ")");
  }
  return result;
}

bool isTargetShown(int targetId) {
  // Check limit switches 0,2,4 for shown position
  int limitSwitch = targetId * 2; // 0,2,4
  bool result = safeInputRead(limitSwitch) == LOW;
  // Debug logging for target 1 (targetId 0) issues
  if (targetId == 0) {
    Serial.println("DEBUG: Target 1 shown check - Input " + String(limitSwitch) + " = " + String(safeInputRead(limitSwitch)) + " (shown=" + String(result) + ")");
  }
  return result;
}

// Initialize all targets to hidden position
bool initializeTargetsToHidden() {
  Serial.println("Initializing targets to hidden position...");
  
  // First, check current positions
  for (int target = 0; target < 3; target++) {
    Serial.println("Target " + String(target + 1) + " initial position - Hidden: " + String(isTargetHidden(target)) + ", Shown: " + String(isTargetShown(target)));
  }
  
  // Turn on relays to move targets left (to hidden position)
  for (int target = 0; target < 3; target++) {
    int relay = target * 2 + 1; // Relays 1,3,5 for left movement
    if (!isTargetHidden(target)) {
      Serial.println("Target " + String(target + 1) + " needs to move to hidden position - activating relay " + String(relay));
      safeRelayWrite(relay, LOW);
      startInputTimeout(relay);
    } else {
      Serial.println("Target " + String(target + 1) + " already in hidden position");
    }
  }
  
  // Wait for all targets to reach hidden position
  unsigned long initStart = millis();
  while (millis() - initStart < 30000) { // 30 second timeout
    bool allHidden = true;
    for (int target = 0; target < 3; target++) {
      if (!isTargetHidden(target)) {
        allHidden = false;
        break;
      } else {
        // Target reached hidden position, stop its motor
        int relay = target * 2 + 1;
        if (relays.digitalRead(relay) == LOW) {
          safeRelayWrite(relay, HIGH);
          stopInputTimeout(relay);
          Serial.println("Target " + String(target + 1) + " reached hidden position - stopped relay " + String(relay));
        }
      }
    }
    
    if (allHidden) {
      Serial.println("All targets initialized to hidden position successfully");
      return true;
    }
    
    delay(50);
  }
  
  // Timeout - emergency stop
  for (int i = 0; i < 6; i++) {
    relays.digitalWrite(i, HIGH);
    inputTimeoutActive[i] = false;
  }
  
  Serial.println("ERROR: Failed to initialize targets to hidden position within 30 seconds");
  return false;
}

void startCompetitionMode() {
  if (!initializeTargetsToHidden()) {
    systemState = SYSTEM_ERROR;
    lastErrorMessage = "Failed to initialize targets for competition mode";
    return;
  }
  
  currentMode = MODE_ZAWODY;
  competitionState.isRunning = false;
  competitionState.waitingForMicTrigger = true;
  competitionState.initialized = true;
  competitionState.currentCycle = 0;
  competitionState.target1Count = 0;
  competitionState.target2Count = 0;
  competitionState.target3Count = 0;
  competitionState.target1Active = false;
  competitionState.target2Active = false;
  competitionState.target3Active = false;
  
  // Ensure system is in stopped state to allow mic monitoring
  systemState = SYSTEM_STOPPED;
  
  Serial.println("=== COMPETITION MODE INITIALIZATION ===");
  Serial.println("Competition mode ready - waiting for mic trigger (threshold: " + String(competitionSettings.micTriggerThreshold, 1) + " dBA)");
  Serial.println("Competition state: isRunning=" + String(competitionState.isRunning) + ", waitingForMicTrigger=" + String(competitionState.waitingForMicTrigger));
  Serial.println("System state: " + String(systemState));
  Serial.println("Current mode: " + String(currentMode));
  Serial.println("===========================================");
}

void executeCompetitionMode() {
  if (!competitionState.isRunning) return;
  
  unsigned long currentTime = millis();
  unsigned long elapsedTime = currentTime - competitionState.timerStart;
  
  // Handle Target 1
  handleCompetitionTarget(0, elapsedTime, competitionSettings.target1DelayStart,
                         competitionSettings.target1ShowTime, competitionSettings.target1HideTime, 
                         competitionState.target1Count, competitionState.target1Active, 
                         competitionState.target1StateTime);
  
  // Handle Target 2  
  handleCompetitionTarget(1, elapsedTime, competitionSettings.target2DelayStart,
                         competitionSettings.target2ShowTime, competitionSettings.target2HideTime, 
                         competitionState.target2Count, competitionState.target2Active, 
                         competitionState.target2StateTime);
  
  // Handle Target 3
  handleCompetitionTarget(2, elapsedTime, competitionSettings.target3DelayStart,
                         competitionSettings.target3ShowTime, competitionSettings.target3HideTime, 
                         competitionState.target3Count, competitionState.target3Active, 
                         competitionState.target3StateTime);
  
  // Check if competition is complete
  if (competitionState.target1Count >= competitionSettings.repetitions &&
      competitionState.target2Count >= competitionSettings.repetitions &&
      competitionState.target3Count >= competitionSettings.repetitions) {
    
    // End competition
    stopCompetitionMode();
    Serial.println("Competition completed - all targets shown required number of times");
  }
}

void handleCompetitionTarget(int targetId, unsigned long elapsedTime, unsigned long delayStart,
                           int showTime, int hideTime, int& targetCount,
                           bool& targetActive, unsigned long& stateTime) {
  
  // Check if target has completed all repetitions
  if (targetCount >= competitionSettings.repetitions) {
    // Target completed - ensure it's properly stopped
    int showRelay = targetId * 2;     // 0,2,4 for showing
    int hideRelay = targetId * 2 + 1; // 1,3,5 for hiding
    
    // Stop any active relays for this completed target
    if (relays.digitalRead(showRelay) == LOW) {
      safeRelayWrite(showRelay, HIGH);
      stopInputTimeout(showRelay);
      Serial.println("Competition: Stopped completed target " + String(targetId + 1) + " show relay " + String(showRelay));
    }
    if (relays.digitalRead(hideRelay) == LOW) {
      safeRelayWrite(hideRelay, HIGH);
      stopInputTimeout(hideRelay);
      Serial.println("Competition: Stopped completed target " + String(targetId + 1) + " hide relay " + String(hideRelay));
    }
    
    // Mark target as inactive since it's completed
    targetActive = false;
    return; // Target completed - no further processing needed
  }
  
  // Check if delay period has passed
  if (elapsedTime < delayStart) {
    // Still in delay period - ensure target is hidden and inactive
    if (targetActive) {
      Serial.printf("Target %d in delay period (%lu/%lu ms) - ensuring hidden\n", 
                   targetId + 1, elapsedTime, delayStart);
      targetActive = false;
    }
    return;
  }
  
  // Calculate time since delay period ended
  unsigned long adjustedTime = elapsedTime - delayStart;
  unsigned long cycleTime = showTime + hideTime;
  unsigned long positionInCycle = adjustedTime % cycleTime;
  
  // Determine if target should be showing or hiding based on timing
  bool shouldShow = (positionInCycle < showTime);
  
  // Check if target is currently moving
  int showRelay = targetId * 2;     // 0,2,4 for showing
  int hideRelay = targetId * 2 + 1; // 1,3,5 for hiding
  bool isMoving = (relays.digitalRead(showRelay) == LOW || relays.digitalRead(hideRelay) == LOW);
  
  if (shouldShow && !targetActive && !isMoving) {
    // Start showing target only if it's not already moving
    if (!isTargetShown(targetId)) {
      showTarget(targetId);
      targetActive = true;
      stateTime = millis();
      targetCount++;
      Serial.println("Competition: Showing target " + String(targetId + 1) + 
                     " (count: " + String(targetCount) + "/" + String(competitionSettings.repetitions) + 
                     ", delay " + String(delayStart) + "ms passed)");
    } else {
      // Target is already shown, just mark as active
      targetActive = true;
      stateTime = millis();
      targetCount++;
      Serial.println("Competition: Target " + String(targetId + 1) + " already shown " + 
                     " (count: " + String(targetCount) + "/" + String(competitionSettings.repetitions) + ")");
    }
  } else if (!shouldShow && targetActive && !isMoving) {
    // Start hiding target only if it's not already moving and hasn't completed all reps
    if (targetCount < competitionSettings.repetitions) {
      if (!isTargetHidden(targetId)) {
        hideTarget(targetId);
        targetActive = false;
        stateTime = millis();
        Serial.println("Competition: Hiding target " + String(targetId + 1));
      } else {
        // Target is already hidden, just mark as inactive
        targetActive = false;
        stateTime = millis();
        Serial.println("Competition: Target " + String(targetId + 1) + " already hidden");
      }
    }
  }
  
  // Auto-stop motors when targets reach their positions
  if (isMoving) {
    if (relays.digitalRead(showRelay) == LOW && isTargetShown(targetId)) {
      // Target reached shown position, stop show motor
      safeRelayWrite(showRelay, HIGH);
      stopInputTimeout(showRelay);
      Serial.println("Competition: Target " + String(targetId + 1) + " reached shown position");
    }
    
    if (relays.digitalRead(hideRelay) == LOW && isTargetHidden(targetId)) {
      // Target reached hidden position, stop hide motor
      safeRelayWrite(hideRelay, HIGH);
      stopInputTimeout(hideRelay);
      Serial.println("Competition: Target " + String(targetId + 1) + " reached hidden position");
    }
  }
}

void showTarget(int targetId) {
  int showRelay = targetId * 2; // Relays 0,2,4 for right movement (show)
  int hideRelay = targetId * 2 + 1; // Relays 1,3,5 for left movement (hide)
  
  // Debug: Check current target position
  bool currentlyShown = isTargetShown(targetId);
  bool currentlyHidden = isTargetHidden(targetId);
  Serial.println("DEBUG: Target " + String(targetId + 1) + " position check - Shown: " + String(currentlyShown) + ", Hidden: " + String(currentlyHidden));
  
  // Stop any opposite movement first
  if (relays.digitalRead(hideRelay) == LOW) {
    safeRelayWrite(hideRelay, HIGH);
    stopInputTimeout(hideRelay);
    Serial.println("Competition: Stopped hide relay " + String(hideRelay) + " before showing target " + String(targetId + 1));
  }
  
  // Start showing if not already shown and not already moving to show
  if (!currentlyShown && relays.digitalRead(showRelay) == HIGH) {
    safeRelayWrite(showRelay, LOW);
    startInputTimeout(showRelay);
    Serial.println("Competition: Started showing target " + String(targetId + 1) + " (relay " + String(showRelay) + ")");
  } else if (currentlyShown) {
    Serial.println("Competition: Target " + String(targetId + 1) + " already in shown position - no movement needed");
  } else if (relays.digitalRead(showRelay) == LOW) {
    Serial.println("Competition: Target " + String(targetId + 1) + " show relay already active");
  }
}

void hideTarget(int targetId) {
  int showRelay = targetId * 2; // Relays 0,2,4 for right movement (show)  
  int hideRelay = targetId * 2 + 1; // Relays 1,3,5 for left movement (hide)
  
  // Debug: Check current target position
  bool currentlyShown = isTargetShown(targetId);
  bool currentlyHidden = isTargetHidden(targetId);
  Serial.println("DEBUG: Target " + String(targetId + 1) + " position check - Shown: " + String(currentlyShown) + ", Hidden: " + String(currentlyHidden));
  
  // Stop any opposite movement first
  if (relays.digitalRead(showRelay) == LOW) {
    safeRelayWrite(showRelay, HIGH);
    stopInputTimeout(showRelay);
    Serial.println("Competition: Stopped show relay " + String(showRelay) + " before hiding target " + String(targetId + 1));
  }
  
  // Start hiding if not already hidden and not already moving to hide
  if (!currentlyHidden && relays.digitalRead(hideRelay) == HIGH) {
    safeRelayWrite(hideRelay, LOW);
    startInputTimeout(hideRelay);
    Serial.println("Competition: Started hiding target " + String(targetId + 1) + " (relay " + String(hideRelay) + ")");
  } else if (currentlyHidden) {
    Serial.println("Competition: Target " + String(targetId + 1) + " already in hidden position - no movement needed");
  } else if (relays.digitalRead(hideRelay) == LOW) {
    Serial.println("Competition: Target " + String(targetId + 1) + " hide relay already active");
  }
}

void stopCompetitionMode() {
  // Stop all motors
  for (int i = 0; i < 6; i++) {
    safeRelayWrite(i, HIGH);
    stopInputTimeout(i);
  }
  
  competitionState.isRunning = false;
  competitionState.waitingForMicTrigger = false;
  competitionState.initialized = false;
  
  Serial.println("Competition mode stopped");
}

void saveCompetitionSettings() {
  preferences.begin("competition", false);
  preferences.putFloat("micThreshold", competitionSettings.micTriggerThreshold);
  preferences.putInt("t1ShowTime", competitionSettings.target1ShowTime);
  preferences.putInt("t1HideTime", competitionSettings.target1HideTime);
  preferences.putInt("t2ShowTime", competitionSettings.target2ShowTime);
  preferences.putInt("t2HideTime", competitionSettings.target2HideTime);
  preferences.putInt("t3ShowTime", competitionSettings.target3ShowTime);
  preferences.putInt("t3HideTime", competitionSettings.target3HideTime);
  preferences.putInt("repetitions", competitionSettings.repetitions);
  preferences.putInt("t1DelayStart", competitionSettings.target1DelayStart);
  preferences.putInt("t2DelayStart", competitionSettings.target2DelayStart);
  preferences.putInt("t3DelayStart", competitionSettings.target3DelayStart);
  preferences.end();
  Serial.println("Competition settings saved to flash");
}

void loadCompetitionSettings() {
  preferences.begin("competition", true);
  competitionSettings.micTriggerThreshold = preferences.getFloat("micThreshold", 50.0);
  competitionSettings.target1ShowTime = preferences.getInt("t1ShowTime", 2000);
  competitionSettings.target1HideTime = preferences.getInt("t1HideTime", 2000);
  competitionSettings.target2ShowTime = preferences.getInt("t2ShowTime", 2000);
  competitionSettings.target2HideTime = preferences.getInt("t2HideTime", 2000);
  competitionSettings.target3ShowTime = preferences.getInt("t3ShowTime", 2000);
  competitionSettings.target3HideTime = preferences.getInt("t3HideTime", 2000);
  competitionSettings.repetitions = preferences.getInt("repetitions", 3);
  competitionSettings.target1DelayStart = preferences.getInt("t1DelayStart", 0);
  competitionSettings.target2DelayStart = preferences.getInt("t2DelayStart", 1000);
  competitionSettings.target3DelayStart = preferences.getInt("t3DelayStart", 2000);
  preferences.end();
  Serial.println("Competition settings loaded from flash");
}

// ========== MANUAL MODE FUNCTIONS ==========

void executeManualMode() {
  // Check limit switches and stop motors when targets reach positions
  for (int target = 0; target < 3; target++) {
    int rightRelay = target * 2;     // 0,2,4
    int leftRelay = target * 2 + 1;  // 1,3,5
    
    // Stop right movement when target is shown
    if (relays.digitalRead(rightRelay) == LOW && isTargetShown(target)) {
      safeRelayWrite(rightRelay, HIGH);
      stopInputTimeout(rightRelay);
      Serial.println("Manual: Target " + String(target + 1) + " reached shown position");
    }
    
    // Stop left movement when target is hidden
    if (relays.digitalRead(leftRelay) == LOW && isTargetHidden(target)) {
      safeRelayWrite(leftRelay, HIGH);
      stopInputTimeout(leftRelay);
      Serial.println("Manual: Target " + String(target + 1) + " reached hidden position");
    }
  }
}

// Manual control functions for web interface
void manualShowTarget(int targetId) {
  if (targetId < 0 || targetId > 2) return;
  
  int rightRelay = targetId * 2;
  int leftRelay = targetId * 2 + 1;
  
  // Stop any opposite movement
  if (relays.digitalRead(leftRelay) == LOW) {
    safeRelayWrite(leftRelay, HIGH);
    stopInputTimeout(leftRelay);
  }
  
  // Start showing if not already shown
  if (!isTargetShown(targetId)) {
    safeRelayWrite(rightRelay, LOW);
    startInputTimeout(rightRelay);
  }
}

void manualHideTarget(int targetId) {
  if (targetId < 0 || targetId > 2) return;
  
  int rightRelay = targetId * 2;
  int leftRelay = targetId * 2 + 1;
  
  // Stop any opposite movement
  if (relays.digitalRead(rightRelay) == LOW) {
    safeRelayWrite(rightRelay, HIGH);
    stopInputTimeout(rightRelay);
  }
  
  // Start hiding if not already hidden
  if (!isTargetHidden(targetId)) {
    safeRelayWrite(leftRelay, LOW);
    startInputTimeout(leftRelay);
  }
}

void manualStopTarget(int targetId) {
  if (targetId < 0 || targetId > 2) return;
  
  int rightRelay = targetId * 2;
  int leftRelay = targetId * 2 + 1;
  
  safeRelayWrite(rightRelay, HIGH);
  safeRelayWrite(leftRelay, HIGH);
  stopInputTimeout(rightRelay);
  stopInputTimeout(leftRelay);
}

String getTargetStatus() {
  String json = "{";
  json += "\"target1\":{";
  json += "\"hidden\":" + String(isTargetHidden(0) ? "true" : "false") + ",";
  json += "\"shown\":" + String(isTargetShown(0) ? "true" : "false") + ",";
  json += "\"moving\":" + String((relays.digitalRead(0) == LOW || relays.digitalRead(1) == LOW) ? "true" : "false");
  json += "},";
  json += "\"target2\":{";
  json += "\"hidden\":" + String(isTargetHidden(1) ? "true" : "false") + ",";
  json += "\"shown\":" + String(isTargetShown(1) ? "true" : "false") + ",";
  json += "\"moving\":" + String((relays.digitalRead(2) == LOW || relays.digitalRead(3) == LOW) ? "true" : "false");
  json += "},";
  json += "\"target3\":{";
  json += "\"hidden\":" + String(isTargetHidden(2) ? "true" : "false") + ",";
  json += "\"shown\":" + String(isTargetShown(2) ? "true" : "false") + ",";
  json += "\"moving\":" + String((relays.digitalRead(4) == LOW || relays.digitalRead(5) == LOW) ? "true" : "false");
  json += "}";
  json += "}";
  return json;
}