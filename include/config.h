#pragma once
#include <Arduino.h>

// ========== ĐỊNH DANH =========
#define BRIDGE_DEVICE_ID "NODE_01" 
#define TARGET_NODE_ID   "AIR_VL_01" // ID của Node đi kèm (để filter nếu cần)
#define MY_AES_KEY       "IoTLab@Bridge01" 

// ========== UART KẾT NỐI NODE (QUAN TRỌNG: PHẢI KHỚP NODE) ==========
#define UART_BAUD       921600   // <--- Đã sửa từ 115200 lên 921600
#define UART_RX_PIN     16
#define UART_TX_PIN     17

// ========== HANDSHAKE VỚI NODE ==========
#define PIN_NODE_WAKEUP  26  // Output: Kích chân này để gọi Node dậy
#define PIN_NODE_STATUS  25  // Input:  Đọc chân này xem Node đang ngủ hay thức

// ========== LORA CONFIG (Giữ nguyên) ==========
#define LORA_FREQ       433E6
#define LORA_TX_POWER   20
#define LORA_SF         8      
#define LORA_BW         125E3  
#define LORA_CR         5      
#define LORA_SYNC_WORD  0xF3   

#define LORA_CS_PIN     5   
#define LORA_RST_PIN    4
#define LORA_DIO0_PIN   2   
#define LORA_SCK_PIN    18
#define LORA_MISO_PIN   19
#define LORA_MOSI_PIN   23

// ========== POWER & BATTERY MONITORING ==========
#define PIN_BAT_ADC           15    // GPIO15 for battery ADC
#define BAT_DIVIDER           2.0   // Voltage divider ratio (4.2V -> 2.1V)
#define BAT_MAX_VOLTAGE       2.1   // Max voltage at ADC pin
#define ADAPTER_DETECT_VOLT   0.5   // Below this = adapter/debug mode (no battery)
#define VOLT_LOW_LIMIT        3.7   // Enter low-power mode below this
#define VOLT_RECOVERY         3.9   // Exit low-power mode above this
#define BAT_CHECK_INTERVAL_MS 60000 // Check battery every 1 minute
#define LOW_POWER_SLEEP_SEC   900   // 15 minutes deep sleep when low battery

// ========== NODE WAKE/SLEEP CONTROL ==========
#define NODE_WAKE_RETRY_MAX   3     // Max wake attempts before 15min cooldown
#define NODE_WAKE_RETRY_MS    5000  // Wait 5s between wake attempts
#define NODE_WAKE_COOLDOWN_MS 900000 // 15 minutes cooldown after failed wakes

// ========== RTOS ==========
#define UART_RX_QUEUE_SIZE   20
#define LORA_RX_QUEUE_SIZE   10
#define PROCESS_QUEUE_SIZE   20