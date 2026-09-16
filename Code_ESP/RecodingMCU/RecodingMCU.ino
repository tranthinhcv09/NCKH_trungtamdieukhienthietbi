#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>         
#include <esp_arduino_version.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "driver/i2s.h"        

// ===== CẤU HÌNH GPIO =====
#define ESP_NOW_BUTTON_PIN 4   // Nút bấm Toggle (Chân GPIO4)
#define SD_CS              5   // Chân CS Thẻ nhớ SD (SPI)
#define STATUS_LED         2   // LED tích hợp DevKit V1

// INMP441 Microphone Pins (I2S RX)
#define MIC_I2S_PORT       I2S_NUM_0
#define MIC_I2S_SCK        32
#define MIC_I2S_WS         25
#define MIC_I2S_SD         33

#define SAMPLE_RATE        16000
#define RECORD_SEC         600

// ===== ESP-NOW PACKET DEFINITIONS =====
#define MSG_TYPE_TEXT 0
#define MSG_TYPE_WIFI 0xA1

typedef struct {
    uint8_t type;     // MSG_TYPE_TEXT = 0
    char text[240];
} TextMessagePacket;

typedef struct {
    uint8_t type;     // MSG_TYPE_WIFI = 0xA1
    char ssid[33];
    char password[65];
    char mqtt_server[40];
} WifiMessagePacket;

// ===== STATE MACHINE & ENUM =====
enum SystemState {
  STATE_INIT,
  STATE_ESPNOW_CONFIG,
  STATE_WIFI_CONNECTING,
  STATE_IDLE,
  STATE_RECORDING,
  STATE_UPLOADING
};

enum MqttCommand {
  CMD_NONE,
  CMD_START_RECORD,
  CMD_STOP_RECORD
};

// ===== GLOBAL VARIABLES & HANDLES =====
Preferences preferences;
SystemState currentState = STATE_INIT;
volatile MqttCommand currentCommand = CMD_NONE;
// [FIXED] Không cần i2s_chan_handle_t nữa vì dùng driver cũ (i2s_driver_install/i2s_read)

char ssid[33]       = "K02";
char password[65]   = "88888888";
char mqttServer[40] = "192.168.1.22";
const int serverPort    = 5000;
const char* uploadPath  = "/voice";
const char* WAV_FILE    = "/rec.wav";
const char* statusTopic = "esp32/status";

bool espNowConfigReceived = false;
WifiMessagePacket espNowConfig = {};

// ===== ESP-NOW STATE =====
uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool espNowReady = false;
uint8_t peerMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  
bool peerKnown = false;    

unsigned long lastEspNowRetry = 0;
const unsigned long ESPNOW_RETRY_INTERVAL_MS = 10000;

volatile bool espNowSendResultReady = false;
volatile bool espNowSendSuccess     = false;

// BIẾN QUẢN LÝ TOGGLE NÚT BẤM
bool inConfigMode = false;
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ===== HELPER KIỂM TRA SD =====
bool isSDAvailable() {
  if (SD.cardType() == CARD_NONE) {
    return false;
  }
  return true;
}

// ===== CHUYỂN KÊNH WI-FI =====
void setWifiChannel(uint8_t channel) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(false, false);
    delay(100);                    
  }

  WiFi.mode(WIFI_STA);
  esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

  if (err == ESP_OK) {
    Serial.printf("📡 Đã chuyển sang Kênh Wi-Fi/ESP-NOW: %d\n", channel);
  } else {
    Serial.printf("❌ LỖI đổi kênh sang %d!\n", channel);
  }
}

// ===== HELPERS WAV =====
void writeLE16(uint8_t *b, uint16_t v) { b[0] = v & 0xFF; b[1] = v >> 8; }
void writeLE32(uint8_t *b, uint32_t v) {
    b[0] = v & 0xFF; b[1] = (v >> 8); b[2] = (v >> 16); b[3] = (v >> 24);
}

