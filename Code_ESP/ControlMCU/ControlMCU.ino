// ============================================================
//  esp32.ino – ESP32 (Arduino) – Điều khiển LED RGB & NVS Config
//  Đã fix: Lỗi đệ quy nút bấm GPIO0 khi thoát ESP-NOW sang WiFi
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_arduino_version.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ===== NVS =====
Preferences preferences;

// ===== PIN & THỜI GIAN GIỮ NÚT =====
#define LIGHT_BUTTON_PIN   4      // Chân nút Bật/Tắt đèn
#define ESP_NOW_BUTTON_PIN 0      // Chân nút Cấu hình / Reset

#define LIGHT_HOLD_MS      800    // GPIO4: Phải giữ >= 800ms mới Bật/Tắt đèn
#define ESPNOW_HOLD_MS     800    // GPIO0: Giữ >= 800ms mới vào/thoát ESP-NOW
#define CLEAR_HOLD_MS      5000   // GPIO0: Giữ >= 5000ms (5s) để Xóa NVS

#define RED_PIN            18
#define GREEN_PIN          19
#define BLUE_PIN           21

#define WIFI_CONNECT_MAX_TRIES 10

// ===== TOPICS MQTT =====
const char* sub_topic = "esp32/rgb"; // nhận
const char* pub_topic = "esp32/send"; //gửi

// ===== BIẾN TRẠNG THÁI MÀU =====
uint8_t R = 0;
uint8_t G = 255;
uint8_t B = 255;

bool lastTouchState = false;   // true = đèn đang BẬT

// ===== CẤU HÌNH MẠNG =====
char ssid[33]       = "";
char password[65]   = "";
char mqttServer[40] = "";
const char* mqtt_server = mqttServer;

// ===== ESP-NOW =====
struct WifiCredentialsMessage {
    char ssid[33];
    char password[65];
    char mqtt_server[40];
};

bool espNowMode           = false;   // Đang ở chế độ ESP-NOW
bool espNowConfigReceived = false;   // Vừa nhận gói tin mới
bool espNowConfigApplied  = false;   // Đã nhận thành công
WifiCredentialsMessage espNowConfig = {};

// ===== MQTT CLIENT =====
WiFiClient  espClient;
PubSubClient mqttClient(espClient);

// ===== PROTOTYPES =====
void connectWiFi();
void setColor(uint8_t r, uint8_t g, uint8_t b);
void toggleEspNowMode();
void handleTouchTap();
void updateEspNowIndicator();
void checkLightButton();
void checkEspNowButton();
void checkAllButtons();
void loadStoredCredentials();
void clearAllCredentials();

// ============================================================
// NVS – Lưu / Nạp cấu hình
// ============================================================
void loadStoredCredentials() {
    preferences.begin("config", true);
    String savedSsid = preferences.getString("ssid", "");
    String savedPass = preferences.getString("pass", "");
    String savedMqtt = preferences.getString("mqtt", "");
    preferences.end();

    if (savedSsid.length() > 0) {
        savedSsid.toCharArray(ssid, sizeof(ssid));
        savedPass.toCharArray(password, sizeof(password));
        savedMqtt.toCharArray(mqttServer, sizeof(mqttServer));
        Serial.println("📦 Đã nạp cấu hình từ NVS:");
        Serial.println("   SSID: " + String(ssid));
        Serial.println("   MQTT: " + String(mqttServer));
    } else {
        Serial.println("⚠️ Chưa có cấu hình lưu trong NVS - tự vào chế độ ESP-NOW.");
    }
}

void clearAllCredentials() {
    if (espNowMode) {
        esp_now_deinit();
        espNowMode = false;
    } else {
        WiFi.disconnect(true);
    }

    preferences.begin("config", false);
    preferences.clear();
    preferences.end();

    memset(ssid, 0, sizeof(ssid));
    memset(password, 0, sizeof(password));
    memset(mqttServer, 0, sizeof(mqttServer));

    Serial.println("🗑️ [NVS] Đã xóa sạch cấu hình!");

    for (int i = 0; i < 3; i++) {
        setColor(255, 0, 0);
        delay(150);
        setColor(0, 0, 0);
        delay(150);
    }

    toggleEspNowMode();
}

// ============================================================
// ESP-NOW – Callback nhận cấu hình
// ============================================================
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowDataRecv(const esp_now_recv_info_t*, const uint8_t* data, int length) {
#else
void onEspNowDataRecv(const uint8_t*, const uint8_t* data, int length) {
#endif
    if (length != sizeof(WifiCredentialsMessage)) return;
    memcpy(&espNowConfig, data, sizeof(espNowConfig));
    espNowConfigReceived = true;
}

bool startEspNowConfig() {
    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        return false;
    }
    espNowConfigReceived = false;
    esp_now_register_recv_cb(onEspNowDataRecv);
    Serial.println("ESP-NOW config mode enabled; waiting for S3...");
    return true;
}

