#include "PowerManager.h"

void PowerManager::begin() { 
    pinMode(PIN_BAT_ADC, INPUT);
    Serial.println("[PWR] PowerManager initialized");
}

float PowerManager::getVoltage() {
    float total = 0;
    for(int i=0; i<10; i++) { 
        total += analogRead(PIN_BAT_ADC); 
        delay(1); 
    }
    int raw = (int)(total / 10.0);
    
    // ✅ Gracefully handle no battery - return 0 silently without error
    if (raw <= 0) {
        return 0.0;  // Battery not connected, let caller decide
    }
    
    float voltage = (raw / 4095.0) * 3.3 * BAT_DIVIDER;
    
    // ✅ Bounds check
    if (voltage < 0.5 || voltage > 5.0) {
        return 0.0;  // Out of range - assume no battery
    }
    
    return voltage;
}

// Detect if battery is connected or external power (adapter) is used
// Returns true if external power is detected (battery pins open, < 0.3V)
bool PowerManager::isAdapterConnected() { 
    float v = getVoltage();
    return v < ADAPTER_VOLT;  // < 0.3V means battery not connected, using adapter
}

bool PowerManager::isBatteryLow() {
    if (isAdapterConnected()) {
        // External power detected - assume full charge
        return false; 
    }
    float v = getVoltage();
    return v < VOLT_LOW_LIMIT;
}

// Check if battery is OK or has recovered
// Returns true if on external power (battery pins open)
bool PowerManager::isBatteryOk() {
    if (isAdapterConnected()) {
        // External power detected - always OK
        return true;
    }
    float v = getVoltage();
    return v >= VOLT_RECOVERY;
}

void PowerManager::sleepForSeconds(uint64_t seconds) {
    Serial.printf("[PWR] Light Sleep %llu s\n", seconds);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_light_sleep_start();
    Serial.println("[PWR] Woke up from Light Sleep!");
}