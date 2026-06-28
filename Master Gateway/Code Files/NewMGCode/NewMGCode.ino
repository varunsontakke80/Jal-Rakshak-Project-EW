#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <SPI.h>
#include <LoRa.h>
#include <MQUnifiedsensor.h>
#include <lvgl.h>

// --- LovyanGFX Autodetect ---
#define LGFX_ESP32_S3_BOX_V3
#include <LGFX_AUTODETECT.hpp>
#include <LovyanGFX.hpp>

static LGFX lcd;

// --- Strictly Assigned ESP32-S3 Pins ---
#define LORA_SCK_PIN  12
#define LORA_MISO_PIN 11
#define LORA_MOSI_PIN 14
#define LORA_CS_PIN   42
#define LORA_RST_PIN  39
#define LORA_DIO0_PIN 13

#define PIN_MQ135     9  // ADC1_CH8
#define PIN_MQ136     10 // ADC1_CH9

// --- NEW: BUZZER ALARM PIN ---
#define BUZZER_PIN    21 

// --- Firebase Credentials ---
#define Web_API_KEY "AIzaSyBuMFYY3y5kA0Tp451Ki4eqZ28NhdZA6pw"
#define DATABASE_URL "https://jal-rakshak-indore-default-rtdb.asia-southeast1.firebasedatabase.app"
#define USER_EMAIL "varunsontakke2@gmail.com"
#define USER_PASS "Varuna@#123"

UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);
FirebaseApp firebase_app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database;

bool firebase_initialized = false;

// Firebase Callback
void processData(AsyncResult &aResult) {
  if (!aResult.isResult()) return;
  if (aResult.isError()) {
    Firebase.printf("Firebase Error: %s, code: %d\n", aResult.error().message().c_str(), aResult.error().code());
  }
}

// --- MQ Sensor Objects ---
MQUnifiedsensor MQ135("ESP32", 3.3, 12, PIN_MQ135, "MQ-135");
MQUnifiedsensor MQ136("ESP32", 3.3, 12, PIN_MQ136, "MQ-136");
bool mq_sensors_ready = false;

// ------------------------------------------------------------------
// C-STRUCT PAYLOAD (Matches Pipe Node Exactly)
// ------------------------------------------------------------------
#pragma pack(push, 1)
struct TelemetryPayload {
    float ph;
    float tds;
    float turbidity;
    float temperature;
    bool waterPresence;
    bool leakAlert;
    bool emergencyFlag; 
};
#pragma pack(pop)

TelemetryPayload incomingData;

// --- Global Hardware Objects ---
SPIClass loraSPI(SPI2_HOST); 
String selected_ssid = "";
String wifi_pass = "";

// --- LVGL Screen Objects ---
lv_obj_t * scr_main;
lv_obj_t * scr_wifi_list;
lv_obj_t * scr_wifi_pass;
lv_obj_t * scr_wifi_status;
lv_obj_t * scr_pipe;
lv_obj_t * scr_node_data; 
lv_obj_t * scr_sensor_check;
lv_obj_t * scr_water;

// --- Dynamic LVGL Widgets ---
lv_obj_t * lbl_main_wifi_status;
lv_obj_t * wifi_list;
lv_obj_t * ta_pass;
lv_obj_t * kb_pass;
lv_obj_t * lbl_status;
lv_obj_t * wifi_spinner = NULL;
lv_obj_t * lbl_sensor_status;
lv_obj_t * btn_proceed_test;
lv_obj_t * water_lbl_status;
lv_obj_t * water_bar;
lv_obj_t * btn_test_now;
lv_obj_t * btn_cancel_test;
lv_obj_t * lbl_cancel_test;
lv_obj_t * lbl_node_data; 

lv_timer_t * wifi_timer = NULL;
lv_timer_t * water_timer = NULL;
int water_progress = 0;
int current_listening_node = 0;

// --- Forward Declarations ---
void build_scr_main();
void build_scr_wifi_list();
void build_scr_wifi_pass();
void build_scr_wifi_status();
void build_scr_pipe();
void build_scr_node_data();
void build_scr_sensor_check();
void build_scr_water();
void apply_base_style(lv_obj_t * scr, uint32_t hex_color);

