#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ==================================================================
// EDGE IMPULSE AI INTEGRATION
// ==================================================================
#include <Jal-Rakshak_PipeNode__inferencing.h>
#include <driver/i2s_std.h> 

// ==================================================================
// NODE IDENTITY & LOCATION DEFAULTS
// ==================================================================
#define NODE_ID             1
#define NODE_LATITUDE       0.000000  
#define NODE_LONGITUDE      0.000000  
//before upload edit the data here
// ==================================================================
// HARDWARE PIN DEFINITIONS
// ==================================================================
// LoRa SPI (3.3V)
#define LORA_SCK      18
#define LORA_MISO     19
#define LORA_MOSI     23
#define LORA_CS       21
#define LORA_RST      22
#define LORA_DIO0     25

// INMP441 Microphone (3.3V)
#define I2S_WS        33
#define I2S_SCK       32
#define I2S_SD        27

// Analog & Digital Water Sensors
#define PIN_PH        34 // ADC1_CH6 (Voltage Divided)
#define PIN_TDS       35 // ADC1_CH7 (Voltage Divided)
#define PIN_TURB      36 // ADC1_CH0 (Voltage Divided)
#define PIN_TEMP      4  // 3.3V Digital 1-Wire (Needs 4.7kΩ Pull-up)
#define PIN_WATER     13 // 3.3V Digital Interrupt

// ==================================================================
// 5V HARDWARE CALIBRATION
// ==================================================================
#define V_DIVIDER_RATIO     2.0  
#define PH_OFFSET           0.00  
#define TURB_CLEAR_VOLTAGE  4.10  

#define ALARM_TDS_PPM     800.0   
#define ALARM_TURB_NTU    400.0   
#define ALARM_PH_LOW      6.0     
#define ALARM_PH_HIGH     8.5     

// ==================================================================
// SYSTEM STRUCTS & GLOBALS
// ==================================================================
#pragma pack(push, 1)
struct TelemetryPayload {
    uint8_t nodeId;
    float latitude;
    float longitude;
    float ph;
    float tds;
    float turbidity;
    float temperature;
    bool waterPresence;
    bool leakAlert;     
    bool emergencyFlag;
};
#pragma pack(pop)

OneWire oneWire(PIN_TEMP);
DallasTemperature tempSensor(&oneWire);

TaskHandle_t telemetryTaskHandle;
TaskHandle_t aiTaskHandle;

volatile bool global_water_contamination = false;
volatile bool global_leak_alert = false;

// Edge Impulse Audio Buffer Allocation
int16_t *sampleBuffer;

// ==================================================================
// HARDWARE INTERRUPT (Wake on Water Arrival)
// ==================================================================
void IRAM_ATTR waterPresenceISR() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(telemetryTaskHandle, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) { portYIELD_FROM_ISR(); }
}

// ==================================================================
// HELPER FUNCTIONS (Sensors)
// ==================================================================
float getReconstructed5V(uint8_t pin) {
    long sum = 0;
    for(int i = 0; i < 10; i++) {
        sum += analogRead(pin);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    float avgRead = sum / 10.0;
    float esp_voltage = (avgRead * 3.3) / 4095.0; 
    return esp_voltage * V_DIVIDER_RATIO; 
}

float readTurbidityNTU() {
    long sum = 0;
    for (int i = 0; i < 800; i++) {
        sum += analogRead(PIN_TURB);
        vTaskDelay(pdMS_TO_TICKS(1)); 
    }
    
    float avg_analog = sum / 800.0;
    float esp_voltage = avg_analog * (3.3 / 4095.0);
    float sensor_voltage = esp_voltage * V_DIVIDER_RATIO; 
    
    if (sensor_voltage < 0.1) return 0.0;
    if (sensor_voltage >= TURB_CLEAR_VOLTAGE) return 0.0; 

    float ntu = 0;
    if (sensor_voltage < 2.5) {
        ntu = 3000; 
    } else {
        ntu = -1120.4 * pow(sensor_voltage, 2) + 5742.3 * sensor_voltage - 4352.9;
    }
    return (ntu < 0) ? 0 : ntu;
}

// ==================================================================
// CORE 0: EDGE IMPULSE ACOUSTIC AI TASK
// ==================================================================
int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr) {
    numpy::int16_to_float(&sampleBuffer[offset], out_ptr, length);
    return 0;
}

