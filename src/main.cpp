#include "Arduino.h"
#include "PCF8574.h"
// #include <AsyncTCP.h>
#include <WiFi.h>
#include <WiFiAP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>

// Motor control state structure for better organization
struct MotorState {
  unsigned long delayStartTime = 0;
  bool waitingForInput = false;
  bool isActive = false;
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

// Custom program structures
struct ProgramStep {
  int relayNumber;
  int customDelay;
  bool waitForInput;
};

struct CustomProgram {
  String name;
  bool loopEnabled;
  int stepCount;
  ProgramStep steps[50]; // Maximum 50 steps per program
};

// Current custom program execution state
CustomProgram currentProgram;
int currentProgramStep = 0;
bool programRunning = false;
unsigned long programStepStartTime = 0;
bool waitingForProgramInput = false;

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

// --- Web Server ---
AsyncWebServer server(80); // Create a web server on port 80

// --- Function Declarations ---
int getRandomDelay(int relay);
void saveRelayDelays();
void loadRelayDelays();

// Custom program functions
bool saveProgram(const String& programName, const String& programData);
String loadProgram(const String& programName);
String getProgramList();
bool deleteProgram(const String& programName);
bool validateProgramSafety(const String& programData);
void executeCustomProgram();
void stopCustomProgram();
bool checkRelayPairSafety(int relay1, int relay2);

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
      // Check if 1 second has passed
      if (currentTime - inputTimeoutStart[i] >= 1000) {
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
        lastErrorMessage = "Relay " + String(i) + " did not reach input " + String(i) + " before one second";
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

  // Update the root route to include delay configuration form
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='5' />"; // Auto refresh every 5 seconds
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
    if (currentMode == MODE_CUSTOM && programRunning) {
      html += "<p><strong>Current Program:</strong> " + currentProgram.name + "</p>";
      html += "<p><strong>Program Step:</strong> " + String(currentProgramStep + 1) + " / " + String(currentProgram.stepCount) + "</p>";
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
      if (programRunning) {
        html += "<a href='/stop-program' class='btn btn-stop'>Stop Program</a>";
      } else {
        html += "<p>Select a program to execute:</p>";
        // Add program selection here (this will be populated by JavaScript)
        html += "<div id='program-selector' style='margin: 10px 0;'></div>";
        html += "<script>";
        html += "fetch('/program-list').then(r=>r.json()).then(d=>{";
        html += "let html='<select id=\"program-select\" style=\"padding:5px;margin:5px;\"><option value=\"\">Select Program...</option>';";
        html += "d.programs.forEach(p=>html+=`<option value=\"${p}\">${p}</option>`);";
        html += "html+='</select><button onclick=\"executeSelectedProgram()\" class=\"btn\">Execute Program</button>';";
        html += "document.getElementById('program-selector').innerHTML=html;";
        html += "});";
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
        max(0L, (long)getRandomDelay(i) - (long)(millis() - motorStates[i].delayStartTime)) : 0);
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
    json += "]}";
    
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

  // Custom Program Editor Route
  server.on("/program-editor", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>TARCZOWNIX - Custom Program Editor</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }";
    html += ".container { max-width: 1200px; margin: 0 auto; background-color: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }";
    html += ".editor-section { border: 2px dashed #ccc; padding: 20px; margin: 20px 0; min-height: 200px; border-radius: 8px; }";
    html += ".relay-item { background-color: #e3f2fd; border: 1px solid #1976d2; padding: 10px; margin: 5px; border-radius: 5px; cursor: grab; display: inline-block; min-width: 120px; text-align: center; }";
    html += ".relay-item:active { cursor: grabbing; }";
    html += ".step-item { background-color: #f1f8e9; border: 1px solid #689f38; padding: 15px; margin: 10px 0; border-radius: 8px; position: relative; }";
    html += ".step-controls { margin-top: 10px; }";
    html += ".step-controls input, .step-controls select { margin: 5px; padding: 5px; }";
    html += ".btn { background-color: #4CAF50; border: none; color: white; padding: 10px 20px; text-decoration: none; display: inline-block; border-radius: 5px; cursor: pointer; margin: 5px; }";
    html += ".btn-danger { background-color: #f44336; }";
    html += ".btn-warning { background-color: #ff9800; }";
    html += ".btn-secondary { background-color: #6c757d; }";
    html += ".form-group { margin: 10px 0; }";
    html += ".form-group label { display: inline-block; width: 150px; font-weight: bold; }";
    html += ".safety-warning { background-color: #ffebee; border: 2px solid #f44336; padding: 15px; margin: 20px 0; border-radius: 8px; }";
    html += ".program-list { background-color: #f9f9f9; padding: 15px; border-radius: 8px; margin: 20px 0; }";
    html += ".program-item { background-color: white; padding: 10px; margin: 5px 0; border-radius: 5px; border: 1px solid #ddd; display: flex; justify-content: space-between; align-items: center; }";
    html += "</style>";
    html += "<script>";
    html += "let draggedElement = null;";
    html += "let stepCounter = 0;";
    html += "let currentSteps = [];";
    
    // JavaScript for drag and drop functionality
    html += "function allowDrop(ev) { ev.preventDefault(); }";
    html += "function drag(ev) { draggedElement = ev.target; }";
    html += "function drop(ev) {";
    html += "  ev.preventDefault();";
    html += "  if (draggedElement && draggedElement.classList.contains('relay-item')) {";
    html += "    addStep(draggedElement.getAttribute('data-relay'));";
    html += "  }";
    html += "}";
    
    // Function to add a step
    html += "function addStep(relayNumber) {";
    html += "  const stepDiv = document.createElement('div');";
    html += "  stepDiv.className = 'step-item';";
    html += "  stepDiv.setAttribute('data-step', stepCounter);";
    html += "  stepDiv.innerHTML = `";
    html += "    <h4>Step ${stepCounter + 1}: Relay ${relayNumber}</h4>";
    html += "    <div class='step-controls'>";
    html += "      <label>Custom Delay (ms):</label>";
    html += "      <input type='number' min='100' max='20000' value='1000' onchange='updateStep(${stepCounter})' id='delay-${stepCounter}'>";
    html += "      <label>Wait for Input:</label>";
    html += "      <select onchange='updateStep(${stepCounter})' id='wait-${stepCounter}'>";
    html += "        <option value='true'>Yes</option>";
    html += "        <option value='false'>No</option>";
    html += "      </select>";
    html += "      <button class='btn btn-danger' onclick='removeStep(${stepCounter})'>Remove</button>";
    html += "      <button class='btn btn-warning' onclick='moveStepUp(${stepCounter})'>Up</button>";
    html += "      <button class='btn btn-warning' onclick='moveStepDown(${stepCounter})'>Down</button>";
    html += "    </div>";
    html += "  `;";
    html += "  document.getElementById('program-steps').appendChild(stepDiv);";
    html += "  currentSteps.push({relay: parseInt(relayNumber), delay: 1000, waitForInput: true, stepId: stepCounter});";
    html += "  stepCounter++;";
    html += "  updateStepNumbers();";
    html += "}";
    
    // Other JavaScript functions
    html += "function removeStep(stepId) {";
    html += "  document.querySelector(`[data-step='${stepId}']`).remove();";
    html += "  currentSteps = currentSteps.filter(step => step.stepId !== stepId);";
    html += "  updateStepNumbers();";
    html += "}";
    
    html += "function updateStep(stepId) {";
    html += "  const step = currentSteps.find(s => s.stepId === stepId);";
    html += "  if (step) {";
    html += "    step.delay = parseInt(document.getElementById(`delay-${stepId}`).value);";
    html += "    step.waitForInput = document.getElementById(`wait-${stepId}`).value === 'true';";
    html += "  }";
    html += "}";
    
    html += "function updateStepNumbers() {";
    html += "  const steps = document.querySelectorAll('.step-item');";
    html += "  steps.forEach((step, index) => {";
    html += "    const h4 = step.querySelector('h4');";
    html += "    const relay = currentSteps.find(s => s.stepId == step.getAttribute('data-step'))?.relay || 0;";
    html += "    h4.textContent = `Step ${index + 1}: Relay ${relay}`;";
    html += "  });";
    html += "}";
    
    html += "function saveProgram() {";
    html += "  const programName = document.getElementById('program-name').value;";
    html += "  const loopEnabled = document.getElementById('loop-enabled').checked;";
    html += "  if (!programName) { alert('Please enter a program name'); return; }";
    html += "  if (currentSteps.length === 0) { alert('Please add at least one step'); return; }";
    html += "  if (!validateProgram()) { return; }";
    html += "  const programData = { name: programName, loopEnabled: loopEnabled, steps: currentSteps.map(s => ({relay: s.relay, delay: s.delay, waitForInput: s.waitForInput})) };";
    html += "  fetch('/save-program', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(programData) })";
    html += "    .then(response => response.text())";
    html += "    .then(data => { alert(data); loadProgramList(); });";
    html += "}";
    
    html += "function loadProgram(programName) {";
    html += "  fetch('/load-program?name=' + encodeURIComponent(programName))";
    html += "    .then(response => response.json())";
    html += "    .then(data => {";
    html += "      document.getElementById('program-name').value = data.name;";
    html += "      document.getElementById('loop-enabled').checked = data.loopEnabled;";
    html += "      document.getElementById('program-steps').innerHTML = '';";
    html += "      currentSteps = [];";
    html += "      stepCounter = 0;";
    html += "      data.steps.forEach((step, index) => {";
    html += "        addStepFromData(step.relay, step.delay, step.waitForInput);";
    html += "      });";
    html += "    })";
    html += "    .catch(error => alert('Error loading program: ' + error));";
    html += "}";
    
    html += "function addStepFromData(relayNumber, delay, waitForInput) {";
    html += "  const stepDiv = document.createElement('div');";
    html += "  stepDiv.className = 'step-item';";
    html += "  stepDiv.setAttribute('data-step', stepCounter);";
    html += "  stepDiv.innerHTML = `";
    html += "    <h4>Step ${stepCounter + 1}: Relay ${relayNumber}</h4>";
    html += "    <div class='step-controls'>";
    html += "      <label>Custom Delay (ms):</label>";
    html += "      <input type='number' min='100' max='20000' value='${delay}' onchange='updateStep(${stepCounter})' id='delay-${stepCounter}'>";
    html += "      <label>Wait for Input:</label>";
    html += "      <select onchange='updateStep(${stepCounter})' id='wait-${stepCounter}'>";
    html += "        <option value='true' ${waitForInput ? 'selected' : ''}>Yes</option>";
    html += "        <option value='false' ${!waitForInput ? 'selected' : ''}>No</option>";
    html += "      </select>";
    html += "      <button class='btn btn-danger' onclick='removeStep(${stepCounter})'>Remove</button>";
    html += "      <button class='btn btn-warning' onclick='moveStepUp(${stepCounter})'>Up</button>";
    html += "      <button class='btn btn-warning' onclick='moveStepDown(${stepCounter})'>Down</button>";
    html += "    </div>";
    html += "  `;";
    html += "  document.getElementById('program-steps').appendChild(stepDiv);";
    html += "  currentSteps.push({relay: parseInt(relayNumber), delay: delay, waitForInput: waitForInput, stepId: stepCounter});";
    html += "  stepCounter++;";
    html += "}";
    
    html += "function deleteProgram(programName) {";
    html += "  if (confirm('Are you sure you want to delete program \"' + programName + '\"?')) {";
    html += "    fetch('/delete-program?name=' + encodeURIComponent(programName))";
    html += "      .then(response => response.text())";
    html += "      .then(data => { alert(data); loadProgramList(); });";
    html += "  }";
    html += "}";
    
    html += "function validateProgram() {";
    html += "  if (currentSteps.length === 0) return false;";
    html += "  for (let i = 1; i < currentSteps.length; i++) {";
    html += "    const currentRelay = currentSteps[i].relay;";
    html += "    const prevRelay = currentSteps[i-1].relay;";
    html += "    const currentPair = Math.floor(currentRelay / 2);";
    html += "    const prevPair = Math.floor(prevRelay / 2);";
    html += "    if (currentPair === prevPair && currentRelay !== prevRelay) {";
    html += "      alert('Safety Error: Steps ' + (i) + ' and ' + (i+1) + ' use relays from the same motor pair (' + prevRelay + ' and ' + currentRelay + '). This could damage the motor!');";
    html += "      return false;";
    html += "    }";
    html += "  }";
    html += "  return true;";
    html += "}";

    html += "window.onload = loadProgramList;";
    html += "</script>";
    html += "</head><body>";
    
    html += "<div class='container'>";
    html += "<h1>Custom Program Editor</h1>";
    html += "<a href='/' class='btn btn-secondary'>Back to Main</a>";
    
    // Safety warning
    html += "<div class='safety-warning'>";
    html += "<h3>! Safety Warning</h3>";
    html += "<p>Never activate both relays from the same motor pair simultaneously:</p>";
    html += "<ul><li>Motor Pair 1: Relays 0 & 1</li><li>Motor Pair 2: Relays 2 & 3</li><li>Motor Pair 3: Relays 4 & 5</li></ul>";
    html += "</div>";
    
    // Relay palette
    html += "<h2>Available Relays (Drag to Program Area)</h2>";
    html += "<div class='relay-palette'>";
    for (int i = 0; i < 6; i++) {
      html += "<div class='relay-item' draggable='true' ondragstart='drag(event)' data-relay='" + String(i) + "'>Relay " + String(i) + "</div>";
    }
    html += "</div>";
    
    // Program editor area
    html += "<h2>Program Steps</h2>";
    html += "<div class='editor-section' ondrop='drop(event)' ondragover='allowDrop(event)' id='program-steps'>";
    html += "<p>Drag relays here to create your custom program sequence</p>";
    html += "</div>";
    
    // Program settings
    html += "<div class='form-group'>";
    html += "<label for='program-name'>Program Name:</label>";
    html += "<input type='text' id='program-name' placeholder='Enter program name'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='loop-enabled'>Loop Program:</label>";
    html += "<input type='checkbox' id='loop-enabled'>";
    html += "</div>";
    
    // Save button
    html += "<button class='btn' onclick='saveProgram()'>Save Program</button>";
    
    // Saved programs list
    html += "<div class='program-list'>";
    html += "<h2>Saved Programs</h2>";
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
      if (validateProgramSafety(body)) {
        // Extract program name from JSON manually
        int nameStart = body.indexOf("\"name\":\"") + 8;
        int nameEnd = body.indexOf("\"", nameStart);
        String programName = body.substring(nameStart, nameEnd);
        
        if (saveProgram(programName, body)) {
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
      String programData = loadProgram(programName);
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
    request->send(200, "application/json", getProgramList());
  });

  // Delete program endpoint
  server.on("/delete-program", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("name")) {
      String programName = request->getParam("name")->value();
      if (deleteProgram(programName)) {
        request->send(200, "text/plain", "Program deleted successfully");
      } else {
        request->send(500, "text/plain", "Failed to delete program");
      }
    } else {
      request->send(400, "text/plain", "Program name required");
    }
  });