// ==========================================
// LVGL TO LOVYANGFX BRIDGE DRIVERS (v8.3.x)
// ==========================================
static const uint32_t screenWidth  = 320;
static const uint32_t screenHeight = 240;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 30]; 

void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  lcd.startWrite();
  lcd.setAddrWindow(area->x1, area->y1, w, h);
  lcd.writePixels((lgfx::rgb565_t *)&color_p->full, w * h);
  lcd.endWrite();
  lv_disp_flush_ready(disp_drv);
}

void my_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
  uint16_t touchX, touchY;
  bool touched = lcd.getTouch(&touchX, &touchY);
  if (!touched) {
    data->state = LV_INDEV_STATE_REL;
  } else {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = touchX;
    data->point.y = touchY;
  }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  // Initialize Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); // Start silent

  // 1. Init LovyanGFX & LVGL
  lcd.init();
  lcd.setRotation(1); 
  lcd.setBrightness(255);
  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 30);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  // 2. Initialize Internal ADC & MQUnifiedsensor Math Models
  analogReadResolution(12); 
  
  MQ135.setRegressionMethod(1); 
  MQ135.setA(102.2); MQ135.setB(-2.473); 
  MQ135.init(); 
  MQ135.setR0(10.0); 

  MQ136.setRegressionMethod(1); 
  MQ136.setA(40.5); MQ136.setB(-2.15); 
  MQ136.init();
  MQ136.setR0(10.0); 
  
  // 3. Init LoRa with DEDICATED SPI BUS
  loraSPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, -1);
  LoRa.setSPI(loraSPI);
  LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
  
  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa Init Failed! Check connections.");
  }

  // 4. Build All Screens
  build_scr_main();
  build_scr_wifi_list();
  build_scr_wifi_pass();
  build_scr_wifi_status();
  build_scr_pipe();
  build_scr_node_data();
  build_scr_sensor_check();
  build_scr_water();

  lv_scr_load(scr_main);
}

// ==========================================
// MAIN LOOP & LIVE LORA RECEPTION
// ==========================================
void loop() {
  static uint32_t last_tick = millis();
  uint32_t current_tick = millis();
  lv_tick_inc(current_tick - last_tick);
  last_tick = current_tick;

  lv_timer_handler(); 
  delay(5);

  // Maintain Firebase Auth Tasks safely in the background
  firebase_app.loop(); 
  
  // Background LoRa Parsing
  int packetSize = LoRa.parsePacket();
  
  if (packetSize == sizeof(TelemetryPayload)) {
      LoRa.readBytes((uint8_t *)&incomingData, sizeof(TelemetryPayload));

      // 🚨 BUZZER ALARM LOGIC 🚨
      // Sounds the alarm globally if ANY emergency flags are triggered
      if (incomingData.emergencyFlag || incomingData.leakAlert || incomingData.waterPresence) {
          digitalWrite(BUZZER_PIN, HIGH);
      } else {
          digitalWrite(BUZZER_PIN, LOW);
      }
      
      // 1. UPDATE THE LOCAL UI
      if (current_listening_node > 0) {
          char displayBuf[512];
          snprintf(displayBuf, sizeof(displayBuf),
                   "Node %d Live Telemetry:\n\n"
                   "pH Level: %.2f\n"
                   "TDS: %.0f ppm\n"
                   "Turbidity: %.2f NTU\n"
                   "Temperature: %.1f C\n"
                   "Water Presence: %s\n"
                   "Acoustic Leak: %s\n"
                   "EMERGENCY ALERT: %s",
                   current_listening_node,
                   incomingData.ph,
                   incomingData.tds,
                   incomingData.turbidity,
                   incomingData.temperature,
                   incomingData.waterPresence ? "YES (DETECTED)" : "NO",
                   incomingData.leakAlert ? "DETECTED!" : "CLEAR",
                   incomingData.emergencyFlag ? "ACTIVE!" : "NONE"
          );

          lv_label_set_text(lbl_node_data, displayBuf);
          
          if (incomingData.emergencyFlag || incomingData.leakAlert || incomingData.waterPresence) {
              lv_obj_set_style_text_color(lbl_node_data, lv_color_hex(0xFF0000), LV_PART_MAIN); 
          } else {
              lv_obj_set_style_text_color(lbl_node_data, lv_color_hex(0x0056b3), LV_PART_MAIN); 
          }
      }

      // 2. BACKUP TRANSMISSION: Stream to Firebase Database
      if (firebase_initialized && firebase_app.ready()) {
          String basePath = "/nodes/node_1";
          
          Database.set<float>(aClient, basePath + "/ph", incomingData.ph, processData, "ph");
          Database.set<float>(aClient, basePath + "/tds", incomingData.tds, processData, "tds");
          Database.set<float>(aClient, basePath + "/turbidity", incomingData.turbidity, processData, "turb");
          Database.set<float>(aClient, basePath + "/temperature", incomingData.temperature, processData, "temp");
          Database.set<bool>(aClient, basePath + "/waterPresence", incomingData.waterPresence, processData, "wp");
          Database.set<bool>(aClient, basePath + "/leakAlert", incomingData.leakAlert, processData, "la");
          Database.set<bool>(aClient, basePath + "/emergencyFlag", incomingData.emergencyFlag, processData, "ef");
          Database.set<String>(aClient, basePath + "/last_updated", String(millis()), processData, "ts");
      }
  }
}

