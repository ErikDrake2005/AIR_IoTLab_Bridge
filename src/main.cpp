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
        vTaskDelay(1 / portTICK_PERIOD_MS); 
    }
}
void setup() {
    setCpuFrequencyMhz(160); 
    Serial.begin(115200);
    bridge.begin();
    xTaskCreatePinnedToCore(TaskLogicCode, "TaskLogic", 8192, NULL, 1, &TaskLogicHandle, 1);
}

void loop() { 
    vTaskDelete(NULL); 
}