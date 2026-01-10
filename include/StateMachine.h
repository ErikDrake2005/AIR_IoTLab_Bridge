#pragma once
#include <Arduino.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include "config.h"
#include "PacketDef.h"
#include "Security.h"
#include "PowerManager.h"

// Enum chỉ nằm ở đây
enum BridgeState {
    ST_BOOT, ST_NORMAL, ST_POLLING,
    ST_WAKE_PULSE_HIGH, ST_WAKE_WAIT_GPIO, ST_WAKE_SEND_REPORT,
    ST_WAIT_NODE_SLEEP, ST_SEND_ACK_SLEEP, ST_WAIT_GW_CONFIRM,
    ST_LOW_BAT_WAIT_OFF, ST_DEEP_SLEEP
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
    unsigned long _timerState;
    String _lastKnownTimestamp;
    
    void _handleLoRaRx();
    void _sendLoRaPacket(JsonDocument& doc); 
    
    void _handleUartRx();
    void _sendUartPacket(JsonDocument& doc);
    
    void _processGatewayCmd(JsonDocument& doc);
    void _switchState(BridgeState newState);
};