void aiTask(void *pvParameters) {
    sampleBuffer = (int16_t *)malloc(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(int16_t));

    i2s_chan_handle_t rx_handle;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000), 
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO), 
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_SCK,
            .ws   = (gpio_num_t)I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    i2s_channel_init_std_mode(rx_handle, &std_cfg);
    i2s_channel_enable(rx_handle);

    while(true) {
        size_t bytes_read;
        int32_t raw_samples[512]; 
        int sample_index = 0;

        while (sample_index < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
            i2s_channel_read(rx_handle, raw_samples, sizeof(raw_samples), &bytes_read, portMAX_DELAY);
            int samples_read = bytes_read / 4; 
            
            for(int i = 0; i < samples_read && sample_index < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i++) {
                sampleBuffer[sample_index++] = raw_samples[i] >> 14; 
            }
        }

        signal_t signal;
        signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
        signal.get_data = &microphone_audio_signal_get_data;

        ei_impulse_result_t result = {0};
        EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);

        if (r == EI_IMPULSE_OK) {
            bool leak_detected = false;
            
            for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                if (strstr(result.classification[ix].label, "leak") != NULL) {
                    if (result.classification[ix].value > 0.80) { 
                        leak_detected = true;
                    }
                }
            }
            
            if (leak_detected) {
                if (!global_leak_alert) {
                    global_leak_alert = true;
                    Serial.println("\n[!] AUDIO AI: ACOUSTIC LEAK ANOMALY DETECTED [!]");
                    xTaskNotifyGive(telemetryTaskHandle); 
                }
            } else {
                global_leak_alert = false;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==================================================================
// CORE 1: LIVE TELEMETRY TASK
// ==================================================================
void telemetryTask(void *pvParameters) {
    // CRITICAL FIX: Pass -1 as the CS pin to prevent hardware lockup
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, -1);
    LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
    
    if (!LoRa.begin(433E6)) {
        Serial.println(">> CRITICAL: LoRa init failed. Check SPI wiring! <<");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    LoRa.setTxPower(17);

    analogReadResolution(12); 
    tempSensor.begin();
    tempSensor.setWaitForConversion(false); 

    TelemetryPayload payload;
    
    payload.nodeId = NODE_ID;
    payload.latitude = NODE_LATITUDE;
    payload.longitude = NODE_LONGITUDE;

    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));

        bool isWaterPresent = (digitalRead(PIN_WATER) == HIGH);
        payload.waterPresence = isWaterPresent;

        Serial.println("\n------------------------------------");

        if (!isWaterPresent) {
            Serial.println("Status:    NO WATER (Sensors Bypassed)");
            payload.temperature = 0.0;
            payload.ph = 0.0;
            payload.tds = 0.0;
            payload.turbidity = 0.0;
            payload.leakAlert = global_leak_alert; 
            payload.emergencyFlag = payload.leakAlert;
        } 
        else {
            Serial.println("Status:    WATER DETECTED (Scanning...)");
            
            // --- 1. TEMPERATURE ---
            tempSensor.requestTemperatures();
            vTaskDelay(pdMS_TO_TICKS(800)); 
            float rawTemp = tempSensor.getTempCByIndex(0);
            payload.temperature = (rawTemp == DEVICE_DISCONNECTED_C) ? 25.0 : rawTemp;

            // --- 2. pH SENSOR ---
            payload.ph = (3.5 * getReconstructed5V(PIN_PH)) + PH_OFFSET;

            // --- 3. TDS SENSOR ---
            float compCoefficient = 1.0 + 0.02 * (payload.temperature - 25.0);
            float compVoltage = getReconstructed5V(PIN_TDS) / compCoefficient;
            payload.tds = (133.42 * pow(compVoltage, 3) - 255.86 * pow(compVoltage, 2) + 857.39 * compVoltage) * 0.5;
            if (payload.tds < 0) payload.tds = 0; 

            // --- 4. TURBIDITY SENSOR ---
            payload.turbidity = readTurbidityNTU();

            // --- 5. EMERGENCY LOGIC ---
            bool isSewage = (payload.tds >= ALARM_TDS_PPM || payload.turbidity >= ALARM_TURB_NTU || 
                             payload.ph <= ALARM_PH_LOW || payload.ph >= ALARM_PH_HIGH);
            
            payload.leakAlert = global_leak_alert; 
            payload.emergencyFlag = (isSewage || payload.leakAlert); 
        }

        // --- TRANSMIT ---
        LoRa.beginPacket();
        LoRa.write((uint8_t*)&payload, sizeof(TelemetryPayload));
        LoRa.endPacket();

        // --- DASHBOARD ---
        Serial.printf("Node ID:   %d\n", payload.nodeId);
        Serial.printf("Temp:      %.2f °C\n", payload.temperature);
        Serial.printf("pH:        %.2f\n", payload.ph);
        Serial.printf("TDS:       %.0f ppm\n", payload.tds);
        Serial.printf("Turbidity: %.2f NTU\n", payload.turbidity);
        
        Serial.println(">> LoRa Data Transmitted <<");
        
        if (payload.emergencyFlag) {
            Serial.print(">> ALARM ACTIVE: ");
            if (payload.leakAlert) Serial.print("[ACOUSTIC LEAK] ");
            if (payload.waterPresence) Serial.print("[SEWAGE DETECTED] ");
            Serial.println("<<");
        }
    }
}

// ==================================================================
void setup() {
    Serial.begin(115200);
    delay(2000); 
    Serial.println("\n====================================");
    Serial.println("  Jal Rakshak Telemetry Node Booting");
    Serial.println("====================================");

    pinMode(PIN_WATER, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_WATER), waterPresenceISR, RISING);

    xTaskCreatePinnedToCore(aiTask, "AI_Task", 16384, NULL, 1, &aiTaskHandle, 0);
    xTaskCreatePinnedToCore(telemetryTask, "Telemetry", 8192, NULL, 1, &telemetryTaskHandle, 1);
}

void loop() { vTaskDelete(NULL); }