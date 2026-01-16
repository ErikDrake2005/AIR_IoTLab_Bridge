#pragma once
#include <Arduino.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include "config.h"
#include "PacketDef.h" // Chứa PacketHeader, PacketUtils
#include "Security.h"
#include "PowerManager.h"
#include "CRC32.h"
#include "CommonTypes.h"

enum BridgeState { ST_BOOT, ST_NORMAL };

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
    
    // --- LORA HANDLING (DOWNLINK) ---
    void _handleLoRa();
    void _processDownlinkJson(JsonDocument& doc);

    // --- UART HANDLING (UPLINK) ---
    void _handleUart();
    void _wrapAndSendUplink(JsonDocument& nodeDoc);

    // --- UTILS ---
    void _tryLoRaInit();
    void _sendLoRa(JsonDocument& doc); 
    void _sendUart(String jsonString);
    
    // --- HANDSHAKE ---
    bool _wakeUpNode();
};