// ============================================================
//   esp32devkitv1.ino – ESP32 DevKit V1 (Arduino)
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_arduino_version.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <SD.h>
#include "driver/i2s.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Preferences.h>

#define ESP_NOW_BUTTON_PIN 4
const int espNowBtnPin = ESP_NOW_BUTTON_PIN;

// ======================= CONFIG & NVS =======================
char ssid[33];
char password[65];
char mqttServer[40];
const int   serverPort  = 5000;
const char* uploadPath  = "/voice";

Preferences preferences;

struct WifiCredentialsMessage {
    char ssid[33];
    char password[65];
    char mqtt_server[40];
};

bool espNowEnabled       = false;
bool espNowConfigReceived = false;
WifiCredentialsMessage espNowConfig = {};

WiFiClient   espClient;
PubSubClient client(espClient);

// ======================= ENUM COMMANDS =======================
enum MqttCommand {
    CMD_NONE = 0,
    CMD_START_RECORD,
    CMD_STOP_RECORD
};

volatile MqttCommand currentCmd = CMD_NONE;

#define SD_CS    5
#define LED      2
#define WAV_FILE "/rec.wav"

// ======================= I2S CONFIG MIC =======================
#define I2S_PORT I2S_NUM_0
#define I2S_SCK  32
#define I2S_WS   25
#define I2S_SD   33
#define SAMPLE_RATE  16000
#define RECORD_SEC   600

// ======================= HELPERS =======================
void safeStrCpy(char* dest, const char* src, size_t destSize) {
    if (destSize == 0) return;
    strncpy(dest, src, destSize - 1);
    dest[destSize - 1] = '\0';
}

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

void blinkLed(int times, int delayMs) {
    for (int i = 0; i < times; i++) {
        digitalWrite(LED, HIGH);
        delay(delayMs);
        digitalWrite(LED, LOW);
        delay(delayMs);
    }
}

// ======================= NVS FUNCTIONS =======================
void loadConfigFromNVS() {
    preferences.begin("net_cfg", true);
    String storedSsid = preferences.getString("ssid", "");
    String storedPass = preferences.getString("pass", "");
    String storedMqtt = preferences.getString("mqtt", "");
    preferences.end();

    safeStrCpy(ssid, storedSsid.c_str(), sizeof(ssid));
    safeStrCpy(password, storedPass.c_str(), sizeof(password));
    safeStrCpy(mqttServer, storedMqtt.c_str(), sizeof(mqttServer));

    Serial.println("📂 Loaded config from NVS:");
    Serial.printf("   SSID: %s | PASS: %s | MQTT: %s\n", ssid, password, mqttServer);
}

void saveConfigToNVS(const char* newSsid, const char* newPass, const char* newMqtt) {
    preferences.begin("net_cfg", false);
    preferences.putString("ssid", newSsid);
    preferences.putString("pass", newPass);
    preferences.putString("mqtt", newMqtt);
    preferences.end();
    Serial.println("💾 Config successfully saved to NVS!");
}

void clearNVSConfig() {
    preferences.begin("net_cfg", false);
    preferences.clear();
    preferences.end();

    ssid[0] = '\0';
    password[0] = '\0';
    mqttServer[0] = '\0';

    Serial.println("🗑️ [NVS] Đã xóa sạch toàn bộ cấu hình lưu trữ!");
}

// Forward declarations
bool connectWiFi();

// ======================= ESP-NOW =======================
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowDataRecv(const esp_now_recv_info_t*, const uint8_t *data, int len) {
#else
void onEspNowDataRecv(const uint8_t*, const uint8_t *data, int len) {
#endif
    if (len != sizeof(WifiCredentialsMessage)) return;
    memcpy(&espNowConfig, data, sizeof(espNowConfig));
    espNowConfigReceived = true;
}

bool setupEspNow() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) {
        return false;
    }
    espNowConfigReceived = false;
    esp_now_register_recv_cb(onEspNowDataRecv);
    return true;
}

void processEspNowConfig() {
    safeStrCpy(ssid, espNowConfig.ssid, sizeof(ssid));
    safeStrCpy(password, espNowConfig.password, sizeof(password));
    safeStrCpy(mqttServer, espNowConfig.mqtt_server, sizeof(mqttServer));

    Serial.println("\n📩 RECEIVED NEW CONFIG VIA ESP-NOW:");
    Serial.printf("   SSID: %s\n   PASS: %s\n   MQTT: %s\n", ssid, password, mqttServer);

    saveConfigToNVS(ssid, password, mqttServer);
    Serial.println("📌 Config saved to NVS. Still in ESP-NOW mode. Press button to exit.");
}