  // Execute custom program endpoint
  server.on("/execute-program", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("name")) {
      String programName = request->getParam("name")->value();
      String programData = loadProgram(programName);
      if (programData.length() > 0) {
        // Parse JSON data to load program
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, programData);
        
        if (error) {
          request->send(400, "text/plain", "Invalid program format");
          return;
        }
        
        // Load program into currentProgram structure
        currentProgram.name = doc["name"].as<String>();
        currentProgram.loopEnabled = doc["loopEnabled"].as<bool>();
        
        JsonArray steps = doc["steps"];
        currentProgram.stepCount = min((int)steps.size(), 50); // Limit to 50 steps
        
        for (int i = 0; i < currentProgram.stepCount; i++) {
          currentProgram.steps[i].relayNumber = steps[i]["relay"];
          currentProgram.steps[i].customDelay = steps[i]["delay"];
          currentProgram.steps[i].waitForInput = steps[i]["waitForInput"];
        }
        
        // Start program execution
        currentMode = MODE_CUSTOM;
        programRunning = true;
        currentProgramStep = 0;
        programStepStartTime = millis();
        waitingForProgramInput = false;
        
        // Turn off all relays first
        for (int i = 0; i < 6; i++) {
          relays.digitalWrite(i, HIGH);
        }
        
        Serial.println("Starting custom program: " + currentProgram.name);
        Serial.println("Steps: " + String(currentProgram.stepCount));
        
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta http-equiv='refresh' content='3;url=/' />";
        html += "<title>Program Started</title></head><body>";
        html += "<h2>Program '" + currentProgram.name + "' started successfully</h2>";
        html += "<p>Steps: " + String(currentProgram.stepCount) + "</p>";
        html += "<p>Loop enabled: " + String(currentProgram.loopEnabled ? "Yes" : "No") + "</p>";
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
        stopCustomProgram();
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
    stopCustomProgram();
    
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
        delay(10);
      }
      