void applyEspNowConfig() {
    espNowConfig.ssid[sizeof(espNowConfig.ssid) - 1]               = '\0';
    espNowConfig.password[sizeof(espNowConfig.password) - 1]       = '\0';
    espNowConfig.mqtt_server[sizeof(espNowConfig.mqtt_server) - 1] = '\0';
    
    strncpy(ssid,       espNowConfig.ssid,        sizeof(ssid) - 1);
    strncpy(password,   espNowConfig.password,    sizeof(password) - 1);
    strncpy(mqttServer, espNowConfig.mqtt_server, sizeof(mqttServer) - 1);

    preferences.begin("config", false);
    preferences.putString("ssid", ssid);
    preferences.putString("pass", password);
    preferences.putString("mqtt", mqttServer);
    preferences.end();

    Serial.println("\n🎉 [ESP-NOW] Đã nhận cấu hình mới & Lưu NVS!");
    Serial.println("   SSID: "        + String(ssid));
    Serial.println("   MQTT server: " + String(mqttServer));
    Serial.println("👉 Nhấn giữ GPIO0 (>=800ms) LẦN NỮA để THOÁT ESP-NOW và kết nối WiFi!");
}

// ============================================================
// XỬ LÝ NÚT BẤM (ĐÃ FIX LỖI ĐỆ QUY)
// ============================================================
void checkLightButton() {
    static bool lastState = HIGH;
    static unsigned long pressStartTime = 0;
    static bool handled = false;

    bool currentState = digitalRead(LIGHT_BUTTON_PIN);

    if (lastState == HIGH && currentState == LOW) {
        pressStartTime = millis();
        handled = false;
        lastState = currentState;
        return;
    }

    if (currentState == LOW && !handled && (millis() - pressStartTime >= LIGHT_HOLD_MS)) {
        handled = true;
        lastState = currentState;
        if (!espNowMode) {
            handleTouchTap();
        }
        return;
    }

    lastState = currentState;
}

void checkEspNowButton() {
    static bool lastState = HIGH;
    static unsigned long pressStartTime = 0;
    static bool clearHandled = false;

    bool currentState = digitalRead(ESP_NOW_BUTTON_PIN);

    // Cánh xuống - Vừa nhấn nút
    if (lastState == HIGH && currentState == LOW) {
        pressStartTime = millis();
        clearHandled = false;
        lastState = currentState;
        return;
    }

    // Giữ >= 5 giây: Xóa NVS
    if (currentState == LOW && !clearHandled && (millis() - pressStartTime >= CLEAR_HOLD_MS)) {
        clearHandled = true;
        lastState = currentState;
        clearAllCredentials();
        return;
    }

    // Cánh lên - Vừa thả nút ra (ĐIỂM SỬA CHÍNH)
    if (lastState == LOW && currentState == HIGH) {
        unsigned long heldFor = millis() - pressStartTime;
        
        // Cập nhật lastState NGAY LẬP TỨC để tránh đệ quy khi connectWiFi() gọi lại hàm này
        lastState = currentState; 

        if (!clearHandled) {
            if (heldFor >= ESPNOW_HOLD_MS) {
                toggleEspNowMode();
            }
        }
        return;
    }

    lastState = currentState;
}

void checkAllButtons() {
    checkLightButton();
    checkEspNowButton();
}

void toggleEspNowMode() {
    espNowMode = !espNowMode;
    if (espNowMode) {
        espNowConfigApplied = false;
        if (!startEspNowConfig()) {
            espNowMode = false;
            Serial.println("!!! Khong vao duoc che do ESP-NOW");
        } else {
            Serial.println("== ĐÃ VÀO CHẾ ĐỘ ESP-NOW (đèn nhấp nháy XANH DƯƠNG) ==");
        }
    } else {
        esp_now_deinit();
        espNowConfigApplied = false;
        Serial.println("== ĐÃ THOÁT CHẾ ĐỘ ESP-NOW, CHUYỂN SANG KẾT NỐI WIFI ==");
        connectWiFi();
        mqttClient.setServer(mqtt_server, 1883);
        setColor(lastTouchState ? R : 0, lastTouchState ? G : 0, lastTouchState ? B : 0);
    }
}

// ============================================================
// LED & Color Helpers
// ============================================================
bool isDarkColor(uint8_t r, uint8_t g, uint8_t b) {
    return ((int)r + g + b) < 50;
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(RED_PIN,   r);
    ledcWrite(GREEN_PIN, g);
    ledcWrite(BLUE_PIN,  b);
}