// ==========================================
// UI DESCRIPTIVE PANEL LAYOUT BUILDERS
// ==========================================
void apply_base_style(lv_obj_t * scr, uint32_t hex_color) {
  lv_obj_set_style_bg_color(scr, lv_color_hex(hex_color), LV_PART_MAIN);
  lv_obj_set_style_text_color(scr, lv_color_hex(0x333333), LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

void build_scr_main() {
  scr_main = lv_obj_create(NULL);
  apply_base_style(scr_main, 0xf0f0f0); 

  lv_obj_t * main_flex = lv_obj_create(scr_main);
  lv_obj_set_size(main_flex, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(main_flex, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(main_flex, 0, LV_PART_MAIN);
  lv_obj_clear_flag(main_flex, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(main_flex, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(main_flex, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(main_flex, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(main_flex, 15, LV_PART_MAIN);

  lv_obj_t * row1 = lv_obj_create(main_flex);
  lv_obj_set_size(row1, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row1, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(row1, 0, LV_PART_MAIN);
  lv_obj_set_layout(row1, LV_LAYOUT_FLEX);
  lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row1, 20, LV_PART_MAIN);

  lv_obj_t * btn1 = lv_btn_create(row1);
  lv_obj_t * lbl_btn1 = lv_label_create(btn1);
  lv_label_set_text(lbl_btn1, "Scan WiFi");
  lv_obj_add_event_cb(btn1, [](lv_event_t * e){
    lv_scr_load(scr_wifi_list);
    lv_label_set_text(lv_obj_get_child(wifi_list, 0), "Scanning..."); 
    int n = WiFi.scanNetworks();
    lv_obj_clean(wifi_list);
    for (int i = 0; i < n; ++i) {
      lv_obj_t * btn = lv_list_add_btn(wifi_list, LV_SYMBOL_WIFI, WiFi.SSID(i).c_str());
      lv_obj_add_event_cb(btn, [](lv_event_t * e){
        lv_obj_t * clicked_btn = (lv_obj_t *)lv_event_get_target(e); 
        selected_ssid = lv_list_get_btn_text(wifi_list, clicked_btn);
        lv_textarea_set_text(ta_pass, "");
        lv_textarea_set_placeholder_text(ta_pass, String("Password for " + selected_ssid).c_str());
        lv_scr_load(scr_wifi_pass);
      }, LV_EVENT_CLICKED, NULL);
    }
  }, LV_EVENT_CLICKED, NULL);

  lbl_main_wifi_status = lv_label_create(row1);
  lv_label_set_text(lbl_main_wifi_status, "no wifi connected");
  lv_obj_set_style_text_color(lbl_main_wifi_status, lv_color_hex(0xFF0000), LV_PART_MAIN);

  lv_obj_t * row2 = lv_obj_create(main_flex);
  lv_obj_set_size(row2, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row2, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(row2, 0, LV_PART_MAIN);
  lv_obj_set_layout(row2, LV_LAYOUT_FLEX);
  lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row2, 20, LV_PART_MAIN);

  lv_obj_t * btn2 = lv_btn_create(row2);
  lv_obj_t * lbl_btn2 = lv_label_create(btn2);
  lv_label_set_text(lbl_btn2, "Pipe node");
  lv_obj_add_event_cb(btn2, [](lv_event_t * e){ lv_scr_load(scr_pipe); }, LV_EVENT_CLICKED, NULL);

  lv_obj_t * lbl_desc2 = lv_label_create(row2);
  lv_label_set_text(lbl_desc2, "select the pipe node");
  lv_obj_set_style_text_color(lbl_desc2, lv_color_hex(0x0056b3), LV_PART_MAIN);

  lv_obj_t * row3 = lv_obj_create(main_flex);
  lv_obj_set_size(row3, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row3, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(row3, 0, LV_PART_MAIN);
  lv_obj_set_layout(row3, LV_LAYOUT_FLEX);
  lv_obj_set_flex_align(row3, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row3, 20, LV_PART_MAIN);

  lv_obj_t * btn3 = lv_btn_create(row3);
  lv_obj_t * lbl_btn3 = lv_label_create(btn3);
  lv_label_set_text(lbl_btn3, "Water test");
  lv_obj_add_event_cb(btn3, [](lv_event_t * e){ lv_scr_load(scr_sensor_check); }, LV_EVENT_CLICKED, NULL);

  lv_obj_t * lbl_desc3 = lv_label_create(row3);
  lv_label_set_text(lbl_desc3, "test the water quality");
  lv_obj_set_style_text_color(lbl_desc3, lv_color_hex(0x0056b3), LV_PART_MAIN);
}

void build_scr_wifi_list() {
  scr_wifi_list = lv_obj_create(NULL);
  apply_base_style(scr_wifi_list, 0xf0f0f0);

  lv_obj_t * btn_back1 = lv_btn_create(scr_wifi_list);
  lv_obj_t * lbl_back1 = lv_label_create(btn_back1);
  lv_label_set_text(lbl_back1, LV_SYMBOL_LEFT " Back to menu");
  lv_obj_add_event_cb(btn_back1, [](lv_event_t * e){ lv_scr_load(scr_main); }, LV_EVENT_CLICKED, NULL);

  wifi_list = lv_list_create(scr_wifi_list);
  lv_obj_set_size(wifi_list, 280, 180);
  lv_obj_align(wifi_list, LV_ALIGN_BOTTOM_MID, 0, -5);
  lv_list_add_text(wifi_list, "Scanning...");
}

void build_scr_wifi_pass() {
  scr_wifi_pass = lv_obj_create(NULL);
  apply_base_style(scr_wifi_pass, 0xf0f0f0);

  lv_obj_t * btn_back_wifi = lv_btn_create(scr_wifi_pass);
  lv_obj_t * lbl_back_wifi = lv_label_create(btn_back_wifi);
  lv_label_set_text(lbl_back_wifi, LV_SYMBOL_LEFT " Back");
  lv_obj_add_event_cb(btn_back_wifi, [](lv_event_t * e){ lv_scr_load(scr_wifi_list); }, LV_EVENT_CLICKED, NULL);

  lv_obj_t * btn_connect = lv_btn_create(scr_wifi_pass);
  lv_obj_align(btn_connect, LV_ALIGN_TOP_RIGHT, -10, 5);
  lv_obj_set_style_bg_color(btn_connect, lv_color_hex(0x28a745), LV_PART_MAIN); 
  lv_obj_t * lbl_connect = lv_label_create(btn_connect);
  lv_label_set_text(lbl_connect, "CONNECT");

  ta_pass = lv_textarea_create(scr_wifi_pass);
  lv_obj_set_width(ta_pass, 300);
  lv_obj_align(ta_pass, LV_ALIGN_TOP_MID, 0, 45);
  lv_textarea_set_password_mode(ta_pass, true);
  lv_textarea_set_one_line(ta_pass, true);

  kb_pass = lv_keyboard_create(scr_wifi_pass);
  lv_keyboard_set_textarea(kb_pass, ta_pass);
  lv_obj_set_size(kb_pass, 320, 140);
  lv_obj_align(kb_pass, LV_ALIGN_BOTTOM_MID, 0, 0);

  lv_obj_add_event_cb(btn_connect, [](lv_event_t * e){
      wifi_pass = lv_textarea_get_text(ta_pass);
      lv_scr_load(scr_wifi_status);
      
      lv_label_set_text(lbl_status, "Connecting to SSID...");
      if(wifi_spinner == NULL) {
        wifi_spinner = lv_spinner_create(scr_wifi_status, 1000, 60);
        lv_obj_set_size(wifi_spinner, 50, 50);
        lv_obj_align(wifi_spinner, LV_ALIGN_CENTER, 0, 50);
      }

      WiFi.begin(selected_ssid.c_str(), wifi_pass.c_str());

      wifi_timer = lv_timer_create([](lv_timer_t * t){
        if(WiFi.status() == WL_CONNECTED) {
          lv_label_set_text(lbl_status, "wifi connected");
          lv_label_set_text(lbl_main_wifi_status, "wifi connected");
          lv_obj_set_style_text_color(lbl_main_wifi_status, lv_color_hex(0x00AA00), LV_PART_MAIN);
          if(wifi_spinner != NULL) { lv_obj_del(wifi_spinner); wifi_spinner = NULL; }
          lv_timer_del(t);

          if (!firebase_initialized) {
              ssl_client.setInsecure();
              ssl_client.setConnectionTimeout(1000);
              ssl_client.setHandshakeTimeout(5);
              initializeApp(aClient, firebase_app, getAuth(user_auth), processData, "authTask");
              firebase_app.getApp<RealtimeDatabase>(Database);
              Database.url(DATABASE_URL);
              firebase_initialized = true;
          }

        } else if (WiFi.status() == WL_CONNECT_FAILED || WiFi.status() == WL_NO_SSID_AVAIL) {
          lv_label_set_text(lbl_status, "Failed to connect.");
          lv_label_set_text(lbl_main_wifi_status, "no wifi connected");
          lv_obj_set_style_text_color(lbl_main_wifi_status, lv_color_hex(0xFF0000), LV_PART_MAIN);
          if(wifi_spinner != NULL) { lv_obj_del(wifi_spinner); wifi_spinner = NULL; }
          lv_timer_del(t);
        }
      }, 500, NULL);
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_add_event_cb(kb_pass, [](lv_event_t * e){
    if(lv_event_get_code(e) == LV_EVENT_CANCEL) {
      lv_scr_load(scr_wifi_list);
    }
  }, LV_EVENT_ALL, NULL);
}

void build_scr_wifi_status() {
  scr_wifi_status = lv_obj_create(NULL);
  apply_base_style(scr_wifi_status, 0xdddddd); 

  lbl_status = lv_label_create(scr_wifi_status);
  lv_label_set_text(lbl_status, "Pending...");
  lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, -20);

  lv_obj_t * btn_ok = lv_btn_create(scr_wifi_status);
  lv_obj_t * lbl_ok = lv_label_create(btn_ok);
  lv_label_set_text(lbl_ok, "Back to menu");
  lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_add_event_cb(btn_ok, [](lv_event_t * e){ lv_scr_load(scr_main); }, LV_EVENT_CLICKED, NULL);
}

void build_scr_pipe() {
  scr_pipe = lv_obj_create(NULL);
  apply_base_style(scr_pipe, 0xf0f0f0);

  lv_obj_t * btn_back2 = lv_btn_create(scr_pipe);
  lv_obj_t * lbl_back2 = lv_label_create(btn_back2);
  lv_label_set_text(lbl_back2, LV_SYMBOL_LEFT " Back to menu");
  lv_obj_add_event_cb(btn_back2, [](lv_event_t * e){ lv_scr_load(scr_main); }, LV_EVENT_CLICKED, NULL);

  lv_obj_t * pipe_list = lv_list_create(scr_pipe);
  lv_obj_set_size(pipe_list, 280, 180);
  lv_obj_align(pipe_list, LV_ALIGN_BOTTOM_MID, 0, -5);

  for(int i=1; i<=4; i++) {
    lv_obj_t * btn_node = lv_list_add_btn(pipe_list, LV_SYMBOL_DIRECTORY, String("Node " + String(i)).c_str());
    btn_node->user_data = (void*)i; 
    
    lv_obj_add_event_cb(btn_node, [](lv_event_t * e){
      int node_id = (int)e->target->user_data;
      current_listening_node = node_id; 
      
      String initial_text = "Listening to LoRa Node " + String(node_id) + "...\nWaiting for packet.";
      lv_label_set_text(lbl_node_data, initial_text.c_str());
      lv_obj_set_style_text_color(lbl_node_data, lv_color_hex(0x333333), LV_PART_MAIN); 
      
      lv_scr_load(scr_node_data);
    }, LV_EVENT_CLICKED, NULL);
  }
}

void build_scr_node_data() {
  scr_node_data = lv_obj_create(NULL);
  apply_base_style(scr_node_data, 0xf0f0f0);

  lv_obj_t * btn_back_data = lv_btn_create(scr_node_data);
  lv_obj_t * lbl_back_data = lv_label_create(btn_back_data);
  lv_label_set_text(lbl_back_data, LV_SYMBOL_LEFT " Back to Nodes");
  
  lv_obj_add_event_cb(btn_back_data, [](lv_event_t * e){ 
    current_listening_node = 0; 
    lv_scr_load(scr_pipe); 
  }, LV_EVENT_CLICKED, NULL);

  lbl_node_data = lv_label_create(scr_node_data);
  lv_label_set_text(lbl_node_data, "Loading Data...");
  lv_obj_align(lbl_node_data, LV_ALIGN_LEFT_MID, 20, 0); 
}

void build_scr_sensor_check() {
  scr_sensor_check = lv_obj_create(NULL);
  apply_base_style(scr_sensor_check, 0xf0f0f0);

  lv_obj_t * btn_back_chk = lv_btn_create(scr_sensor_check);
  lv_obj_t * lbl_back_chk = lv_label_create(btn_back_chk);
  lv_label_set_text(lbl_back_chk, LV_SYMBOL_LEFT " Back to menu");
  lv_obj_add_event_cb(btn_back_chk, [](lv_event_t * e){ lv_scr_load(scr_main); }, LV_EVENT_CLICKED, NULL);

  lv_obj_t * chk_flex = lv_obj_create(scr_sensor_check);
  lv_obj_set_size(chk_flex, LV_PCT(100), 180);
  lv_obj_align(chk_flex, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(chk_flex, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(chk_flex, 0, LV_PART_MAIN);
  lv_obj_set_layout(chk_flex, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(chk_flex, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(chk_flex, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lbl_sensor_status = lv_label_create(chk_flex);
  lv_label_set_text(lbl_sensor_status, "Verify MQ Sensors on GPIO 9 & 10");
  lv_obj_set_style_text_align(lbl_sensor_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

  lv_obj_t * btn_scan = lv_btn_create(chk_flex);
  lv_obj_t * lbl_scan = lv_label_create(btn_scan);
  lv_label_set_text(lbl_scan, "Check Internal ADC");

  btn_proceed_test = lv_btn_create(chk_flex);
  lv_obj_t * lbl_proceed = lv_label_create(btn_proceed_test);
  lv_label_set_text(lbl_proceed, "Proceed to Test");
  lv_obj_add_flag(btn_proceed_test, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_event_cb(btn_scan, [](lv_event_t * e){
    int val1 = analogRead(PIN_MQ135);
    int val2 = analogRead(PIN_MQ136);

    if ((val1 > 10 && val1 < 4090) && (val2 > 10 && val2 < 4090)) {
        mq_sensors_ready = true;
        lv_label_set_text(lbl_sensor_status, "ADC OK!\nVoltages stable. Heater active.\nReady to proceed.");
        lv_obj_set_style_text_color(lbl_sensor_status, lv_color_hex(0x00AA00), LV_PART_MAIN);
    } else {
        mq_sensors_ready = false;
        lv_label_set_text(lbl_sensor_status, "ERROR: Invalid ADC Data.\nCheck 10k/20k Voltage Divider!");
        lv_obj_set_style_text_color(lbl_sensor_status, lv_color_hex(0xFF0000), LV_PART_MAIN);
    }
    
    lv_obj_clear_flag(btn_proceed_test, LV_OBJ_FLAG_HIDDEN); 
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_add_event_cb(btn_proceed_test, [](lv_event_t * e){ lv_scr_load(scr_water); }, LV_EVENT_CLICKED, NULL);
}

void build_scr_water() {
  scr_water = lv_obj_create(NULL);
  apply_base_style(scr_water, 0xf0f0f0);

  lv_obj_t * btn_back3 = lv_btn_create(scr_water);
  lv_obj_t * lbl_back3 = lv_label_create(btn_back3);
  lv_label_set_text(lbl_back3, LV_SYMBOL_LEFT " Back to menu");

  lv_obj_t * water_flex = lv_obj_create(scr_water);
  lv_obj_set_size(water_flex, LV_PCT(100), 180);
  lv_obj_align(water_flex, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(water_flex, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(water_flex, 0, LV_PART_MAIN);
  lv_obj_set_layout(water_flex, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(water_flex, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(water_flex, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  water_lbl_status = lv_label_create(water_flex);
  lv_label_set_text(water_lbl_status, "connect the water cup with test water");
  
  btn_test_now = lv_btn_create(water_flex);
  lv_obj_t * lbl_test_now = lv_label_create(btn_test_now);
  lv_label_set_text(lbl_test_now, "Test now");

  water_bar = lv_bar_create(water_flex);
  lv_obj_set_size(water_bar, 200, 20);
  lv_bar_set_value(water_bar, 0, LV_ANIM_OFF);
  lv_obj_add_flag(water_bar, LV_OBJ_FLAG_HIDDEN);

  btn_cancel_test = lv_btn_create(water_flex);
  lbl_cancel_test = lv_label_create(btn_cancel_test);
  lv_label_set_text(lbl_cancel_test, "Cancel");

  auto reset_water_screen = [](lv_event_t * e){
    if(water_timer != NULL) { lv_timer_del(water_timer); water_timer = NULL; }
    lv_bar_set_value(water_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(water_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(btn_test_now, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl_cancel_test, "Cancel");
    lv_label_set_text(water_lbl_status, "connect the water cup with test water");
    lv_obj_set_style_text_color(water_lbl_status, lv_color_hex(0x333333), LV_PART_MAIN); 
    lv_scr_load(scr_main);
  };
  lv_obj_add_event_cb(btn_back3, reset_water_screen, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(btn_cancel_test, reset_water_screen, LV_EVENT_CLICKED, NULL);

  lv_obj_add_event_cb(btn_test_now, [](lv_event_t * e){
    water_progress = 0;
    lv_bar_set_value(water_bar, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(water_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(water_lbl_status, "testing water...");

    water_timer = lv_timer_create([](lv_timer_t * t){
      water_progress += 5;
      lv_bar_set_value(water_bar, water_progress, LV_ANIM_OFF);

      if(water_progress >= 100) {
        lv_timer_del(t);
        water_timer = NULL;

        MQ135.update();
        MQ136.update();
        
        float nh3_ppm = MQ135.readSensor();
        float h2s_ppm = MQ136.readSensor();

        String status = "";
        uint32_t color = 0x333333;
        
        if (nh3_ppm >= 10.0 || h2s_ppm >= 2.0) {
          status = "Severe Sewage Contaminated!";
          color = 0xFF0000;
        } else if (nh3_ppm >= 2.0 || h2s_ppm >= 0.5) {
          status = "Mild Sewage Contaminated!";
          color = 0xFF8800; 
        } else {
          status = "Safe";
          color = 0x00AA00; 
        }

        char result_buf[256];
        snprintf(result_buf, sizeof(result_buf), 
                 "Status: %s\nAmmonia: %.2f PPM\nH2S: %.2f PPM", 
                 status.c_str(), nh3_ppm, h2s_ppm);

        lv_label_set_text(water_lbl_status, result_buf);
        lv_obj_set_style_text_color(water_lbl_status, lv_color_hex(color), LV_PART_MAIN);

        lv_obj_add_flag(btn_test_now, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(water_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_cancel_test, "Back to menu");
      }
    }, 200, NULL);
  }, LV_EVENT_CLICKED, NULL);
}