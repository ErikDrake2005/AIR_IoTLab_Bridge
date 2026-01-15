#pragma once
#include <Arduino.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include "config.h"
#include "PacketDef.h"
#include "Security.h"
#include "PowerManager.h"

// Trạng thái hoạt động Bridge - Simplified (Always-On Mode)
enum BridgeState { 
    ST_BOOT,                // Khởi động
    ST_NORMAL               // Luôn hoạt động, LoRa RX listening 24/7, Node tự quản lý deep_sleep
};

// Cấu trúc lưu trữ trạng thái Node
struct NodeState {
    String device_ID;           // Bridge's own device_ID
    String node_device_id;      // Connected Node identifier
    String door_status;         // "open", "close", "unknown"
    String fan_status;          // "on", "off", "unknown"
    String mode;                // "manual", "auto", "unknown"
    String measuring;           // "YES", "NO"
    String timestamp;
    float pin_voltage;
    
    NodeState() : device_ID(BRIDGE_DEVICE_ID), node_device_id(NODE_DEVICE_ID),
                  door_status("unknown"), fan_status("unknown"), 
                  mode("manual"), measuring("NO"), timestamp("0"), pin_voltage(3.6) {}
};

class StateMachine {
public:
    StateMachine();
    void begin();
    void loop();

private:
    BridgeState _state;
    PowerManager _pwr;
    HardwareSerial* _uart;
    uint32_t _packetCounter;
    // State management
    NodeState _nodeState;
    unsigned long _lastHeartbeat;
    
    // LoRa management
    bool _loraReady;
    bool _loraInRxMode;  // Track if LoRa is in RX mode (half-duplex state)
    unsigned long _lastLoRaRetry;
    const unsigned long LORA_RETRY_INTERVAL = 10000;  // Retry LoRa every 10s
    
    // Device ID filtering
    bool _isCommandForMe(const JsonDocument& doc);  // Check device_ID match
    void _addDeviceIdToResponse(JsonDocument& doc); // Add Bridge's device_ID
    void _sendReport(int sleepMode);
    // Command translation for Node-RED compatibility
    void _translateNodeRedCmd(JsonDocument& doc);   // Translate Node-RED format to internal
    
    void _handleLoRa();
    void _handleUart();
    void _tryLoRaInit();  // Retry LoRa initialization
    void _sendLoRa(JsonDocument& doc); 
    void _sendUart(JsonDocument& doc);
    void _procGwCmd(JsonDocument& doc);
    void _procNodeJson(JsonDocument& doc);
    void _sendReport(bool sleep);
    void _setState(BridgeState s);
    void _checkBatteryPeriodic();
    void _wakeNodeWithPulse();
};