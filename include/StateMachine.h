#pragma once
#include <Arduino.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include "config.h"
#include "PacketDef.h"
#include "Security.h"
#include "PowerManager.h"
#include "NodePowerManager.h"
#include "CRC32.h"
#include "CommonTypes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
struct PacketMsg {
    uint8_t buf[512];
    int len;
    uint8_t source;
};

class StateMachine {
public:
    StateMachine();
    void begin();
    
    // Task Functions
    static void uartRxTaskFn(void* param);
    static void loraRxTaskFn(void* param);
    static void processTaskFn(void* param);
    static void powerMonitorTaskFn(void* param);

private:
    PowerManager _pwr;
    NodePowerManager _nodePwr;
    HardwareSerial* _uart;
    
    QueueHandle_t _processQueue;
    QueueHandle_t _txQueue;
    
    SemaphoreHandle_t _loraMutex;
    int _lastRssi = 0;
    float _lastSnr = 0;
    void _uartRxLoop();
    void _loraRxLoop();
    void _processLoop();
    void _powerMonitorLoop();

    // Helpers
    void _sendToNode(const char* jsonCmd);
    void _sendToGateway(JsonDocument& doc);
    void _sendFixedToGateway(uint8_t* data, int len);
    void _sendSleepReport();
    void _enterLowPowerMode();
    bool _extractJsonFromUart(String raw, String& outJson);
};