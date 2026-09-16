#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
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
#define DHTPIN  4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ===== LED RGB tích hợp ESP32-S3 (chân 48) =====
#define LED_PIN   48
#define NUMPIXELS 1
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ===== NÚT BẤM DÙNG CHUNG (GPIO 0) =====
#define btnPin   0
#define PUSHTIME 5000

// ===== TOPICS MQTT =====
const char* sub_topic = "esp32/speech";
const char* pub_rgb   = "esp32/rgb";
const char* pub_topic = "esp32/send";

uint8_t wifiPeerAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ===== TIMING & VARIABLES =====
const uint8_t       MQTT_MAX_RETRIES         = 30;
const unsigned long MQTT_RETRY_INTERVAL_MS   = 5000;
const unsigned long DHT_READ_INTERVAL_MS     = 60000;
const unsigned long DHT_MIN_INTERVAL_MS      = 2000;
const unsigned long ESPNOW_RETRY_INTERVAL_MS = 10000;
const unsigned long WIFI_BROADCAST_INTERVAL_MS = 500;
const unsigned long STA_RECONNECT_DELAY_MS   = 2000;

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

volatile bool wifiReconnectRequested = false;

volatile bool          pendingWifiUpdate = false;
WifiMessagePacket       pendingWifiPacket;

volatile bool pendingTextAvailable = false;
char          pendingTextBuffer[240];
String        pendingTextSource;

// ---- Nút bấm (state machine) ----
bool          btnPressed         = false;
unsigned long btnPressStart      = 0;
bool          longPressTriggered = false;

// ---- WiFi scan bất đồng bộ ----
bool wifiScanTriggered = false;

// ============================================================
// LED Helpers
// ============================================================
void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}
void ledOff() { pixels.clear(); pixels.show(); }
void RED()    { setLEDColor(255, 0, 0); }
void GREEN()  { setLEDColor(0, 255, 0); }
void BLUE()   { setLEDColor(0, 0, 255); }
void PURPLE() { setLEDColor(255, 0, 255); }

// ============================================================
// Prototypes
// ============================================================
void processIncomingText(String message, String source);
bool sendEspNowText(String text);
bool sendWifiCredentialsChannel1();
void sendText(const char* topic, String text);
void sendTextdht(const char* topic, String text);
void applyWifiCredentials(const char* newSsid, const char* newPass, const char* newMqtt);
void WiFiEvent(WiFiEvent_t event);
void setupWifi();
void setupWebServer();
void checkButton();
bool setupEspNow();
void reconnectMQTT();
bool forceEspNowChannel(uint8_t channel);

// ============================================================
// ÉP KÊNH RF CHO ESP-NOW (có log + kiểm tra lỗi)
// ============================================================
bool forceEspNowChannel(uint8_t channel) {
    if (WiFi.status() == WL_CONNECTED) {
        // QUAN TRỌNG: wifioff PHẢI là false. Nếu để true, WiFi.disconnect()
        // sẽ dừng luôn driver WiFi (esp_wifi_stop()) -> ESP-NOW tự động bị
        // deinit theo. Ở đây ta chỉ cần rời AP (disassociate) để mở khoá
        // kênh, không được tắt driver.
        WiFi.disconnect(false, false);
        delay(100);
    }
    WiFi.mode(WIFI_STA);
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) {
        Serial.printf("📡 Đã ép kênh RF sang Kênh %d thành công.\n", channel);
    } else {
        Serial.printf("❌ LỖI ép kênh sang %d! esp_err_t = %d (0x%X)\n", channel, err, err);
    }
    return (err == ESP_OK);
}

// ============================================================
// PHÁT CẤU HÌNH WIFI CỐ ĐỊNH TRÊN KÊNH 1
// ============================================================
bool sendWifiCredentialsChannel1() {
    if (!espNowReady) return false;

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, wifiPeerAddress, 6);
    peerInfo.channel = 0; // 0 = dùng kênh RF hiện tại (đã ép = 1)
    peerInfo.encrypt = false;
    esp_now_mod_peer(&peerInfo);

    WifiMessagePacket pkt;
    pkt.type = MSG_TYPE_WIFI;
    ssid.toCharArray(pkt.ssid, sizeof(pkt.ssid));
    password.toCharArray(pkt.password, sizeof(pkt.password));
    mqtt_server.toCharArray(pkt.mqtt_server, sizeof(pkt.mqtt_server));

    Serial.printf("📡 [ESPNOW SEND Ch1] SSID: '%s' | PASS: '%s' | MQTT: '%s' | Kênh RF hiện tại: %d\n",
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
        Serial.println("❌ Gửi thất bại: MQTT!");
    }
}

