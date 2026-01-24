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

// Gói tin nội bộ dùng trong hàng đợi
struct PacketMsg {
    uint8_t buf[512];
    int len;
    uint8_t source; // 0=FROM_UART(Node), 1=FROM_LORA(Gateway)
};

class StateMachine {
public:
    StateMachine();
    void begin();
    
    // Task Functions
    static void uartRxTaskFn(void* param);
    static void loraRxTaskFn(void* param);
    static void processTaskFn(void* param);
    static void powerMonitorTaskFn(void* param);  // NEW: Battery monitoring task

private:
    PowerManager _pwr;
    NodePowerManager _nodePwr;  // NEW: Node power management
    HardwareSerial* _uart;
    
    QueueHandle_t _processQueue; // Hàng đợi xử lý chính
    QueueHandle_t _txQueue;      // Hàng đợi gửi đi (LoRa/UART)
    
    SemaphoreHandle_t _loraMutex;
    int _lastRssi = 0;
    float _lastSnr = 0;
    
    // Các hàm vòng lặp Task
    void _uartRxLoop();
    void _loraRxLoop();
    void _processLoop();
    void _powerMonitorLoop();  // NEW: Battery check loop

    // Helpers
    void _sendToNode(const char* jsonCmd);
    void _sendToGateway(JsonDocument& doc);
    void _sendFixedToGateway(uint8_t* data, int len);  // Fixed-Schema packets
    
    // NEW: Sleep report và low-power mode
    void _sendSleepReport();       // Gửi JSON mặc định khi Node ngủ
    void _enterLowPowerMode();     // Bridge deep-sleep cycle
    
    // Tách chuỗi CRC từ Node: "JSON|CRC" -> Check OK -> Trả về JSON
    bool _extractJsonFromUart(String raw, String& outJson);
};