      if (motorStates[relay1].waitingForInput && 
          millis() - motorStates[relay1].delayStartTime >= getRandomDelay(relay1)) {
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
        delay(10);
      }
      
      if (motorStates[relay2].waitingForInput && 
          millis() - motorStates[relay2].delayStartTime >= getRandomDelay(relay2)) {
        safeRelayWrite(relay1, LOW);
        startInputTimeout(relay1);
        motorStates[relay2].waitingForInput = false;
        systemState = SYSTEM_RUNNING;
        delay(10);
      }
    }
  } else if (currentMode == MODE_CUSTOM && programRunning) {
    // Custom program execution logic
    executeCustomProgram();
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

void executeCustomProgram() {
  if (!programRunning || currentProgram.stepCount == 0) {
    return;
  }
  
  ProgramStep& currentStep = currentProgram.steps[currentProgramStep];
  
  // If waiting for input on current step
  if (waitingForProgramInput) {
    if (currentStep.waitForInput && safeInputRead(currentStep.relayNumber) == LOW) {
      // Input detected, turn off relay and move to next step
      stopInputTimeout(currentStep.relayNumber);
      safeRelayWrite(currentStep.relayNumber, HIGH);
      waitingForProgramInput = false;
      currentProgramStep++;
      programStepStartTime = millis();
      
      // Check if program is complete
      if (currentProgramStep >= currentProgram.stepCount) {
        if (currentProgram.loopEnabled) {
          currentProgramStep = 0; // Loop back to start
          Serial.println("Program loop: restarting from step 0");
        } else {
          stopCustomProgram(); // Program complete
          Serial.println("Custom program completed");
          return;
        }
      }
    }
    return;
  }
  
  // Check if delay has elapsed for current step
  if (millis() - programStepStartTime >= currentStep.customDelay) {
    // Activate relay for current step
    int relayToActivate = currentStep.relayNumber;
    
    // Safety check: ensure no relay from same pair is already on
    int pairRelays[2] = {(relayToActivate / 2) * 2, (relayToActivate / 2) * 2 + 1};
    int otherRelay = (relayToActivate == pairRelays[0]) ? pairRelays[1] : pairRelays[0];
    
    if (relays.digitalRead(otherRelay) == LOW) {
      // Safety violation - stop program
      lastErrorMessage = "Safety violation: Relay " + String(otherRelay) + " already active in pair";
      lastErrorTime = millis();
      stopCustomProgram();
      return;
    }
    
    // Activate relay
    safeRelayWrite(relayToActivate, LOW);
    startInputTimeout(relayToActivate);
    waitingForProgramInput = true;
    systemState = SYSTEM_RUNNING;
    
    Serial.println("Custom program step " + String(currentProgramStep) + ": Activated relay " + String(relayToActivate));
  }
}

void stopCustomProgram() {
  programRunning = false;
  waitingForProgramInput = false;
  currentProgramStep = 0;
  
  // Turn off all relays
  for (int i = 0; i < 6; i++) {
    relays.digitalWrite(i, HIGH);
    inputTimeoutActive[i] = false;
  }
  
  systemState = SYSTEM_STOPPED;
  Serial.println("Custom program stopped");
}

bool checkRelayPairSafety(int relay1, int relay2) {
  // Check if two relays are from the same motor pair
  int pair1 = relay1 / 2;
  int pair2 = relay2 / 2;
  
  return pair1 != pair2; // Safe if from different pairs
}