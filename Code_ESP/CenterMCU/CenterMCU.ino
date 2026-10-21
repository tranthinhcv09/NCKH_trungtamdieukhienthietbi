#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "DHT.h"
#include <esp_wifi.h>

// ===== CẤU TRÚC GÓI TIN ESP-NOW PHÂN LOẠI =====
#define MSG_TYPE_TEXT 0
#define MSG_TYPE_WIFI 0xA1

typedef struct {
    uint8_t type;     // MSG_TYPE_TEXT
    char text[240];
} TextMessagePacket;

typedef struct {
    uint8_t type;     // MSG_TYPE_WIFI (0xA1)
    char ssid[33];
    char password[65];
    char mqtt_server[40];
} WifiMessagePacket;

// ===== DHT11 CONFIG =====
#define DHTPIN  13
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ===== NÚT BẤM DÙNG CHUNG (GPIO 15) =====
#define btnPin   15
#define PUSHTIME 5000

// ===== TOPICS MQTT =====
const char* sub_topic = "esp32/speech";
const char* pub_rgb   = "esp32/rgb";
const char* pub_topic = "esp32/send";

uint8_t wifiPeerAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ===== TIMING & VARIABLES =====
const uint8_t       MQTT_MAX_RETRIES           = 30;
const unsigned long MQTT_RETRY_INTERVAL_MS     = 5000;
const unsigned long DHT_READ_INTERVAL_MS       = 6000;
const unsigned long DHT_MIN_INTERVAL_MS        = 2000;
const unsigned long ESPNOW_RETRY_INTERVAL_MS   = 10000;
const unsigned long WIFI_BROADCAST_INTERVAL_MS = 500;
const unsigned long STA_RECONNECT_DELAY_MS     = 2000;

// ===== KÊNH ESP-NOW KHI KHÔNG CÓ WIFI (PHẢI TRÙNG ESP_NOW_OFFLINE_CHANNEL BÊN ESP32-S3) =====
#define ESPNOW_OFFLINE_CHANNEL 1
const unsigned long WIFI_RETRY_INTERVAL_MS  = 120000; // dò lại router mỗi 120s khi offline (0 = tắt)
const unsigned long WIFI_ATTEMPT_TIMEOUT_MS = 5000;   // mỗi lần thử tối đa 5s rồi về kênh offline

unsigned long lastMqttAttempt    = 0;
unsigned long lastEspNowSend     = 0;
unsigned long lastDhtRead        = 0;
unsigned long lastEspNowRetry    = 0;
uint8_t       mqttFailedAttempts = 0;
uint8_t       wifiRetryCount     = 0;

char        mqttServerBuf[40];
Preferences preferences;

WiFiClient   espClient;
PubSubClient mqttClient(espClient);
WebServer    webServer(80);

String ssid;
String password;
String mqtt_server;

int  wifiMode = 0; // 0=AP, 1=STA connected, 2=STA disconnected

bool wifiBroadcastEnabled = false;
bool webServerStarted     = false;
bool espNowReady          = false;

// ---- Cờ xử lý bất đồng bộ ----
volatile bool staReconnectPending = false;
unsigned long staDisconnectTime   = 0;

// ---- Quản lý WiFi khi offline (không cho STA dò kênh liên tục) ----
volatile bool    staConnecting     = false;  // đang thử nối router
volatile bool    staAttemptFailed  = false;  // event báo thử nối thất bại
volatile uint8_t lastRouterChannel = 0;      // kênh router lần nối thành công gần nhất
unsigned long    staConnectStart   = 0;
unsigned long    lastWifiRetry     = 0;
unsigned long    lastChannelCheck  = 0;

volatile bool wifiReconnectRequested = false;

volatile bool      pendingWifiUpdate = false;
WifiMessagePacket  pendingWifiPacket;

volatile bool pendingTextAvailable = false;
char          pendingTextBuffer[240];
String        pendingTextSource;

// ---- Nút bấm (state machine) ----
bool          btnPressed         = false;
unsigned long btnPressStart      = 0;
bool          longPressTriggered = false;

// ---- WiFi scan bất đồng bộ ----
bool wifiScanTriggered = false;