void handleTouchTap() {
    if (!lastTouchState) {
        Serial.println("👉 [GPIO4 >= 800ms] BẬT đèn...");
        if (isDarkColor(R, G, B)) {
            R = 0; G = 255; B = 255;
        }
        setColor(R, G, B);
        if (mqttClient.connected()) mqttClient.publish(pub_topic, "to_on");
        lastTouchState = true;
    } else {
        Serial.println("👉 [GPIO4 >= 800ms] TẮT đèn...");
        setColor(0, 0, 0);
        if (mqttClient.connected()) mqttClient.publish(pub_topic, "to_off");
        lastTouchState = false;
    }
}

void updateEspNowIndicator() {
    static unsigned long lastBlink = 0;
    static bool blinkOn = false;

    unsigned long interval = espNowConfigApplied ? 500 : 250;

    if (millis() - lastBlink < interval) return;

    lastBlink = millis();
    blinkOn = !blinkOn;

    if (!blinkOn) {
        setColor(0, 0, 0);
    } else {
        if (espNowConfigApplied) {
            setColor(0, 255, 0);   // Xanh lá (Đã nhận xong)
        } else {
            setColor(0, 150, 255); // Xanh dương (Đang chờ S3)
        }
    }
}

// ============================================================
// MQTT Callback
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[length + 1];
    memcpy(msg, payload, length);
    msg[length] = '\0';
    String message = String(msg);

    Serial.println("\n📩 MQTT in: " + message);

    if (message == "OFFRGB") {
        setColor(0, 0, 0);
        lastTouchState = false;
        return;
    } else if (message == "ONRGB") {
        setColor(R, G, B);
        lastTouchState = true;
        return;
    } else if (message == "CLEAR_CONFIG") {
        mqttClient.publish(pub_topic, "Config NVS erased! Resetting...");
        clearAllCredentials();
        return;
    }

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, message);

    if (!err && doc.containsKey("r") && doc.containsKey("g") && doc.containsKey("b")) {
        R = doc["r"].as<uint8_t>();
        G = doc["g"].as<uint8_t>();
        B = doc["b"].as<uint8_t>();

        Serial.printf("🎨 Đổi màu LED: R=%d, G=%d, B=%d\n", R, G, B);
        setColor(R, G, B);
        lastTouchState = !isDarkColor(R, G, B);

        String feedback = "Đã đổi màu: R" + String(R) + " G" + String(G) + " B" + String(B);
        mqttClient.publish(pub_topic, feedback.c_str());
    }
}

// ============================================================
// WiFi & MQTT
// ============================================================
void connectWiFi() {
    if (strlen(ssid) == 0) {
        Serial.println("⚠️ Chưa có SSID! Tự động bật ESP-NOW chờ cấu hình...");
        if (!espNowMode) toggleEspNowMode();
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("🔌 Connecting WiFi: ");
    Serial.println(ssid);

    int tryCount = 0;
    while (WiFi.status() != WL_CONNECTED) {
        checkAllButtons();
        if (espNowMode) {
            Serial.println("\n⏸️ Đã vào chế độ ESP-NOW, dừng chờ WiFi");
            return;
        }

        Serial.print(".");
        tryCount++;
        delay(500);

        if (tryCount >= WIFI_CONNECT_MAX_TRIES) {
            Serial.println();
            Serial.println("⚠️ Thất bại sau " + String(WIFI_CONNECT_MAX_TRIES) +
                            " lần thử - TỰ ĐỘNG chuyển sang chế độ chờ ESP-NOW");
            toggleEspNowMode();
            return;
        }
    }
    Serial.println("\n✅ WiFi Connected! IP: " + WiFi.localIP().toString());
    WiFi.setSleep(false);
}

void reconnectMQTT() {
    while (!mqttClient.connected()) {
        Serial.print("🔌 Connecting MQTT...");
        if (mqttClient.connect("ESP32_28pin_control")) {
            mqttClient.subscribe(sub_topic, 1);
            Serial.println("OK!");
        } else {
            Serial.print("Failed, rc="); Serial.println(mqttClient.state());
            delay(2000);
        }
    }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(2000);

    pinMode(LIGHT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(ESP_NOW_BUTTON_PIN, INPUT_PULLUP);

    ledcAttach(RED_PIN,   5000, 8);
    ledcAttach(GREEN_PIN, 5000, 8);
    ledcAttach(BLUE_PIN,  5000, 8);
    setColor(0, 0, 0);

    loadStoredCredentials();
    connectWiFi();

    mqttClient.setServer(mqtt_server, 1883);
    mqttClient.setCallback(mqttCallback);
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    checkAllButtons();

    if (espNowMode) {
        if (espNowConfigReceived) {
            applyEspNowConfig();
            espNowConfigReceived = false;
            espNowConfigApplied = true;
        }
        updateEspNowIndicator();
        delay(10);
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) reconnectMQTT();
        mqttClient.loop();
    }

    delay(10);
}