// ============================================================
// ESP-NOW FUNCTIONS
// ============================================================
bool sendEspNowText(String text) {
    if (!espNowReady) return false;

    TextMessagePacket pkt;
    pkt.type = MSG_TYPE_TEXT;
    text.toCharArray(pkt.text, sizeof(pkt.text));

    esp_err_t result = esp_now_send(wifiPeerAddress, (uint8_t *)&pkt, sizeof(pkt));
    return (result == ESP_OK);
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
    if (len < 1) return;
    uint8_t type = incomingData[0];

    if (type == MSG_TYPE_TEXT && len == sizeof(TextMessagePacket)) {
        TextMessagePacket pkt;
        memcpy(&pkt, incomingData, sizeof(pkt));
        strncpy(pendingTextBuffer, pkt.text, sizeof(pendingTextBuffer) - 1);
        pendingTextBuffer[sizeof(pendingTextBuffer) - 1] = '\0';
        pendingTextSource   = "ESP-NOW";
        pendingTextAvailable = true;

    } else if (type == MSG_TYPE_WIFI && len == sizeof(WifiMessagePacket)) {
        memcpy(&pendingWifiPacket, incomingData, sizeof(pendingWifiPacket));
        pendingWifiUpdate = true;

    } else {
        Serial.printf("⚠️ Gói ESP-NOW không xác định (type=0x%02X, len=%d)\n", type, len);
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

    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW init thất bại!");
        return false;
    }

    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, wifiPeerAddress, 6);
    peerInfo.channel = 0; // Sử dụng kênh RF hiện tại
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

    espNowReady = false;

    if (ssid.length() > 0) {
        Serial.println("=== WIFI DEBUG INFO ===");
        Serial.println("SSID: '" + ssid + "' (length: " + String(ssid.length()) + ")");

        WiFi.mode(WIFI_STA);
        // [FIX 5] KHÔNG dùng WiFi.disconnect(true) — sẽ dừng luôn driver
        // WiFi (esp_wifi_stop()) và làm hỏng ESP-NOW. Chỉ rời AP hiện tại.
        WiFi.disconnect(false, false);
        delay(100);

        // [FIX 4] KHÔNG gọi WiFi.onEvent() ở đây nữa — đã đăng ký 1 lần
        // duy nhất trong setup(), tránh đăng ký trùng gây sự kiện bị bắn 2 lần.
        WiFi.begin(ssid.c_str(), password.c_str());

        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
            delay(500);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            wifiMode = 1;
            espNowReady = setupEspNow();
            return;
        }
        Serial.println("❌ Không kết nối được mạng đã lưu -> chuyển sang AP mode.");
    }

    // Chuyển sang WIFI_AP_STA để ESP-NOW vẫn chạy được song song với Access Point
    WiFi.mode(WIFI_AP_STA);
    uint8_t macAddr[6];
    WiFi.softAPmacAddress(macAddr);
    String ssid_ap = "TDTHINH-" + String(macAddr[4], HEX) + String(macAddr[5], HEX);
    ssid_ap.toUpperCase();
    WiFi.softAP(ssid_ap.c_str(), NULL, 10);
    wifiMode = 0;

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

    if (message == "TkN nN jc LiTS") {
        sendText(pub_rgb, "ONRGB");
        GREEN();
        return;
    } else if (message == "TkN eF jc LiTS" || message == "eF LiT") {
        ledOff();
        sendText(pub_rgb, "OFFRGB");
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

    sendText(pub_topic, "ACK from ESP32-S3: " + message);
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
    if (mqttClient.connect("ESP32_S3_DHT_MASTER")) {
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
        case IP_EVENT_STA_GOT_IP:
            Serial.println("✅ Connected to WiFi, IP: " + WiFi.localIP().toString());
            Serial.printf("📡 Wi-Fi Channel hiện tại: %d\n", WiFi.channel());
            wifiMode = 1;
            wifiRetryCount = 0;
            staReconnectPending = false;
            GREEN();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            Serial.println("⚠️ WiFi STA bị rớt kết nối, sẽ thử lại...");
            wifiMode = 2;
            staReconnectPending = true;
            staDisconnectTime   = millis();
            break;
        default:
            break;
    }
}