// Dummy LED functions (nếu bạn dùng LED RGB phần cứng, thêm code điều khiển vào đây)
void GREEN() {}
void RED() {}
void PURPLE() {}
void BLUE() {}
void ledOff() {}
void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {}

// ============================================================
// Prototypes
// ============================================================
void processIncomingText(String message, String source);
bool sendEspNowText(String text);
bool sendWifiCredentials();
void sendText(const char* topic, String text);
void sendTextdht(const char* topic, String text);
void applyWifiCredentials(const char* newSsid, const char* newPass, const char* newMqtt);
void WiFiEvent(WiFiEvent_t event);
void setupWifi();
void setupWebServer();
void checkButton();
bool setupEspNow();
void reconnectMQTT();
void lockEspNowChannel();
void manageOfflineWifi();

// ============================================================
// KHOÁ KÊNH ESP-NOW KHI KHÔNG CÓ ROUTER
// ============================================================
void lockEspNowChannel() {
    if (WiFi.status() == WL_CONNECTED || staConnecting) return;
    uint8_t ch = 0;
    wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);
    if (ch != ESPNOW_OFFLINE_CHANNEL) {
        esp_wifi_set_channel(ESPNOW_OFFLINE_CHANNEL, WIFI_SECOND_CHAN_NONE);
        Serial.printf("🔒 Offline: ép ESP-NOW về kênh %d (đang ở kênh %d)\n", ESPNOW_OFFLINE_CHANNEL, ch);
    }
}

// Gọi trong loop(): giữ kênh cố định khi offline, chỉ thỉnh thoảng thử nối lại router
void manageOfflineWifi() {
    if (wifiBroadcastEnabled) return;                       // đang phát config: giữ nguyên
    if (WiFi.status() == WL_CONNECTED) { staConnecting = false; return; }

    unsigned long now = millis();

    if (staConnecting) {
        if (staAttemptFailed || now - staConnectStart > WIFI_ATTEMPT_TIMEOUT_MS) {
            staConnecting    = false;
            staAttemptFailed = false;
            WiFi.disconnect(false, false);                  // dừng dò kênh
            Serial.println("📴 Chưa có router -> về kênh ESP-NOW offline");
            lockEspNowChannel();
        }
        return;
    }

    if (now - lastChannelCheck >= 1000) {
        lastChannelCheck = now;
        lockEspNowChannel();
    }

    if (WIFI_RETRY_INTERVAL_MS > 0 && ssid.length() > 0 &&
        now - lastWifiRetry >= WIFI_RETRY_INTERVAL_MS) {
        lastWifiRetry    = now;
        staAttemptFailed = false;
        staConnectStart  = now;
        staConnecting    = true;
        Serial.println("🔁 Offline: thử nối lại router...");
        if (lastRouterChannel) WiFi.begin(ssid.c_str(), password.c_str(), lastRouterChannel);
        else                   WiFi.begin(ssid.c_str(), password.c_str());
    }
}

// ============================================================
// PHÁT CẤU HÌNH WIFI TRÊN KÊNH HIỆN TẠI
// ============================================================
bool sendWifiCredentials() {
    if (!espNowReady) return false;

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, wifiPeerAddress, 6);
    peerInfo.channel = 0; // 0 = dùng chung kênh RF hiện tại của WiFi
    peerInfo.encrypt = false;
    esp_now_mod_peer(&peerInfo);

    WifiMessagePacket pkt = {};
    pkt.type = MSG_TYPE_WIFI;
    ssid.toCharArray(pkt.ssid, sizeof(pkt.ssid));
    password.toCharArray(pkt.password, sizeof(pkt.password));
    mqtt_server.toCharArray(pkt.mqtt_server, sizeof(pkt.mqtt_server));

    Serial.printf("📡 [ESPNOW SEND] SSID: '%s' | PASS: '%s' | MQTT: '%s' | Kênh RF: %d\n",
                  pkt.ssid, pkt.password, pkt.mqtt_server, WiFi.channel());

    esp_err_t result = esp_now_send(wifiPeerAddress, (uint8_t *)&pkt, sizeof(pkt));
    if (result != ESP_OK) {
        Serial.printf("❌ esp_now_send() lỗi: %d\n", result);
    }
    return (result == ESP_OK);
}

