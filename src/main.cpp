#include <Arduino.h>
#include "StateMachine.h"

StateMachine bridge; 

void setup() {
    // Initialize Serial FIRST
    Serial.begin(115200);
    delay(500);
    
    // Skip CPU frequency change - use default
    
    Serial.println("\n\n=== BRIDGE INIT ===");
    Serial.println("[SETUP] Serial initialized at 115200 baud");
    
    Serial.println("[SETUP] Calling bridge.begin()...");
    bridge.begin();
    Serial.println("[SETUP] Bridge initialized successfully");
    Serial.println("[SETUP] Complete - ready for loop\n");
}

void loop() { 
    bridge.loop();
    delay(10);
}