// ======================= BUTTON HANDLING =======================
void checkEspNowButton() {
    static unsigned long pressStartTime = 0;
    static bool isPressing = false;
    static bool longPressHandled = false;
    
    const unsigned long LONG_PRESS_TIME = 3000;

    int btnState = digitalRead(espNowBtnPin);

    if (btnState == LOW && !isPressing) {
        isPressing = true;
        pressStartTime = millis();
        longPressHandled = false;
    }

    if (isPressing && btnState == LOW) {
        if (!longPressHandled && (millis() - pressStartTime >= LONG_PRESS_TIME)) {
            longPressHandled = true;
            Serial.println("\n⚠️ [GIỮ NÚT 3S] Tiến hành xóa sạch dữ liệu NVS...");
            clearNVSConfig();
            blinkLed(5, 100);
        }
    }

    if (btnState == HIGH && isPressing) {
        isPressing = false;
        unsigned long holdDuration = millis() - pressStartTime;

        if (holdDuration > 50 && !longPressHandled) {
            espNowEnabled = !espNowEnabled;
            Serial.println(espNowEnabled ? "\n🔘 [ẤN NGẮN] ESP-NOW: BẬT (ON)" : "\n🔘 [ẤN NGẮN] ESP-NOW: TẮT (OFF)");

            if (espNowEnabled) {
                Serial.printf("   Current SSID: %s | PASS: %s | MQTT: %s\n", ssid, password, mqttServer);
                if (!setupEspNow()) {
                    espNowEnabled = false;
                    Serial.println("❌ Khởi tạo ESP-NOW thất bại!");
                } else {
                    Serial.println("📡 Bắt đầu chờ cấu hình WiFi & MQTT qua ESP-NOW...");
                }
            } else {
                esp_now_deinit();
                Serial.println("🛑 Đã tắt ESP-NOW. Đang kết nối lại WiFi...");
                if (connectWiFi()) {
                    client.setServer(mqttServer, 1883);
                }
            }
        }
    }
}

// ======================= NETWORK MANAGEMENT =======================
bool connectWiFi() {
    if (strlen(ssid) == 0) {
        Serial.println("⚠️ Chưa có thông tin SSID trong bộ nhớ NVS!");
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.printf("🔌 Connecting WiFi to %s", ssid);

    int tryCount = 0;
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        tryCount++;
        delay(500);
        if (tryCount > 20) {
            Serial.println("\n⚠️ WiFi connect failed!");
            return false;
        }
    }
    Serial.println("\n✅ WiFi Connected. IP: " + WiFi.localIP().toString());
    WiFi.setSleep(false);
    return true;
}

bool handleMqttConnection() {
    if (espNowEnabled || client.connected() || strlen(mqttServer) == 0) return true;

    static unsigned long lastAttempt = 0;

    if (millis() - lastAttempt > 3000) {
        lastAttempt = millis();
        Serial.println("🔌 MQTT Connecting...");

        if (client.connect("ESP32_DEV_VOICE")) {
            client.subscribe("esp32/audio", 1);
            Serial.println("✅ MQTT Connected");
            blinkLed(3, 150); 
            return true;
        }
    }
    return false;
}

// ======================= I2S SETUP =======================
void setupI2S() {
    i2s_config_t cfg = {
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
    i2s_pin_config_t pin = {
        .bck_io_num    = I2S_SCK,
        .ws_io_num     = I2S_WS,
        .data_out_num  = I2S_PIN_NO_CHANGE,
        .data_in_num   = I2S_SD
    };
    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin);
}

// ======================= RECORDING =======================
int recordWavUntilStop(const char *filename) {
    File f = SD.open(filename, FILE_WRITE);
    if (!f) {
        Serial.println("❌ Không mở được file SD");
        return 0;
    }

    uint8_t header[44] = {0};
    f.write(header, 44); 

    int32_t  i2s_buf[256];
    int16_t  sd_buf[512];
    size_t   bytes_read;
    uint32_t totalSamples = 0;
    int      sd_buf_idx   = 0;

    unsigned long startTime    = millis();
    unsigned long lastMqttCheck = 0;
    bool stopSignal = false;
    bool isTimeout  = false;

    digitalWrite(LED, HIGH);
    Serial.println("🎙️ Recording started...");

    while (!stopSignal) {
        if (millis() - startTime >= (unsigned long)RECORD_SEC * 1000) {
            isTimeout = true;
            break;
        }

        if (i2s_read(I2S_PORT, i2s_buf, sizeof(i2s_buf), &bytes_read, pdMS_TO_TICKS(10)) == ESP_OK) {
            int samples = bytes_read / 4;
            for (int i = 0; i < samples; i++) {
                sd_buf[sd_buf_idx++] = (int16_t)(i2s_buf[i] >> 16);
                totalSamples++;
                if (sd_buf_idx >= 512) {
                    f.write((uint8_t*)sd_buf, sizeof(sd_buf));
                    sd_buf_idx = 0;
                }
            }
        }

        if (millis() - lastMqttCheck > 100) {
            client.loop();
            lastMqttCheck = millis();
            if (currentCmd == CMD_STOP_RECORD) {
                currentCmd = CMD_NONE;
                Serial.println("⏹️ Stop signal received via Enum State");
                stopSignal = true;
            }
        }
    }

    if (sd_buf_idx > 0) {
        f.write((uint8_t*)sd_buf, sd_buf_idx * 2);
    }

    uint32_t dataBytes = totalSamples * 2;
    makeWavHeader(header, dataBytes);
    f.seek(0);
    f.write(header, 44);
    f.close();

    digitalWrite(LED, LOW);

    if (isTimeout) {
        Serial.printf("⏱️ Đã hết 10 phút! Lưu %lu bytes nhưng KHÔNG GỬI lên server.\n", dataBytes);
        return 2;
    }

    Serial.printf("✅ Saved %lu bytes (%lu samples)\n", dataBytes, totalSamples);
    return 1;
}