// ============================================================
// GỬI TEXT DÙNG CHUNG — ƯU TIÊN ESP-NOW, FALLBACK MQTT
// ============================================================
void sendText(const char* topic, String text) {
    bool sent = sendEspNowText(text);
    if (sent) {
        Serial.println("📡 [ESP-NOW Text Sent]: " + text);
        return;
    }
    if (mqttClient.connected()) {
        mqttClient.publish(topic, text.c_str());
        Serial.println("📤 [MQTT Fallback Sent]: " + text);
    } else {
        Serial.println("❌ Gửi thất bại: Cả ESP-NOW và MQTT đều không khả dụng!");
    }
}

void sendTextdht(const char* topic, String text) {
    bool sent = sendEspNowText(text);
    if (sent) {
        Serial.println("📡 [ESP-NOW Text Sent]: " + text);
    }

    if (mqttClient.connected()) {
        mqttClient.publish(topic, text.c_str());
        Serial.println("📤 [MQTT Fallback Sent]: " + text);
    } else {
        Serial.println("❌ Gửi thất bại: MQTT không kết nối!");
    }
}

// ============================================================
// ESP-NOW FUNCTIONS
// ============================================================
bool sendEspNowText(String text) {
    if (!espNowReady) return false;

    TextMessagePacket pkt = {};
    pkt.type = MSG_TYPE_TEXT;
    text.toCharArray(pkt.text, sizeof(pkt.text));

    esp_err_t result = esp_now_send(wifiPeerAddress, (uint8_t *)&pkt, sizeof(pkt));
    Serial.printf("ESP-NOW send=%d | kênh RF=%d\n", result, WiFi.channel());
    return (result == ESP_OK);
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
    if (len < 1) return;
    uint8_t type = data[0];
    const uint8_t *m = info->src_addr;

    if (type == MSG_TYPE_TEXT && len >= 2) {
        // Chấp nhận mọi độ dài: chỉ lấy phần text sau byte type
        size_t n = min((size_t)(len - 1), sizeof(pendingTextBuffer) - 1);
        memcpy(pendingTextBuffer, data + 1, n);
        pendingTextBuffer[n] = '\0';
        pendingTextSource    = "ESP-NOW";
        pendingTextAvailable = true;

    } else if (type == MSG_TYPE_WIFI && len == sizeof(WifiMessagePacket)) {
        memcpy(&pendingWifiPacket, data, sizeof(pendingWifiPacket));
        // Đảm bảo chuỗi luôn kết thúc bằng '\0'
        pendingWifiPacket.ssid[sizeof(pendingWifiPacket.ssid) - 1]               = '\0';
        pendingWifiPacket.password[sizeof(pendingWifiPacket.password) - 1]       = '\0';
        pendingWifiPacket.mqtt_server[sizeof(pendingWifiPacket.mqtt_server) - 1] = '\0';
        pendingWifiUpdate = true;

    } else {
        Serial.println("⚠️ Gói ESP-NOW không xác định -> bỏ qua");
    }
}

void applyWifiCredentials(const char* newSsid, const char* newPass, const char* newMqtt) {
    Serial.printf("📶 [ESP-NOW] Nhận cấu hình WiFi mới: SSID=%s | MQTT=%s\n", newSsid, newMqtt);

    ssid        = String(newSsid);
    password    = String(newPass);
    mqtt_server = String(newMqtt);

    preferences.begin("config", false);
    preferences.putString("ssid", ssid);
    preferences.putString("pass", password);
    preferences.putString("mqtt", mqtt_server);
    preferences.end();

    mqtt_server.toCharArray(mqttServerBuf, sizeof(mqttServerBuf));
    mqttClient.setServer(mqttServerBuf, 1883);
    if (mqttClient.connected()) mqttClient.disconnect();

    Serial.println("🔄 Sẽ kết nối lại WiFi với cấu hình mới...");
    wifiReconnectRequested = true;
}

