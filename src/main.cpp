#include "Arduino.h"
#include "PCF8574.h"
// #include <AsyncTCP.h>
#include <WiFi.h>
#include <WiFiAP.h>
#include <stdlib.h>  // Required for random number generation
#include <ESPAsyncWebServer.h>
#include <Preferences.h>

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

void setup() {
  Serial.begin(115200);
  Serial.println("Starting setup...");

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

  randomSeed(analogRead(0)); // Seed the random number generator

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
    html += "<title>TARCZOWNIX Control</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; text-align: center; margin: 20px; }";
    html += ".container { max-width: 500px; margin: 0 auto; padding: 20px; border: 1px solid #ddd; border-radius: 10px; }";
    html += "h1 { color: #333; }";
    html += ".btn { background-color: #4CAF50; border: none; color: white; padding: 15px 32px; ";
    html += "text-align: center; text-decoration: none; display: inline-block; font-size: 16px; ";
    html += "margin: 10px 2px; cursor: pointer; border-radius: 8px; }";
    html += ".btn:hover { background-color: #45a049; }";
    html += ".form-group { margin: 15px 0; }";
    html += "input[type=number] { padding: 10px; width: 100px; border-radius: 4px; border: 1px solid #ccc; }";
    html += "label { display: inline-block; width: 120px; text-align: right; margin-right: 10px; }";
    html += ".card { border: 1px solid #ddd; border-radius: 8px; padding: 15px; margin: 15px 0; background-color: #f9f9f9; }";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<h1>TARCZOWNIX Control</h1>";

    // Relay control section
    html += "<div class='card'>";
    html += "<h2>Sequence Control</h2>";
    html += "<p>Click the button below to start the relay sequence:</p>";
    html += "<a href='/start' class='btn'>Start Relay 0</a>";
    html += "<p>Current relay states:</p>";
    html += "<ul style='list-style-type:none; padding:0;'>";
    // relay states
    for (int i = 0; i < 6; i++) {
      html += "<li>Relay " + String(i) + ": " + (relays.digitalRead(i) == LOW ? "ON" : "OFF") + "</li>";
    }
    html += "</ul>";
    html += "</div>";

    // Delay configuration section for each relay
    for (int i = 0; i < 6; i++) {
      html += "<div class='card'>";
      html += "<h2>Relay " + String(i) + " Delay Configuration</h2>";
      html += "<form action='/set-delay' method='get'>";
      html += "<input type='hidden' name='relay' value='" + String(i) + "'>";
      html += "<div class='form-group'>";
      html += "<label for='min'>Min Delay:</label>";
      html += "<input type='number' id='min' name='min' min='100' max='10000' value='" + String(minDelayRelay[i]) + "' required>";
      html += "</div>";
      html += "<div class='form-group'>";
      html += "<label for='max'>Max Delay:</label>";
      html += "<input type='number' id='max' name='max' min='100' max='20000' value='" + String(maxDelayRelay[i]) + "' required>";
      html += "</div>";
      html += "<input type='submit' class='btn' value='Save Settings'>";
      html += "</form>";
      html += "<p>Current range: " + String(minDelayRelay[i]) + " - " + String(maxDelayRelay[i]) + " ms</p>";
      html += "</div>";
    }

    html += "</div>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });

  server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request) {
    String message;
    bool hasError = false;

    // start relays 0, 2, 4 and check if they are not on
    if (relays.digitalRead(0) == HIGH && relays.digitalRead(2) == HIGH && relays.digitalRead(4) == HIGH) {
      relays.digitalWrite(0, LOW); // Set relay 0 to on
      relays.digitalWrite(2, LOW); // Set relay 2 to on
      relays.digitalWrite(4, LOW); // Set relay 4 to on
      message = "Relay 0, 2, and 4 are now ON";
    } else {
      message = "Error: One or more relays are already ON";
      hasError = true;
    }


    // Return response with redirect
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='3;url=/' />"; // Redirect after 3 seconds
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

  server.begin(); // Start the server

  
}

void loop() {

  if(relays.digitalRead(0) == LOW) {
    Serial.println("Relay 0 is ON");
    if(inputs.digitalRead(0) == LOW) {
      Serial.println("Input 0 is LOW, turning off relay 0");
      relays.digitalWrite(0, HIGH); // Set relay 0 to off
      delay(getRandomDelay(0)); // Add random delay before next action
      relays.digitalWrite(1, LOW); // Set relay 1 to on
    }
    delay(10); // Wait for 1 second before checking again
  }

  if(relays.digitalRead(1) == LOW) {
    Serial.println("Relay 1 is ON");
    if(inputs.digitalRead(1) == LOW) {
      Serial.println("Input 1 is LOW, turning off relay 1");
      relays.digitalWrite(1, HIGH); // Set relay 1 to off
      delay(getRandomDelay(1)); // Add random delay before next action
      relays.digitalWrite(0, LOW); // Set relay 0 to on
    }
    delay(10); // Wait for 1 second before checking again
  }

  if(relays.digitalRead(2) == LOW) {
    Serial.println("Relay 2 is ON");
    if(inputs.digitalRead(2) == LOW) {
      Serial.println("Input 2 is LOW, turning off relay 2");
      relays.digitalWrite(2, HIGH); // Set relay 2 to off
      delay(getRandomDelay(2)); // Add random delay before next action
      relays.digitalWrite(3, LOW); // Set relay 3 to on
    }
    delay(10); // Wait for 1 second before checking again
  }
  if(relays.digitalRead(3) == LOW) {
    Serial.println("Relay 3 is ON");
    if(inputs.digitalRead(3) == LOW) {
      Serial.println("Input 3 is LOW, turning off relay 3");
      relays.digitalWrite(3, HIGH); // Set relay 3 to off
      delay(getRandomDelay(3)); // Add random delay before next action
      relays.digitalWrite(2, LOW); // Set relay 2 to on
    }
    delay(10); // Wait for 1 second before checking again
  }
  if(relays.digitalRead(4) == LOW) {
    Serial.println("Relay 4 is ON");
    if(inputs.digitalRead(4) == LOW) {
      Serial.println("Input 4 is LOW, turning off relay 4");
      relays.digitalWrite(4, HIGH); // Set relay 4 to off
      delay(getRandomDelay(4)); // Add random delay before next action
      relays.digitalWrite(5, LOW); // Set relay 5 to on
    }
    delay(10); // Wait for 1 second before checking again
  }
  if(relays.digitalRead(5) == LOW) {
    Serial.println("Relay 5 is ON");
    if(inputs.digitalRead(5) == LOW) {
      Serial.println("Input 5 is LOW, turning off relay 5");
      relays.digitalWrite(5, HIGH); // Set relay 5 to off
      delay(getRandomDelay(5)); // Add random delay before next action
      relays.digitalWrite(4, LOW); // Set relay 4 to on
    }
    delay(10); // Wait for 1 second before checking again
  }
  
  // Short delay to prevent CPU overuse
  delay(10);
}

// Function to get a random delay in milliseconds for a specific relay
int getRandomDelay(int relay) {
  return random(minDelayRelay[relay], maxDelayRelay[relay]);
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