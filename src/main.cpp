#include <Arduino.h>
#include "StateMachine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

StateMachine bridge;

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("\n\n=== BRIDGE STARTING ===");
    Serial.flush();
    delay(100);
    
    Serial.println("Creating Bridge...");
    Serial.flush();
    
    bridge.begin();
    
    Serial.println("Bridge ready, entering FreeRTOS scheduler");
    Serial.flush();
    delay(200);
    
    // Delete the loop task - FreeRTOS tasks handle everything
    vTaskDelete(nullptr);
}

void loop() {
    vTaskDelete(nullptr);
}