void makeWavHeader(uint8_t *h, uint32_t dataSize) {
  memcpy(h,      "RIFF", 4);
  writeLE32(h + 4,  36 + dataSize);
  memcpy(h + 8,  "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  writeLE32(h + 16, 16);
  writeLE16(h + 20, 1);           // PCM
  writeLE16(h + 22, 1);           // Mono
  writeLE32(h + 24, SAMPLE_RATE);
  writeLE32(h + 28, SAMPLE_RATE * 2); 
  writeLE16(h + 32, 2);           
  writeLE16(h + 34, 16);          
  memcpy(h + 36, "data", 4);
  writeLE32(h + 40, dataSize);
}

// ===== NVS MANAGEMENT =====
void loadConfigNVS() {
  preferences.begin("settings", true);
  String savedSSID = preferences.getString("ssid");
  String savedPass = preferences.getString("pass");
  String savedMqtt = preferences.getString("mqtt");
  preferences.end();

  if (savedSSID.length() > 0) savedSSID.toCharArray(ssid, sizeof(ssid));
  if (savedPass.length() > 0) savedPass.toCharArray(password, sizeof(password));
  if (savedMqtt.length() > 0) savedMqtt.toCharArray(mqttServer, sizeof(mqttServer));

  Serial.printf("📂 NVS -> SSID: %s | PASS: %s | MQTT IP: %s\n", ssid, password, mqttServer);
}

void saveConfigNVS(const char* newSSID, const char* newPass, const char* newMqtt) {
  preferences.begin("settings", false);
  preferences.putString("ssid", newSSID);
  preferences.putString("pass", newPass);
  preferences.putString("mqtt", newMqtt);
  preferences.end();
  Serial.printf("💾 NVS -> SSID: %s | PASS: %s | MQTT IP: %s\n", newSSID, newPass, newMqtt);
  Serial.println("💾 Đã lưu cấu hình NVS mới!");
}

void handleCommand(const String &message) { 
  if (message == "haVcKWfSpcN") { 
    currentCommand = CMD_START_RECORD; 
  } else if (message == "fND KWfSpcN") { 
    currentCommand = CMD_STOP_RECORD; 
  } 
}

// ===== ESP-NOW CALLBACKS & MESSAGING =====
void addEspNowPeerIfNeeded(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0; 
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
#else
void onEspNowDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
  espNowSendSuccess = (status == ESP_NOW_SEND_SUCCESS);
  espNowSendResultReady = true;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *senderMac = info->src_addr;
#else
void onEspNowDataRecv(const uint8_t *mac_addr, const uint8_t *data, int len) {
  const uint8_t *senderMac = mac_addr;
#endif
  if (len < 1) return;

  uint8_t type = data[0];

  if (type == MSG_TYPE_WIFI && len == sizeof(WifiMessagePacket)) {
    memcpy(&espNowConfig, data, sizeof(WifiMessagePacket));
    espNowConfig.ssid[sizeof(espNowConfig.ssid) - 1] = '\0';
    espNowConfig.password[sizeof(espNowConfig.password) - 1] = '\0';
    espNowConfig.mqtt_server[sizeof(espNowConfig.mqtt_server) - 1] = '\0';
    espNowConfigReceived = true;
    Serial.println("📩 Đã nhận WiFi Credentials qua ESP-NOW từ ESP32-S3!");
  } 
  else if (type == MSG_TYPE_TEXT && len == sizeof(TextMessagePacket)) {
    TextMessagePacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    pkt.text[sizeof(pkt.text) - 1] = '\0';

    memcpy(peerMac, senderMac, 6);
    peerKnown = true;
    addEspNowPeerIfNeeded(peerMac);

    String message = String(pkt.text);
    message.trim();
    Serial.printf("📩 ESP-NOW TEXT: %s\n", message.c_str());
    handleCommand(message);
  }
  else {
    Serial.printf("⚠️ Gói ESP-NOW không khớp cấu hình (Type: 0x%02X, Len: %d)\n", type, len);
  }
}

bool trySendEspNow(const char* text) {
  if (!espNowReady) return false;
  
  TextMessagePacket pkt;
  pkt.type = MSG_TYPE_TEXT;
  strncpy(pkt.text, text, sizeof(pkt.text) - 1);
  pkt.text[sizeof(pkt.text) - 1] = '\0';

  espNowSendResultReady = false;
  espNowSendSuccess = false;

  if (esp_now_send(peerMac, (uint8_t*)&pkt, sizeof(pkt)) != ESP_OK) return false;

  unsigned long waitStart = millis();
  while (!espNowSendResultReady && millis() - waitStart < 300) delay(2);
  return espNowSendSuccess;
}

bool mqttPublishText(const char* text) {
  if (!mqttClient.connected()) return false;
  return mqttClient.publish(statusTopic, text);
}

void sendStatusText(const char* text) {
  if (!trySendEspNow(text)) {
    mqttPublishText(text);
  }
}

bool setupEspNow() {
  esp_now_deinit();
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(onEspNowDataRecv);
  esp_now_register_send_cb(onEspNowDataSent);
  addEspNowPeerIfNeeded(broadcastMac);
  memcpy(peerMac, broadcastMac, 6);
  peerKnown = false;
  return true;
}

// ===== MQTT =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  message.trim();
  handleCommand(message);
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;
  static unsigned long lastReconnectAttempt = 0;
  if (millis() - lastReconnectAttempt > 5000) {
    lastReconnectAttempt = millis();
    String clientId = "ESP32DevKitV1-" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("✅ MQTT connected!");
      mqttClient.subscribe("esp32/audio");
    }
  }
}

