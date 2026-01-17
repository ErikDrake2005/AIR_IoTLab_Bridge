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

// ========== POWER & RTOS ==========
#define PIN_BAT_ADC      35   
#define BAT_DIVIDER      2.0  
#define ADAPTER_VOLT     4.0  
#define VOLT_LOW_LIMIT   3.3  // <-- Bổ sung dòng này
#define VOLT_RECOVERY    3.6
#define UART_RX_QUEUE_SIZE   20
#define LORA_RX_QUEUE_SIZE   10
#define PROCESS_QUEUE_SIZE   20