// ======================= NETWORK UPLOAD =======================
bool sendToPi(const char *path) {
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.println("❌ Không đọc được file WAV");
        return false;
    }

    WiFiClient httpClient;
    if (!httpClient.connect(mqttServer, serverPort)) {
        Serial.println("❌ Host unreachable");
        f.close();
        return false;
    }

    String boundary = "----ESP32Boundary";
    String bodyStart = "--" + boundary +
                       "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"rec.wav\""
                       "\r\nContent-Type: audio/wav\r\n\r\n";
    String bodyEnd = "\r\n--" + boundary + "--\r\n";
    size_t totalLen = bodyStart.length() + f.size() + bodyEnd.length();

    httpClient.printf(
        "POST %s HTTP/1.1\r\nHost: %s\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n",
        uploadPath, mqttServer, boundary.c_str(), totalLen
    );

    httpClient.print(bodyStart);
    uint8_t buffer[1024];
    while (f.available()) {
        int n = f.read(buffer, sizeof(buffer));
        httpClient.write(buffer, n);
    }
    httpClient.print(bodyEnd);
    f.close();

    unsigned long timeout = millis();
    while (httpClient.connected() && !httpClient.available()) {
        if (millis() - timeout > 10000) { 
            Serial.println("❌ HTTP Server Response Timeout!");
            httpClient.stop();
            return false;
        }
        delay(10);
    }

    String statusLine = httpClient.readStringUntil('\n');
    bool uploadSuccess = (statusLine.indexOf("200") != -1 || statusLine.indexOf("201") != -1);

    if (!uploadSuccess) {
        Serial.printf("❌ Upload failed: %s\n", statusLine.c_str());
        httpClient.stop();
        return false;
    }

    while (httpClient.connected()) {
        String line = httpClient.readStringUntil('\n');
        if (line == "\r" || line.length() == 0) {
            break;
        }
    }

    String responseBody = httpClient.readString();
    httpClient.stop();

#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    DynamicJsonDocument doc(2048);
#endif

    DeserializationError error = deserializeJson(doc, responseBody);

    if (!error) {
        bool success = doc["success"] | false;
        const char* answer = doc["answer"];

        if (success && answer != nullptr) {
            Serial.println("\n==========================================");
            Serial.printf("💬 Gemini trả lời: %s\n", answer);
            Serial.println("==========================================\n");
        } else {
            Serial.println("⚠️ JSON hợp lệ nhưng thiếu trường 'answer' hoặc success = false.");
        }
    } else {
        Serial.printf("⚠️ Lỗi parse JSON: %s\n", error.c_str());
        Serial.println("Nội dung nhận được: " + responseBody);
    }

    return true;
}

// ======================= MQTT CALLBACK =======================
void callback(char* topic, byte* payload, unsigned int length) {
    char msg[length + 1];
    memcpy(msg, payload, length);
    msg[length] = '\0';
    
    Serial.printf("📩 MQTT [%s]: %s\n", topic, msg);

    if (strncmp(msg, "haVcKWfSpcN", 11) == 0) {
        currentCmd = CMD_START_RECORD;
    } else if (strncmp(msg, "fND", 3) == 0) {
        currentCmd = CMD_STOP_RECORD;
    }
}

// ======================= SETUP =======================
void setup() {
    Serial.begin(115200);
    pinMode(LED, OUTPUT);
    pinMode(espNowBtnPin, INPUT_PULLUP);

    loadConfigFromNVS();

    if (connectWiFi()) {
        client.setServer(mqttServer, 1883);
        client.setCallback(callback);
        client.setBufferSize(1024);
    }

    if (!SD.begin(SD_CS)) {
        Serial.println("❌ SD Failed");
        while (1);
    }

    setupI2S();
    Serial.println("=== System Ready ===");
}

// ======================= LOOP =======================
void loop() {
    checkEspNowButton();

    if (espNowEnabled) {
        if (espNowConfigReceived) {
            espNowConfigReceived = false;
            processEspNowConfig();
        }
        delay(10);
        return;
    }

    if (!client.connected()) {
        handleMqttConnection();
    } else {
        client.loop();
    }

    if (currentCmd == CMD_START_RECORD) {
        currentCmd = CMD_NONE;
        Serial.println("🎙️ Recording triggered by Enum command...");
        
        int recResult = recordWavUntilStop(WAV_FILE);
        
        if (recResult == 1) { 
            bool success = sendToPi(WAV_FILE);
            if (!success) {
                Serial.println("⚠️ Upload thất bại! File thu âm vẫn được giữ trên SD.");
            }
        } else if (recResult == 2) {
            Serial.println("🚫 Bỏ qua bước upload do ghi âm quá 10 phút.");
        }
    }

    delay(10);
}