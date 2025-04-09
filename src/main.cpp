#include "Arduino.h"
#include "PCF8574.h"

// Set i2c address
PCF8574 inputs(0x22,4,15);
PCF8574 relays(0x24,4,15);

void setup() {
  Serial.begin(115200);

  // Set pinMode to OUTPUT
  relays.pinMode(P0, OUTPUT);
  relays.pinMode(P1, OUTPUT);
  relays.pinMode(P2, OUTPUT);
  relays.pinMode(P3, OUTPUT);
  relays.pinMode(P4, OUTPUT);
  relays.pinMode(P5, OUTPUT);

  Serial.print("Init relays...");
  if (relays.begin()){
      Serial.println("OK");
  }else{
      Serial.println("KO");
  }

  inputs.pinMode(P0, INPUT);
  inputs.pinMode(P1, INPUT);
  inputs.pinMode(P2, INPUT);
  inputs.pinMode(P3, INPUT);
  inputs.pinMode(P4, INPUT);
  inputs.pinMode(P5, INPUT);

  Serial.print("Init inputs...");
  if (inputs.begin()){
      Serial.println("OK");
  }else{
      Serial.println("KO");
  }
  Serial.println("Init done");
}

void loop() {

  // Read input pins
  int input1 = inputs.digitalRead(P0);
  int input2 = inputs.digitalRead(P1);

  // Set relay states based on input
  relays.digitalWrite(P0, input1); // Relay 1 follows input 1
  relays.digitalWrite(P1, input2); // Relay 2 follows input 2

  delay(10); // Small delay to prevent excessive I2C traffic
}