bool setupEspNow() {
    esp_now_deinit();
    esp_wifi_set_ps(WIFI_PS_NONE);   // tắt modem-sleep: tránh lỡ gói ESP-NOW

    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW init thất bại!");
        return false;
    }

    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, wifiPeerAddress, 6);
    peerInfo.channel = 0; // Tự động đồng bộ theo Kênh hiện tại
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("❌ Thêm ESP-NOW peer thất bại!");
        esp_now_deinit();
        return false;
    }
    return true;
}

void setupWifi() {
    ssid.trim();
    password.trim();

    espNowReady      = false;
    staConnecting    = false;
    staAttemptFailed = false;

    // Tự quản lý việc nối lại: tắt autoReconnect để STA không dò kênh liên tục
    WiFi.setAutoReconnect(false);

    if (ssid.length() > 0) {
        Serial.println("=== WIFI DEBUG INFO ===");
        Serial.println("SSID: '" + ssid + "' (length: " + String(ssid.length()) + ")");

        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false, false);
        delay(100);

        WiFi.begin(ssid.c_str(), password.c_str());

        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
            delay(500);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            esp_wifi_set_ps(WIFI_PS_NONE);
            wifiMode = 1;
            lastRouterChannel = WiFi.channel();
            espNowReady = setupEspNow();
            return;
        }
        Serial.println("❌ Không kết nối được mạng đã lưu -> chuyển sang AP mode.");
        WiFi.disconnect(false, false);   // dừng STA dò kênh trước khi sang AP
    }

    // Chế độ AP: Phát AP cố định kênh offline (trùng kênh ESP-NOW)
    WiFi.mode(WIFI_AP_STA);
    uint8_t macAddr[6];
    WiFi.softAPmacAddress(macAddr);
    String ssid_ap = "TDTHINH-" + String(macAddr[4], HEX) + String(macAddr[5], HEX);
    ssid_ap.toUpperCase();
    
    WiFi.softAP(ssid_ap.c_str(), NULL, ESPNOW_OFFLINE_CHANNEL, 0);
    esp_wifi_set_channel(ESPNOW_OFFLINE_CHANNEL, WIFI_SECOND_CHAN_NONE);
    wifiMode      = 0;
    lastWifiRetry = millis();

    Serial.println("========================================");
    Serial.println("📶 ĐANG PHÁT ACCESS POINT ĐỂ CẤU HÌNH:");
    Serial.println("   Tên WiFi : " + ssid_ap);
    Serial.println("   IP config: " + WiFi.softAPIP().toString());
    Serial.println("   Mở trình duyệt tới: http://" + WiFi.softAPIP().toString());
    Serial.println("========================================");

    espNowReady = setupEspNow();
}

// ============================================================
// XỬ LÝ TEXT DÙNG CHUNG
// ============================================================
void processIncomingText(String message, String source) {
    message.trim();
    if (message.length() == 0) return;

    Serial.println("\n📩 [" + source + " Received]: " + message);

    if (message == "temp" || message == "dht" || message == "read_dht") {
        if (millis() - lastDhtRead < DHT_MIN_INTERVAL_MS) {
            sendTextdht(pub_topic, "⏳ DHT11 vừa đọc gần đây, thử lại sau vài giây");
            return;
        }
        float h = dht.readHumidity();
        float t = dht.readTemperature();

        if (isnan(h) || isnan(t)) {
            Serial.println("❌ Lỗi đọc cảm biến DHT11!");
            sendTextdht(pub_topic, "❌ Lỗi đọc cảm biến DHT11!");
            return;
        }

        String dhtData = "Nhiệt độ: " + String(t, 1) + "°C | Độ ẩm: " + String(h, 1) + "%";
        sendTextdht(pub_topic, dhtData);
        return;
    }

    if (message == "TkN nN jc LiTS" || message == "to_on") {
        sendTextdht(pub_rgb, "ONRGB");
        GREEN();
        return;
    } else if (message == "TkN eF jc LiTS" || message == "eF LiT" || message == "to_off") {
        ledOff();
        sendTextdht(pub_rgb, "OFFRGB");
        return;
    } else if (message == "GRmN LiT") {
        GREEN();
        return;
    } else if (message == "RfD LiT") {
        RED();
        return;
    } else if (message == "BLo LiT") {
        BLUE();
        return;
    } else if (message == "fan_off") {
        sendTextdht("esp32/fan", "fan_off");
        return;
    } else if (message == "fan_on_50" || message == "fan_speed_50") {
        sendTextdht("esp32/fan", "fan_on_50");
        return;
    } else if (message == "fan_on_25" || message == "fan_speed_25") {
        sendTextdht("esp32/fan", "fan_on_25");
        return;
    } else if (message == "fan_on_75" || message == "fan_speed_75") {
        sendTextdht("esp32/fan", "fan_on_75");
        return;
    } else if (message == "fan_on_100" || message == "fan_speed_100") {
        sendTextdht("esp32/fan", "fan_on_100");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, message);

    if (!err && doc["r"].is<int>() && doc["g"].is<int>() && doc["b"].is<int>()) {
        uint8_t r = doc["r"].as<uint8_t>();
        uint8_t g = doc["g"].as<uint8_t>();
        uint8_t b = doc["b"].as<uint8_t>();
        setLEDColor(r, g, b);
        sendText(pub_rgb, message);
        return;
    }

    sendText(pub_topic, "ACK from ESP32 DevKit: " + message);
}

