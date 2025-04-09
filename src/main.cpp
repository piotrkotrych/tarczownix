#include "Arduino.h"
#include "PCF8574.h"
#include <WiFi.h>
#include <WiFiAP.h>
#include <WebServer.h>

// --- Constants ---
const char* ssid = "ESP32-AccessPoint";
const char* password = "password";
const IPAddress local_ip(192, 168, 1, 111);
const IPAddress gateway(192, 168, 1, 1);
const IPAddress subnet(255, 255, 255, 0);
const unsigned long DEBOUNCE_DELAY = 50; // ms
const unsigned long PRINT_INTERVAL = 1000; // ms

// --- I2C ---
PCF8574 inputs(0x22, 4, 15);
PCF8574 relays(0x24, 4, 15);

// --- Enums ---
enum RelayState { RELAY_OFF = HIGH, RELAY_ON = LOW }; // Inverted logic
enum InputState { INPUT_INACTIVE = HIGH, INPUT_ACTIVE = LOW }; // Inverted logic

// --- Web Server ---
WebServer server(80);

// --- State Variables ---
bool sequenceRunning = false;
unsigned long lastStateChangeTime = 0;
bool inputState[6] = {INPUT_INACTIVE, INPUT_INACTIVE, INPUT_INACTIVE, INPUT_INACTIVE, INPUT_INACTIVE, INPUT_INACTIVE};

// --- Function Declarations ---
void handleRoot();
void handleToggleRelay();
void handleStartSequence();
void updateInputs();
void runSequence();

// --- Handlers ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><title>TARCZOWNIX Control</title><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<style>body { text-align: center; font-family: Arial, sans-serif; } .button { padding: 10px 20px; font-size: 16px; cursor: pointer; }</style></head><body>";
  html += "<h1>TARCZOWNIX Control</h1>";

  html += "<h2>Relay Control</h2>";
  for (int i = 0; i < 6; i++) {
    html += "<p>Relay " + String(i) + ": " + (relays.digitalRead(i) == RELAY_ON ? "ON" : "OFF");
    html += "  <a href='/toggle?relay=" + String(i) + "'><button class='button'>Toggle</button></a></p>";
  }

  html += "<h2>Input Status</h2>";
  for (int i = 0; i < 6; i++) {
    html += "<p>Input " + String(i) + ": " + (inputState[i] == INPUT_ACTIVE ? "ACTIVE" : "INACTIVE") + "</p>";
  }

  html += "<h2>Sequence Control</h2>";
  if (sequenceRunning) {
    html += "<p>Sequence Running</p>";
  } else {
    html += "<a href='/start'><button class='button'>Start Sequence</button></a>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleToggleRelay() {
  String relayParam = server.arg("relay");
  int relayPin = relayParam.toInt();
  if (relayPin >= 0 && relayPin < 6) {
    int currentState = relays.digitalRead(relayPin);
    relays.digitalWrite(relayPin, (currentState == RELAY_ON) ? RELAY_OFF : RELAY_ON);
    Serial.printf("Toggled relay %d\n", relayPin);
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleStartSequence() {
  sequenceRunning = true;
  lastStateChangeTime = millis();
  relays.digitalWrite(P0, RELAY_ON); // Start with Relay 0 ON
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// --- Setup ---
void setup() {
  Serial.begin(115200);

  // --- Pin Modes ---
  for (int i = 0; i < 6; i++) {
    relays.pinMode(i, OUTPUT);
    relays.digitalWrite(i, RELAY_OFF); // Initialize relays OFF
    inputs.pinMode(i, INPUT);
    inputs.digitalWrite(i, HIGH); // Enable pull-up resistors
  }

  // --- I2C Begin ---
  if (!relays.begin()) { Serial.println("Relays init failed"); while (1); }
  if (!inputs.begin()) { Serial.println("Inputs init failed"); while (1); }

  // --- WiFi Setup ---
  WiFi.softAP(ssid, password);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  // --- Web Server Setup ---
  server.on("/", handleRoot);
  server.on("/toggle", handleToggleRelay);
  server.on("/start", handleStartSequence);
  server.begin();
  Serial.println("HTTP server started");
}

// --- Loop ---
void loop() {
  server.handleClient();
  updateInputs();
  runSequence();
  delay(10);
}

// --- Functions ---
void updateInputs() {
  static unsigned long lastDebounceTime[6] = {0};
  for (int i = 0; i < 6; i++) {
    int reading = inputs.digitalRead(i);
    if (reading != inputState[i] && (millis() - lastDebounceTime[i] > DEBOUNCE_DELAY)) {
      inputState[i] = reading;
      lastDebounceTime[i] = millis();
    }
  }
}

void runSequence() {
  if (!sequenceRunning) return;

  unsigned long now = millis();

  if (relays.digitalRead(P0) == RELAY_ON && inputState[P0] == INPUT_ACTIVE) {
    relays.digitalWrite(P0, RELAY_OFF);
    relays.digitalWrite(P1, RELAY_ON);
    Serial.println("Relay 0 OFF, Relay 1 ON");
    lastStateChangeTime = now;
  } else if (relays.digitalRead(P1) == RELAY_ON && inputState[P1] == INPUT_ACTIVE) {
    relays.digitalWrite(P1, RELAY_OFF);
    relays.digitalWrite(P0, RELAY_ON);
    Serial.println("Relay 1 OFF, Relay 0 ON");
    lastStateChangeTime = now;
  }
}