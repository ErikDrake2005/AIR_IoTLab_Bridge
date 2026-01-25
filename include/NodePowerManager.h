#pragma once
#include <Arduino.h>
#include "config.h"

/**
 * NodePowerManager - Quản lý trạng thái ngủ/thức của Node
 * 
 * Chức năng:
 * - Đánh thức Node qua GPIO26 (PIN_NODE_WAKEUP)
 * - Theo dõi trạng thái Node qua GPIO25 (PIN_NODE_STATUS)
 * - Retry logic: 3 lần, mỗi lần cách 5s
 * - Cooldown: 15 phút sau khi thất bại 3 lần
 * - Lưu pending command khi Node đang ngủ
 */

enum NodeState {
    NODE_STATE_UNKNOWN,
    NODE_STATE_AWAKE,
    NODE_STATE_SLEEPING,
    NODE_STATE_WAKING,      // Đang trong quá trình đánh thức
    NODE_STATE_COOLDOWN     // Đợi 15 phút sau khi thất bại
};

// Trạng thái mặc định khi Node ngủ (reset state)
struct NodeDefaultStatus {
    const char* mode = "MANUAL";
    uint8_t chamberStatus = 0;   // stop
    uint8_t doorStatus = 1;      // open
    uint8_t fanStatus = 0;       // off
    uint8_t savedManualCycle = 5;
    uint8_t savedDailyMeasures = 4;
};

class NodePowerManager {
public:
    NodePowerManager();
    
    void begin();
    void update();  // Gọi trong loop để xử lý retry/cooldown
    
    // Trạng thái Node
    NodeState getState() const { return _state; }
    bool isNodeAwake() const;
    bool isNodeSleeping() const;
    bool isInCooldown() const { return _state == NODE_STATE_COOLDOWN; }
    
    // Điều khiển
    bool wakeUpNode();              // Đánh thức Node (blocking, có retry)
    bool requestSleep();            // Yêu cầu Node ngủ
    void confirmSleep();            // Xác nhận Node đã ngủ (GPIO25 LOW)
    
    // Pending command (khi cần đánh thức Node để gửi lệnh)
    void setPendingCommand(const char* cmd);
    bool hasPendingCommand() const { return _pendingCmd[0] != '\0'; }
    const char* getPendingCommand() const { return _pendingCmd; }
    void clearPendingCommand() { _pendingCmd[0] = '\0'; }
    
    // JSON mặc định khi Node ngủ
    const NodeDefaultStatus& getDefaultStatus() const { return _defaultStatus; }
    
    // Reset cooldown (khi có lệnh en=1 mới từ Server)
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
