#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "StateMachine.h"

StateMachine bridge; 
TaskHandle_t TaskLoRaHandle;
TaskHandle_t TaskLogicHandle;

void TaskLogicCode(void * pvParameters) {
    for(;;) { 
        bridge.loop();
        vTaskDelay(10 / portTICK_PERIOD_MS);  // 10ms delay to prevent watchdog
    }
}

void setup() {
    setCpuFrequencyMhz(160); 
    Serial.begin(115200);
    
    // Disable watchdog during initialization
    disableCore0WDT();
    disableCore1WDT();
    
    delay(100);
    Serial.println("\n=== BRIDGE INIT ===");
    
    bridge.begin();
    
    delay(100);
    Serial.println("[SETUP] Bridge initialized");
    
    xTaskCreatePinnedToCore(TaskLogicCode, "TaskLogic", 8192, NULL, 1, &TaskLogicHandle, 1);
    
    delay(100);
    Serial.println("[SETUP] Task created");
}

void loop() { 
    vTaskDelete(NULL); 
}