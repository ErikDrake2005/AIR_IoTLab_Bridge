#include "PowerManager.h"

void PowerManager::begin() { 
    pinMode(PIN_BAT_ADC, INPUT);
    _cachedVoltage = getVoltage();
    _cachedPercent = _voltageToPercent(_cachedVoltage);
    _lastCheckMs = millis();
    _lowPowerMode = false;
    _shouldEnterLowPower = false;
    Serial.printf("[PWR] PowerManager initialized, V=%.2fV (%d%%)\n", _cachedVoltage, _cachedPercent);
}

void PowerManager::update() {
    // Kiểm tra pin định kỳ mỗi BAT_CHECK_INTERVAL_MS (1 phút)
    if (millis() - _lastCheckMs >= BAT_CHECK_INTERVAL_MS) {
        _lastCheckMs = millis();
        _cachedVoltage = getVoltage();
        _cachedPercent = _voltageToPercent(_cachedVoltage);
        
        Serial.printf("[PWR] Battery check: %.2fV (%d%%)\n", _cachedVoltage, _cachedPercent);
        
        // Kiểm tra trạng thái
        if (!_lowPowerMode && isBatteryLow()) {
            Serial.printf("[PWR] *** BATTERY LOW (%.2fV < %.1fV)! Should enter low-power mode ***\n", 
                          _cachedVoltage, VOLT_LOW_LIMIT);
            _shouldEnterLowPower = true;
        }
        else if (_lowPowerMode && isBatteryOk()) {
            Serial.println("[PWR] Battery recovered, can exit low-power mode");
            _lowPowerMode = false;
        }
    }
}

float PowerManager::getVoltage() {
    float total = 0;
    for(int i=0; i<10; i++) { 
        total += analogRead(PIN_BAT_ADC); 
        delay(1); 
    }
    int raw = (int)(total / 10.0);
    
    // Gracefully handle no battery - return 0 silently
    if (raw <= 0) {
        return 0.0;
    }
    
    // Công thức: V_real = V_ADC × K^(-1)
    // V_ADC = raw / 4095 * 3.3V
    // V_real = V_ADC * BAT_DIVIDER (K^-1 = 3.76)
    float adcVoltage = (raw / 4095.0) * 3.3;
    float realVoltage = adcVoltage * BAT_DIVIDER;
    
    // Bounds check for 3S LiPo (9V - 13V valid range)
    if (realVoltage < 2.0 || realVoltage > 15.0) {
        return 0.0;  // Out of range - assume no battery
    }
    
    return realVoltage;
}

float PowerManager::getCachedVoltage() {
    return _cachedVoltage;
}

int PowerManager::getBatteryPercent() {
    return _voltageToPercent(getVoltage());
}

int PowerManager::getCachedPercent() {
    return _cachedPercent;
}

// Convert voltage to percentage (0-100%)
// 3S LiPo: 12.6V = 100%, 9.0V = 0%
int PowerManager::_voltageToPercent(float voltage) {
    if (voltage <= 0 || voltage < BAT_EMPTY_VOLTAGE) {
        return 0;
    }
    if (voltage >= BAT_FULL_VOLTAGE) {
        return 100;
    }
    
    // Linear interpolation: (V - Vmin) / (Vmax - Vmin) * 100
    float percent = (voltage - BAT_EMPTY_VOLTAGE) / (BAT_FULL_VOLTAGE - BAT_EMPTY_VOLTAGE) * 100.0;
    return (int)percent;
}

// Detect if battery is connected or external power (adapter) is used
// Returns true if external power detected (voltage < 2V means battery pins open)
bool PowerManager::isAdapterConnected() { 
    float v = getCachedVoltage();
    if (v == 0.0) v = getVoltage();
    return v < ADAPTER_DETECT_VOLT;
}

bool PowerManager::isBatteryLow() {
    if (isAdapterConnected()) {
        // External power detected - assume full charge
        return false; 
    }
    float v = getCachedVoltage();
    if (v == 0.0) v = getVoltage();
    return v < VOLT_LOW_LIMIT;  // < 3.7V
}

// Check if battery is OK or has recovered
// Returns true if on external power (battery pins open)
bool PowerManager::isBatteryOk() {
    if (isAdapterConnected()) {
        // External power detected - always OK
        return true;
    }
    float v = getCachedVoltage();
    if (v == 0.0) v = getVoltage();
    return v >= VOLT_RECOVERY;  // >= 3.9V
}

void PowerManager::deepSleep(uint64_t seconds) {
    Serial.printf("[PWR] Deep Sleep for %llu seconds\n", seconds);
    Serial.flush();
    delay(10);
    
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_deep_sleep_start();
    // Không trở về từ deep sleep - sẽ reset
}

void PowerManager::lightSleep(uint64_t seconds) {
    Serial.printf("[PWR] Light Sleep for %llu seconds\n", seconds);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_light_sleep_start();
    Serial.println("[PWR] Woke up from Light Sleep!");
}