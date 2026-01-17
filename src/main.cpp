#include <Arduino.h>
#include "StateMachine.h"

StateMachine bridge; 

void setup() {
    // 1. Khởi động Serial Debug trước
    Serial.begin(115200);
    
    // 2. QUAN TRỌNG: Chờ 3s để ổn định nguồn (tránh sụt áp khi bật LoRa)
    Serial.println(">>> WAITING FOR POWER STABILIZATION (3s)...");
    delay(3000); 
    
    Serial.println("\n\n=== BRIDGE INIT ===");
    Serial.println("[SETUP] Serial initialized at 115200 baud");
    
    // 3. Khởi động Bridge
    Serial.println("[SETUP] Calling bridge.begin()...");
    bridge.begin();
    
    Serial.println("[SETUP] Bridge initialized successfully");
    Serial.println("[SETUP] Ready for main loop\n");
}

void loop() { 
    bridge.loop();
    // Delay nhỏ để giảm tải CPU, tránh WDT trên Core 1
    delay(10); 
}