// ============================================================
// MQTT
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (length > 240) length = 240;
    char msg[length + 1];
    memcpy(msg, payload, length);
    msg[length] = '\0';
    processIncomingText(String(msg), "MQTT");
}

void reconnectMQTT() {
    if (mqttClient.connect("ESP32_DevKit_DHT_MASTER")) {
        mqttClient.subscribe(sub_topic, 1);
        mqttFailedAttempts = 0;
        Serial.println("✅ MQTT connected!");
        return;
    }
    mqttFailedAttempts++;
    if (mqttFailedAttempts >= MQTT_MAX_RETRIES) {
        mqttFailedAttempts = 0;
    }
}

// ============================================================
// WIFI
// ============================================================
void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            esp_wifi_set_ps(WIFI_PS_NONE);  
            Serial.println("✅ Connected to WiFi, IP: " + WiFi.localIP().toString());
            Serial.printf("📡 Wi-Fi Channel hiện tại: %d\n", WiFi.channel());
            wifiMode = 1;
            wifiRetryCount = 0;
            staReconnectPending = false;
            staConnecting       = false;
            lastRouterChannel   = WiFi.channel();
            GREEN();
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (staConnecting) {
                staAttemptFailed = true;      // lần thử nối lại thất bại -> loop() sẽ về kênh offline
            } else if (wifiMode == 1) {
                Serial.println("⚠️ WiFi STA bị rớt -> chuyển sang kênh ESP-NOW offline");
                wifiMode      = 2;
                lastWifiRetry = millis();     // lần dò lại đầu tiên sau WIFI_RETRY_INTERVAL_MS
            }
            break;
        default:
            break;
    }
}

