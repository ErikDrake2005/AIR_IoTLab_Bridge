#include "NodePowerManager.h"

NodePowerManager::NodePowerManager() {
    _state = NODE_STATE_UNKNOWN;
    _wakeAttempts = 0;
    _lastWakeAttemptMs = 0;
    _cooldownStartMs = 0;
    _pendingCmd[0] = '\0';
}

void NodePowerManager::begin() {
    pinMode(PIN_NODE_WAKEUP, OUTPUT);
    digitalWrite(PIN_NODE_WAKEUP, LOW);
    
    pinMode(PIN_NODE_STATUS, INPUT_PULLDOWN);
    
    // Kiểm tra trạng thái ban đầu
    if (digitalRead(PIN_NODE_STATUS) == HIGH) {
        _state = NODE_STATE_AWAKE;
        Serial.println("[NPM] Node is AWAKE");
    } else {
        _state = NODE_STATE_SLEEPING;
        Serial.println("[NPM] Node is SLEEPING");
    }
}

void NodePowerManager::update() {
    // Cập nhật trạng thái dựa trên GPIO
    bool gpioHigh = (digitalRead(PIN_NODE_STATUS) == HIGH);
    
    // Xử lý cooldown
    if (_state == NODE_STATE_COOLDOWN) {
        if (millis() - _cooldownStartMs >= NODE_WAKE_COOLDOWN_MS) {
            Serial.println("[NPM] Cooldown ended, retry allowed");
            _state = NODE_STATE_SLEEPING;
            _wakeAttempts = 0;
        }
        return;
    }
    
    // Cập nhật trạng thái
    if (gpioHigh && _state != NODE_STATE_AWAKE) {
        _state = NODE_STATE_AWAKE;
        _wakeAttempts = 0;
        Serial.println("[NPM] Node woke up (GPIO HIGH)");
    } 
    else if (!gpioHigh && _state == NODE_STATE_AWAKE) {
        _state = NODE_STATE_SLEEPING;
        Serial.println("[NPM] Node went to sleep (GPIO LOW)");
    }
}

bool NodePowerManager::isNodeAwake() const {
    return digitalRead(PIN_NODE_STATUS) == HIGH;
}

bool NodePowerManager::isNodeSleeping() const {
    return digitalRead(PIN_NODE_STATUS) == LOW;
}

bool NodePowerManager::wakeUpNode() {
    if (isNodeAwake()) {
        _state = NODE_STATE_AWAKE;
        _wakeAttempts = 0;
        return true;
    }
    if (_state == NODE_STATE_COOLDOWN) {
        Serial.println("[NPM] In cooldown, cannot wake");
        return false;
    }
    
    _state = NODE_STATE_WAKING;
    
    // Retry loop
    for (int attempt = 0; attempt < NODE_WAKE_RETRY_MAX; attempt++) {
        _wakeAttempts++;
        _lastWakeAttemptMs = millis();
        
        Serial.printf("[NPM] Wake attempt %d/%d\n", attempt + 1, NODE_WAKE_RETRY_MAX);
        
        // Pulse wake pin
        if (_pulseWakePin()) {
            // Đợi Node thức
            if (_waitForNodeAwake(NODE_WAKE_RETRY_MS)) {
                _state = NODE_STATE_AWAKE;
                _wakeAttempts = 0;
                Serial.println("[NPM] Node woke up successfully!");
                return true;
            }
        }
        
        // Đợi trước khi retry
        if (attempt < NODE_WAKE_RETRY_MAX - 1) {
            vTaskDelay(NODE_WAKE_RETRY_MS / portTICK_PERIOD_MS);
        }
    }
    
    // Thất bại sau 3 lần -> vào cooldown
    Serial.println("[NPM] Wake failed after 3 attempts, entering 15min cooldown");
    _state = NODE_STATE_COOLDOWN;
    _cooldownStartMs = millis();
    
    return false;
}

bool NodePowerManager::requestSleep() {
    Serial.println("[NPM] Sleep requested for Node");
    return true;
}

void NodePowerManager::confirmSleep() {
    if (isNodeSleeping()) {
        _state = NODE_STATE_SLEEPING;
        Serial.println("[NPM] Node sleep confirmed (GPIO LOW)");
    }
}

void NodePowerManager::setPendingCommand(const char* cmd) {
    if (cmd) {
        strncpy(_pendingCmd, cmd, sizeof(_pendingCmd) - 1);
        _pendingCmd[sizeof(_pendingCmd) - 1] = '\0';
        Serial.printf("[NPM] Pending command saved: %s\n", _pendingCmd);
    }
}

void NodePowerManager::resetCooldown() {
    if (_state == NODE_STATE_COOLDOWN) {
        Serial.println("[NPM] Cooldown reset by new en=1 command");
        _state = NODE_STATE_SLEEPING;
        _wakeAttempts = 0;
    }
}

bool NodePowerManager::_pulseWakePin() {
    digitalWrite(PIN_NODE_WAKEUP, HIGH);
    vTaskDelay(100 / portTICK_PERIOD_MS);  // 100ms pulse
    digitalWrite(PIN_NODE_WAKEUP, LOW);
    return true;
}

bool NodePowerManager::_waitForNodeAwake(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        if (digitalRead(PIN_NODE_STATUS) == HIGH) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            return true;
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
    return false;
}
