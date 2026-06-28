/*
   Project   : Modified from Forest Guard by Mukesh Shankhala (Final Patch for 38-pin ESP32)
   Description:
   Streams raw audio data from the INMP441 I²S microphone over serial.
   Visual feedback is handled via a standard multi-pin RGB LED.
*/

#include <Arduino.h>
#include <driver/i2s.h>

// ====== CONFIG ======
const i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr int SAMPLE_RATE   = 16000;     // Hz
constexpr int SECONDS_TO_GRAB = 10;      // 10 seconds
constexpr bool MIC_ON_RIGHT = false;     // Left channel tracking for INMP441 (L/R tied to GND)

// SAFE PINS for 38-Pin ESP32 (Do not use 6-11)
#define I2S_BCK  14
#define I2S_WS   15
#define I2S_SD   32

// Standard RGB LED Pins (Safe Output GPIOs)
#define LED_RED_PIN   25
#define LED_GREEN_PIN 26
#define LED_BLUE_PIN  27

// I2S DMA settings
constexpr int DMA_BUF_COUNT = 8;         
constexpr int DMA_BUF_LEN   = 256;       

// Data sizing metrics
constexpr size_t TOTAL_SAMPLES = SAMPLE_RATE * SECONDS_TO_GRAB;
constexpr size_t BYTES_TO_SEND = TOTAL_SAMPLES * sizeof(int16_t);

static inline int16_t s32_to_s16(int32_t s32) {
  return (int16_t)(s32 >> 11);
}

// Helper function to manage standard RGB LED states
void setRGBColor(bool redState, bool greenState, bool blueState) {
  // For Common Cathode: HIGH = On, LOW = Off
  digitalWrite(LED_RED_PIN, redState ? HIGH : LOW);
  digitalWrite(LED_GREEN_PIN, greenState ? HIGH : LOW);
  digitalWrite(LED_BLUE_PIN, blueState ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200); 

  // Initialize RGB Pins as Output
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_BLUE_PIN, OUTPUT);

  // Status Indicator: Blue during boot staging
  setRGBColor(false, false, true);
  delay(1000);

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = MIC_ON_RIGHT ? I2S_CHANNEL_FMT_ONLY_RIGHT : I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUF_COUNT,
    .dma_buf_len = DMA_BUF_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,     
    .data_in_num = I2S_SD
  };

  if (i2s_driver_install(I2S_PORT, &i2s_config, 0, nullptr) != ESP_OK) {
    while (true) { 
      // Blink Red if driver installation fails
      setRGBColor(true, false, false);
      delay(500);
      setRGBColor(false, false, false);
      delay(500); 
    }
  }
  
  if (i2s_set_pin(I2S_PORT, &pin_config) != ESP_OK) {
    while (true) { delay(1000); }
  }

  i2s_zero_dma_buffer(I2S_PORT);

  // Python Fix: Serial.println("READY") intentionally left out here.

  // Turn off light when ready for commands
  setRGBColor(false, false, false);
  delay(500);
}

void capture_and_send_10s() {
  Serial.print("START_AUDIO ");
  Serial.println((unsigned)BYTES_TO_SEND);

  constexpr size_t READ_CHUNK_BYTES = 1024;        
  alignas(4) uint8_t i2s_raw[READ_CHUNK_BYTES];
  size_t samples_sent = 0;

  while (samples_sent < TOTAL_SAMPLES) {
    size_t bytes_read = 0;
    esp_err_t r = i2s_read(I2S_PORT, i2s_raw, sizeof(i2s_raw), &bytes_read, portMAX_DELAY);
    if (r != ESP_OK || bytes_read == 0) continue;

    size_t frames = bytes_read / 4; 
    for (size_t i = 0; i < frames && samples_sent < TOTAL_SAMPLES; ++i) {
      int32_t s32;
      memcpy(&s32, &i2s_raw[i * 4], 4);
      int16_t s16 = s32_to_s16(s32);
      Serial.write((uint8_t*)&s16, sizeof(s16));
      samples_sent++;
    }
  }

  Serial.println();
  Serial.println("END_AUDIO");
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "CAPTURE_AUDIO") {
      // Status Indicator: Solid Green during recording window
      setRGBColor(false, true, false);

      capture_and_send_10s();

      // Return to blank state when capture window terminates
      setRGBColor(false, false, false);
    }
  }
}