// ===== MICROPHONE (INMP441) — [FIXED] dùng driver I2S cũ, đã kiểm chứng hoạt động =====
void initMicrophone() {
  i2s_config_t i2s_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 512,
    .use_apll             = false
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num   = MIC_I2S_SCK,
    .ws_io_num    = MIC_I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_I2S_SD
  };

  esp_err_t err1 = i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL);
  esp_err_t err2 = i2s_set_pin(MIC_I2S_PORT, &pin_config);

  if (err1 != ESP_OK || err2 != ESP_OK) {
    Serial.printf("❌ Lỗi khởi tạo I2S cho mic INMP441! (install=%d, set_pin=%d)\n", err1, err2);
  } else {
    Serial.println("✅ Mic INMP441 (I2S legacy driver) đã sẵn sàng.");
  }
}

void recordAudioToSD() {
  if (!isSDAvailable()) {
    Serial.println("❌ Thẻ nhớ chưa sẵn sàng, thử khởi tạo lại...");
    if (!SD.begin(SD_CS) || SD.cardType() == CARD_NONE) {
      sendStatusText("error:sd_not_ready");
      return;
    }
  }

  File file = SD.open(WAV_FILE, FILE_WRITE);
  if (!file) {
    sendStatusText("error:sd_open_failed");
    return;
  }
  uint8_t header[44];
  makeWavHeader(header, 0);
  file.write(header, 44);

  digitalWrite(STATUS_LED, HIGH);
  sendStatusText("recording_started");

  size_t bytesRead = 0;
  int32_t i2s_buf[256];   // Buffer đọc dữ liệu 32-bit từ I2S
  int16_t sd_buf[512];    // Buffer nén về 16-bit ghi ra SD
  int sd_buf_idx = 0;
  uint32_t totalSamples = 0;
  unsigned long startTime = millis();

  while (currentCommand != CMD_STOP_RECORD && (millis() - startTime < RECORD_SEC * 1000)) {
    // [FIXED] Đọc dữ liệu mẫu 32-bit bằng i2s_read() (driver cũ) thay vì i2s_channel_read()
    if (i2s_read(MIC_I2S_PORT, i2s_buf, sizeof(i2s_buf), &bytesRead, pdMS_TO_TICKS(10)) == ESP_OK && bytesRead > 0) {
      int samples = bytesRead / 4;
      for (int i = 0; i < samples; i++) {
        // Dịch bit lấy 16-bit cao (chuẩn INMP441)
        sd_buf[sd_buf_idx++] = (int16_t)(i2s_buf[i] >> 16);
        totalSamples++;
        if (sd_buf_idx >= 512) {
          file.write((uint8_t*)sd_buf, sizeof(sd_buf));
          sd_buf_idx = 0;
        }
      }
    }
    mqttClient.loop();
  }

  // Ghi các mẫu còn dư trong buffer
  if (sd_buf_idx > 0) {
    file.write((uint8_t*)sd_buf, sd_buf_idx * 2);
  }

  uint32_t totalDataBytes = totalSamples * 2;
  file.seek(0);
  makeWavHeader(header, totalDataBytes);
  file.write(header, 44);
  file.close();

  digitalWrite(STATUS_LED, LOW);
  sendStatusText("recording_stopped");
}

bool uploadFileHTTP() {
  if (WiFi.status() != WL_CONNECTED) return false;
  File file = SD.open(WAV_FILE, FILE_READ);
  if (!file) return false;

  WiFiClient client;
  if (!client.connect(mqttServer, serverPort)) {
    file.close();
    return false;
  }

  String boundary = "----ESP32Boundary" + String(millis());
  String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"rec.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";
  uint32_t contentLength = head.length() + file.size() + tail.length();

  client.print(String("POST ") + uploadPath + " HTTP/1.1\r\nHost: " + mqttServer + "\r\nContent-Type: multipart/form-data; boundary=" + boundary + "\r\nContent-Length: " + String(contentLength) + "\r\nConnection: close\r\n\r\n");
  client.print(head);

  uint8_t buf[512];
  while (file.available()) {
    size_t len = file.read(buf, sizeof(buf));
    client.write(buf, len);
  }
  client.print(tail);
  file.close();

  unsigned long waitStart = millis();
  while (client.connected() && !client.available() && millis() - waitStart < 10000) delay(10);
  bool ok = false;
  if (client.available()) {
    String statusLine = client.readStringUntil('\n');
    ok = (statusLine.indexOf(" 200") > 0) || (statusLine.indexOf(" 201") > 0);
  }
  client.stop();
  if (ok) sendStatusText("upload_done");
  else sendStatusText("error:upload_failed");
  return ok;
}