// ============================================================
// WEB SERVER  —  KHÔNG YÊU CẦU ĐĂNG NHẬP (theo yêu cầu)
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
        <h3>CẤU HÌNH ESP32-S3</h3>
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

    // [FIX 2] /saveWifi giờ lưu vào Preferences (NVS) — CÙNG kho lưu trữ
    // mà setup() đọc ra khi khởi động lại — và cập nhật ngay biến toàn cục
    // ssid/password/mqtt_server trong RAM, khớp với applyWifiCredentials().
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
        webServer.send(200, "text/plain", "Đang khởi động lại ESP32-S3...");
        delay(1000);
        ESP.restart();
    });

    // Hỗ trợ captive portal: mọi route lạ redirect về "/"
    webServer.onNotFound([]() {
        webServer.sendHeader("Location", "/", true);
        webServer.send(302, "text/plain", "");
    });

    webServer.begin();
    webServerStarted = true;
    Serial.println("🌐 Web Server Config đã sẵn sàng (không cần đăng nhập)!");
}

// ============================================================
// NÚT BẤM DÙNG CHUNG (GPIO 0)
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
                    Serial.println("\n📡 [NÚT BẤM] BẬT PHÁT WiFi Config qua ESP-NOW (Cố định Kênh 1)!");
                    forceEspNowChannel(1);
                }
            } else {
                GREEN();
                Serial.println("\n🛑 [NÚT BẤM] TẮT PHÁT WiFi Config -> Trở về bình thường");
                wifiReconnectRequested = true;
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

    // Đăng ký sự kiện WiFi DUY NHẤT 1 lần ở đây (không lặp lại trong setupWifi()).
    WiFi.onEvent(WiFiEvent);

    preferences.begin("config", true);
    ssid        = preferences.getString("ssid", "");
    password    = preferences.getString("pass", "");
    mqtt_server = preferences.getString("mqtt", "192.168.1.20");
    preferences.end();

    setupWifi();

    // [FIX 1 - LỖI CHÍNH] Gọi setupWebServer() LUÔN LUÔN, dù đang ở STA
    // hay AP mode — trước đây hàm này không hề được gọi trong setup().
    setupWebServer();

    pixels.begin();
    pixels.setBrightness(50);
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
    Serial.println("🚀 Hệ thống DHT11 & Text Hub đã sẵn sàng!");
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

    if (!wifiBroadcastEnabled && staReconnectPending && millis() - staDisconnectTime > STA_RECONNECT_DELAY_MS) {
        staReconnectPending = false;
        wifiRetryCount++;
        if (wifiRetryCount > 10) {
            Serial.println("⚠️ Thử kết nối lại thất bại quá 10 lần! Chuyển về AP Mode...");
            wifiRetryCount = 0;
            setupWifi();
        } else {
            WiFi.begin(ssid.c_str(), password.c_str());
        }
    }

    if (!wifiBroadcastEnabled && wifiReconnectRequested) {
        wifiReconnectRequested = false;
        setupWifi();
    }

    if (!espNowReady && millis() - lastEspNowRetry >= ESPNOW_RETRY_INTERVAL_MS) {
        lastEspNowRetry = millis();
        espNowReady = setupEspNow();
    }

    if (wifiBroadcastEnabled && ssid.length() > 0 &&
        millis() - lastEspNowSend >= WIFI_BROADCAST_INTERVAL_MS) {
        lastEspNowSend = millis();
        sendWifiCredentialsChannel1();
    }

    if (!wifiBroadcastEnabled && wifiMode == 1) {
        if (!mqttClient.connected() && millis() - lastMqttAttempt > MQTT_RETRY_INTERVAL_MS) {
            lastMqttAttempt = millis();
            reconnectMQTT();
        }
        mqttClient.loop();
    }

    if (!wifiBroadcastEnabled && (millis() - lastDhtRead >= DHT_READ_INTERVAL_MS)) {
        lastDhtRead = millis();
        float t = dht.readTemperature();
        float h = dht.readHumidity();

        if (!isnan(t) && !isnan(h)) {
            String tempStr = "DHT11 -> Temp: " + String(t, 1) + "C | Hum: " + String(h, 1) + "%";
            sendTextdht("esp32/dht", tempStr);
        } else {
            Serial.println("❌ Lỗi đọc DHT11 định kỳ, sẽ thử lại ở chu kỳ sau (10s)");
        }
    }

    delay(1);
}
