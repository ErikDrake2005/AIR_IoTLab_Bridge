#pragma once
#include <Arduino.h>
#include "config.h"


enum NodeState {
    NODE_STATE_UNKNOWN,
    NODE_STATE_AWAKE,
    NODE_STATE_SLEEPING,
    NODE_STATE_WAKING,
    NODE_STATE_COOLDOWN
};
struct NodeDefaultStatus {
    const char* mode = "MANUAL";
    uint8_t chamberStatus = 0; // stop
    uint8_t doorStatus = 1; // open
    uint8_t fanStatus = 0; // off
    uint8_t savedManualCycle = 5;
    uint8_t savedDailyMeasures = 4;
};

class NodePowerManager {
public:
    NodePowerManager();
    
    void begin();
    void update();  
    NodeState getState() const { return _state; }
    bool isNodeAwake() const;
    bool isNodeSleeping() const;
    bool isInCooldown() const { return _state == NODE_STATE_COOLDOWN; }
    
    // Điều khiển
    bool wakeUpNode(); 
    bool requestSleep(); 
    void confirmSleep();   
    void setPendingCommand(const char* cmd);
    bool hasPendingCommand() const { return _pendingCmd[0] != '\0'; }
    const char* getPendingCommand() const { return _pendingCmd; }
    void clearPendingCommand() { _pendingCmd[0] = '\0'; }
    const NodeDefaultStatus& getDefaultStatus() const { return _defaultStatus; }
    void resetCooldown();
    
private:
    NodeState _state;
    NodeDefaultStatus _defaultStatus;
    // Retry tracking
    uint8_t _wakeAttempts;
    unsigned long _lastWakeAttemptMs;
    unsigned long _cooldownStartMs;
    
    // Pending command buffer
    char _pendingCmd[256];
    
    // Helper
    bool _pulseWakePin();
    bool _waitForNodeAwake(unsigned long timeoutMs);
};