// ===== BUTTON TOGGLE =====
void handleButtonToggle() {
  int reading = digitalRead(ESP_NOW_BUTTON_PIN);
  if (reading != lastButtonState) lastDebounceTime = millis();

  if ((millis() - lastDebounceTime) > debounceDelay) {
    static int buttonState = HIGH;
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        inConfigMode = !inConfigMode;
        if (inConfigMode) {
          Serial.println("🔘 Nút bấm: VÀO CHẾ ĐỘ NHẬN CONFIG ESP-NOW (Kênh 1)");
          setWifiChannel(1);
          espNowConfigReceived = false;
          currentState = STATE_ESPNOW_CONFIG;
        } else {
          Serial.println("🔘 Nút bấm: THOÁT CẤU HÌNH -> Thử kết nối WiFi");
          digitalWrite(STATUS_LED, LOW);
          currentState = STATE_WIFI_CONNECTING;
        }
      }
    }
  }
  lastButtonState = reading;
}

// ===== SETUP & LOOP =====
void setup() {
  Serial.begin(115200);
  pinMode(ESP_NOW_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  randomSeed(esp_random());

  if (!SD.begin(SD_CS)) {
    Serial.println("❌ Khởi tạo Thẻ nhớ SD thất bại hoặc không tìm thấy thẻ!");
  } else {
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
      Serial.println("❌ Đã nhận diện Module SD nhưng KHÔNG CÓ THẺ NHỚ cắm vào!");
    } else {
      Serial.print("✅ Thẻ nhớ SD đã sẵn sàng! Loại thẻ: ");
      if (cardType == CARD_MMC) Serial.println("MMC");
      else if (cardType == CARD_SD) Serial.println("SDSC");
      else if (cardType == CARD_SDHC) Serial.println("SDHC");
      else Serial.println("UNKNOWN");

      uint64_t cardSize = SD.cardSize() / (1024 * 1024);
      Serial.printf("💾 Dung lượng thẻ nhớ: %llu MB\n", cardSize);
    }
  }

  loadConfigNVS();
  initMicrophone();

  mqttClient.setServer(mqttServer, 1883);
  mqttClient.setCallback(mqttCallback);

  WiFi.mode(WIFI_STA);

  espNowReady = setupEspNow();
  if (!espNowReady) lastEspNowRetry = millis();

  currentState = STATE_WIFI_CONNECTING;
}

void loop() {
  handleButtonToggle();

  if (!espNowReady && millis() - lastEspNowRetry >= ESPNOW_RETRY_INTERVAL_MS) {
    lastEspNowRetry = millis();
    espNowReady = setupEspNow();
  }

  switch (currentState) {
    case STATE_ESPNOW_CONFIG: {
      digitalWrite(STATUS_LED, (millis() / 200) % 2);
      if (espNowConfigReceived) {
        saveConfigNVS(espNowConfig.ssid, espNowConfig.password, espNowConfig.mqtt_server);
        strcpy(ssid, espNowConfig.ssid);
        strcpy(password, espNowConfig.password);
        strcpy(mqttServer, espNowConfig.mqtt_server);
        mqttClient.setServer(mqttServer, 1883);

        espNowConfigReceived = false;
        inConfigMode = false;
        digitalWrite(STATUS_LED, LOW);
        currentState = STATE_WIFI_CONNECTING;
      }
      break;
    }

    case STATE_WIFI_CONNECTING: {
      static bool connectStarted = false;
      static unsigned long connectStartTime = 0;

      if (!connectStarted) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, password);
        connectStarted = true;
        connectStartTime = millis();
        Serial.printf("🔄 Đang kết nối tới Wi-Fi: %s ...\n", ssid);
      }

      if (WiFi.status() == WL_CONNECTED) {
        connectStarted = false;
        Serial.printf("✅ Đã kết nối Wi-Fi thành công! IP: %s | Kênh RF: %d\n", WiFi.localIP().toString().c_str(), WiFi.channel());
        currentState = STATE_IDLE;
      } else if (millis() - connectStartTime > 10000) {
        connectStarted = false;
        Serial.println("⚠️ Rớt/không kết nối được Wi-Fi Router, chuyển sang STATE_IDLE để chờ ESP-NOW.");
        currentState = STATE_IDLE;
      }
      break;
    }

    case STATE_IDLE: {
      if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) reconnectMQTT();
        mqttClient.loop();
      }
      if (currentCommand == CMD_START_RECORD) currentState = STATE_RECORDING;
      break;
    }

    case STATE_RECORDING: {
      recordAudioToSD();
      currentCommand = CMD_NONE;
      currentState = STATE_UPLOADING;
      break;
    }

    case STATE_UPLOADING: {
      uploadFileHTTP();
      currentState = STATE_IDLE;
      break;
    }

    default:
      break;
  }

  delay(10);
}