// ============================================================
// WEB SERVER
// ============================================================
void setupWebServer() {
    if (webServerStarted) return;

    webServer.on("/", HTTP_GET, []() {
        String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="utf-8">
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>SETTING WIFI & MQTT</title>
        <style type="text/css">
            body { font-family: sans-serif; display: flex; justify-content: center; align-items: center; min-height: 90vh; background-color: #f0f2f5; margin: 0; }
            .card { background: white; padding: 25px 30px; border-radius: 10px; box-shadow: 0 4px 12px rgba(0,0,0,0.15); width: 320px; }
            h3 { text-align: center; margin-top: 0; color: #333; }
            p#info { text-align: center; font-size: 14px; color: #666; margin-bottom: 15px; }
            label { font-size: 16px; font-weight: bold; color: #444; display: block; margin-top: 10px; margin-bottom: 5px; }
            input, select { width: 100%; height: 35px; font-size: 15px; border: 1px solid #ccc; border-radius: 5px; box-sizing: border-box; padding: 0 8px; }
            .btn-group { display: flex; justify-content: space-between; margin-top: 20px; }
            button { width: 48%; height: 40px; border: none; border-radius: 5px; font-size: 15px; font-weight: bold; cursor: pointer; color: white; }
            .btn-save { background-color: #007bff; }
            .btn-save:hover { background-color: #0056b3; }
            .btn-restart { background-color: #dc3545; }
            .btn-restart:hover { background-color: #a71d2a; }
        </style>
    </head>
    <body>
        <div class="card">
        <h3>CẤU HÌNH ESP32 DEVKIT V1</h3>
        <p id="info">Đang quét mạng WiFi...</p>

        <label>Tên WiFi (SSID):</label>
        <select id="ssid">
            <option value="">-- Chọn WiFi hoặc nhập bên dưới --</option>
        </select>

        <label>Hoặc nhập SSID tùy chỉnh:</label>
        <input id="custom_ssid" type="text" placeholder="Nhập SSID nếu ẩn">

        <label>Mật khẩu WiFi:</label>
        <input id="password" type="text" placeholder="Nhập mật khẩu">

        <label>MQTT Broker IP:</label>
        <input id="mqtt" type="text" placeholder="Ví dụ: 192.168.1.28">

        <div class="btn-group">
            <button class="btn-save" onclick="saveWifi()">LƯU</button>
            <button class="btn-restart" onclick="reStart()">KHỞI ĐỘNG</button>
        </div>
        </div>

        <script type="text/javascript">
        window.onload = function() {
            loadCurrentConfig();
            scanWifi();
        };

        function loadCurrentConfig() {
            var xhr = new XMLHttpRequest();
            xhr.onreadystatechange = function() {
            if (xhr.readyState == 4 && xhr.status == 200) {
                try {
                var cfg = JSON.parse(xhr.responseText);
                if (cfg.ssid) document.getElementById("custom_ssid").value = cfg.ssid;
                if (cfg.pass) document.getElementById("password").value = cfg.pass;
                if (cfg.mqtt) document.getElementById("mqtt").value = cfg.mqtt;
                } catch(e){}
            }
            };
            xhr.open("GET", "/getConfig", true);
            xhr.send();
        }

        function scanWifi() {
            var xhttp = new XMLHttpRequest();
            xhttp.onreadystatechange = function() {
            if (xhttp.readyState == 4 && xhttp.status == 200) {
                try {
                var obj = JSON.parse(xhttp.responseText);
                if (obj.status === "scanning") {
                    document.getElementById("info").innerHTML = "Đang quét WiFi...";
                    setTimeout(scanWifi, 1000);
                    return;
                }
                document.getElementById("info").innerHTML = "Đã quét xong danh sách WiFi!";
                var select = document.getElementById("ssid");
                select.innerHTML = '<option value="">-- Chọn WiFi trong danh sách --</option>';
                var list = obj.networks || [];
                for (var i = 0; i < list.length; ++i) {
                    if (list[i] && list[i].length > 0) {
                    var opt = document.createElement('option');
                    opt.value = list[i];
                    opt.innerHTML = list[i];
                    select.appendChild(opt);
                    }
                }
                } catch(e){
                    document.getElementById("info").innerHTML = "Lỗi quét WiFi.";
                }
            }
            };
            xhttp.open("GET", "/scanWifi", true);
            xhttp.send();
        }

        function saveWifi() {
            var selectedSsid = document.getElementById("ssid").value;
            var customSsid = document.getElementById("custom_ssid").value;
            var finalSsid = selectedSsid ? selectedSsid : customSsid;
            var pass = document.getElementById("password").value;
            var mqtt_val = document.getElementById("mqtt").value;

            if (!finalSsid) {
            alert("Vui lòng chọn hoặc nhập tên WiFi!");
            return;
            }

            var xhttp = new XMLHttpRequest();
            xhttp.onreadystatechange = function() {
            if (xhttp.readyState == 4 && xhttp.status == 200) {
                alert(xhttp.responseText);
            }
            };
            xhttp.open("GET", "/saveWifi?ssid=" + encodeURIComponent(finalSsid) +
                            "&pass=" + encodeURIComponent(pass) +
                            "&mqtt_server=" + encodeURIComponent(mqtt_val), true);
            xhttp.send();
        }

        function reStart() {
            if (!confirm("Bạn có chắc muốn khởi động lại thiết bị?")) return;
            var xhttp = new XMLHttpRequest();
            xhttp.onreadystatechange = function() {
            if (xhttp.readyState == 4 && xhttp.status == 200) {
                alert(xhttp.responseText);
            }
            };
            xhttp.open("GET", "/reStart", true);
            xhttp.send();
        }
        </script>
    </body>
    </html>
    )rawliteral";

        webServer.send(200, "text/html", html);
    });

    webServer.on("/getConfig", HTTP_GET, []() {
        JsonDocument doc;
        doc["ssid"] = ssid;
        doc["pass"] = password;
        doc["mqtt"] = mqtt_server;
        String json;
        serializeJson(doc, json);
        webServer.send(200, "application/json", json);
    });

    webServer.on("/scanWifi", HTTP_GET, []() {
        int8_t result = WiFi.scanComplete();

        if (!wifiScanTriggered) {
            WiFi.scanNetworks(true, false);
            wifiScanTriggered = true;
            webServer.send(200, "application/json", "{\"status\":\"scanning\"}");
            return;
        }

        if (result == WIFI_SCAN_RUNNING) {
            webServer.send(200, "application/json", "{\"status\":\"scanning\"}");
            return;
        }

        if (result == WIFI_SCAN_FAILED) {
            wifiScanTriggered = false;
            webServer.send(200, "application/json", "{\"status\":\"scanning\"}");
            return;
        }

        JsonDocument doc;
        doc["status"] = "done";
        JsonArray arr = doc["networks"].to<JsonArray>();
        for (int i = 0; i < result; ++i) {
            arr.add(WiFi.SSID(i));
        }
        WiFi.scanDelete();
        wifiScanTriggered = false;

        String json;
        serializeJson(doc, json);
        webServer.send(200, "application/json", json);
    });

    webServer.on("/saveWifi", HTTP_GET, []() {
        if (webServer.hasArg("ssid")) {
            ssid        = webServer.arg("ssid");
            password    = webServer.arg("pass");
            mqtt_server = webServer.arg("mqtt_server");

            ssid.trim();
            password.trim();
            mqtt_server.trim();

            preferences.begin("config", false);
            preferences.putString("ssid", ssid);
            preferences.putString("pass", password);
            preferences.putString("mqtt", mqtt_server);
            preferences.end();

            mqtt_server.toCharArray(mqttServerBuf, sizeof(mqttServerBuf));
            mqttClient.setServer(mqttServerBuf, 1883);

            webServer.send(200, "text/plain", "Đã lưu cấu hình thành công! Hãy bấm KHỞI ĐỘNG.");
        } else {
            webServer.send(400, "text/plain", "Lỗi: Thiếu thông tin SSID!");
        }
    });

    webServer.on("/reStart", HTTP_GET, []() {
        webServer.send(200, "text/plain", "Đang khởi động lại ESP32 DevKit V1...");
        delay(1000);
        ESP.restart();
    });

    webServer.onNotFound([]() {
        webServer.sendHeader("Location", "/", true);
        webServer.send(302, "text/plain", "");
    });

    webServer.begin();
    webServerStarted = true;
    Serial.println("🌐 Web Server Config đã sẵn sàng!");
}

// ============================================================
// NÚT BẤM DÙNG CHUNG (GPIO 15) - ĐÃ ĐƠN GIẢN HÓA TRÊN 1 KÊNH
// ============================================================
void checkButton() {
    bool reading = (digitalRead(btnPin) == LOW);

    if (reading && !btnPressed) {
        btnPressed         = true;
        btnPressStart      = millis();
        longPressTriggered = false;

    } else if (reading && btnPressed) {
        if (!longPressTriggered && millis() - btnPressStart >= PUSHTIME) {
            longPressTriggered = true;
            Serial.println("🗑️ Giữ đủ 5s -> Xoá cấu hình NVS");
            preferences.begin("config", false);
            preferences.clear();
            preferences.end();
            delay(300);
            ESP.restart();
        }

    } else if (!reading && btnPressed) {
        btnPressed = false;
        unsigned long pressDuration = millis() - btnPressStart;

        if (!longPressTriggered && pressDuration < PUSHTIME) {
            wifiBroadcastEnabled = !wifiBroadcastEnabled;

            if (wifiBroadcastEnabled) {
                if (ssid.length() == 0) {
                    Serial.println("\n⚠️ Chưa có cấu hình WiFi để phát! Bỏ qua.");
                    wifiBroadcastEnabled = false;
                } else {
                    PURPLE();
                    Serial.println("\n📡 [NÚT BẤM] BẬT PHÁT WiFi Config qua ESP-NOW!");
                }
            } else {
                GREEN();
                Serial.println("\n🛑 [NÚT BẤM] TẮT PHÁT WiFi Config -> Trở về bình thường");
            }
        }
    }
}

// ============================================================
// SETUP & LOOP
// ============================================================
void setup() {
    Serial.begin(115200);
    pinMode(btnPin, INPUT_PULLUP);

    dht.begin();

    WiFi.onEvent(WiFiEvent);

    preferences.begin("config", true);
    ssid        = preferences.getString("ssid", "");
    password    = preferences.getString("pass", "");
    mqtt_server = preferences.getString("mqtt", "192.168.1.20");
    preferences.end();

    setupWifi();
    setupWebServer();

    RED();

    mqtt_server.toCharArray(mqttServerBuf, sizeof(mqttServerBuf));
    mqttClient.setServer(mqttServerBuf, 1883);
    mqttClient.setCallback(mqttCallback);

    espNowReady = setupEspNow();
    if (!espNowReady) {
        Serial.println("⚠️ ESP-NOW chưa sẵn sàng, sẽ tự thử lại định kỳ.");
        lastEspNowRetry = millis();
    }

    Serial.print("📶 MAC Address (STA): ");
    Serial.println(WiFi.macAddress());

    GREEN();
    Serial.println("🚀 Hệ thống DHT11 & Text Hub trên ESP32 DevKit V1 đã sẵn sàng!");
    lastDhtRead = millis();
}

void loop() {
    checkButton();
    webServer.handleClient();

    if (pendingTextAvailable) {
        pendingTextAvailable = false;
        processIncomingText(String(pendingTextBuffer), pendingTextSource);
    }
    if (pendingWifiUpdate) {
        pendingWifiUpdate = false;
        applyWifiCredentials(pendingWifiPacket.ssid, pendingWifiPacket.password, pendingWifiPacket.mqtt_server);
    }

    manageOfflineWifi();   // giữ kênh ESP-NOW cố định khi mất router, dò lại thưa

    if (!wifiBroadcastEnabled && wifiReconnectRequested) {
        wifiReconnectRequested = false;
        setupWifi();
    }

    if (!espNowReady && millis() - lastEspNowRetry >= ESPNOW_RETRY_INTERVAL_MS) {
        lastEspNowRetry = millis();
        espNowReady = setupEspNow();
    }

    // Gửi cấu hình định kỳ khi cờ wifiBroadcastEnabled được BẬT bằng nút bấm
    if (wifiBroadcastEnabled && ssid.length() > 0 &&
        millis() - lastEspNowSend >= WIFI_BROADCAST_INTERVAL_MS) {
        lastEspNowSend = millis();
        sendWifiCredentials();
    }

    // Xử lý kết nối MQTT khi ở chế độ STA thành công
    if (wifiMode == 1) {
        if (!mqttClient.connected() && millis() - lastMqttAttempt > MQTT_RETRY_INTERVAL_MS) {
            lastMqttAttempt = millis();
            reconnectMQTT();
        }
        mqttClient.loop();
    }

    // Đọc cảm biến DHT11 định kỳ
    if (millis() - lastDhtRead >= DHT_READ_INTERVAL_MS) {
        lastDhtRead = millis();
        float t = dht.readTemperature();
        float h = dht.readHumidity();

        if (!isnan(t) && !isnan(h)) {
            String tempStr = "DHT11 -> Temp: " + String(t, 1) + "C | Hum: " + String(h, 1) + "%";
            sendTextdht("esp32/dht", tempStr);
        } else {
            Serial.println("❌ Lỗi đọc DHT11 định kỳ, sẽ thử lại ở chu kỳ sau");
        }
    }

    delay(1);
}
