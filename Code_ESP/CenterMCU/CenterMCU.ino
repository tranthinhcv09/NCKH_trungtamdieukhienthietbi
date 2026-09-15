// ============================================================
//  esp32s3.ino – ESP32-S3 (Arduino) – Audio TTS + Hub MQTT
//  Chức năng:
//    - Subscribe "esp32/speech"  : nhận lệnh từ main.c (IDF), phát TTS
//    - Publish   "esp32/rgb"     : điều khiển LED RGB trên esp32.ino
//    - Web server cấu hình WiFi + MQTT Broker IP (AP mode & STA mode)
//    - Lưu cấu hình vĩnh viễn vào NVS (Preferences)
//    - Phát cấu hình WiFi sang các ESP khác qua ESP-NOW
// ============================================================
// ESP32-S3 Smart Audio System
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "Audio.h"
#include <queue>
#include <Ticker.h>
#include <Preferences.h>

// ===== PIN I2S (MAX98357A) =====
#define I2S_BCLK 7
#define I2S_LRC  8
#define I2S_DOUT 9

// ===== LED RGB tích hợp ESP32-S3 (chân 48) =====
#define LED_PIN   48
#define NUMPIXELS 1
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

#define ledPin      2
#define btnPin      0 //4 5 6 
#define espNowBtnPin 0
#define PUSHTIME    5000

// ===== TOPICS MQTT =====
const char* sub_topic = "esp32/speech";  // Nhận từ main.c
const char* pub_rgb   = "esp32/rgb";     // Gửi lệnh LED cho esp32.ino
const char* pub_topic = "esp32/send";    // Gửi feedback chung

// Broadcast address gửi ESP-NOW đến tất cả
uint8_t wifiPeerAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct WifiCredentialsMessage {
    char ssid[33];
    char password[65];
    char mqtt_server[40];
};

// ===== TIMING =====
const unsigned long SPEAK_TIMEOUT_MS       = 20000; // 20s max mỗi câu TTS
const uint8_t       MQTT_MAX_RETRIES       = 30;
const unsigned long MQTT_RETRY_INTERVAL_MS = 5000;  // không spam reconnect

unsigned long speakStartTime   = 0;
unsigned long lastMqttAttempt  = 0;
unsigned long lastEspNowToggle = 0;
unsigned long lastEspNowSend   = 0;
uint8_t       mqttFailedAttempts = 0;

char        mqttServerBuf[40];
Preferences preferences;

WiFiClient   espClient;
PubSubClient mqttClient(espClient);
WebServer    webServer(80);
Ticker       blinker;
Audio        audio;

// Cấu hình mạng mặc định
String ssid     ;
String password ;
String mqtt_server ;

int  wifiMode     = 0; // 0=AP, 1=STA connected, 2=STA disconnected
unsigned long lastTimePress = 0;
unsigned long blinkTime     = 0;

// ===== TRẠNG THÁI =====
bool isSpeaking         = false;
bool espNowEnabled      = false;
bool webServerStarted   = false;
std::queue<String> speechQueue;
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

static bool lastBtn4State = LOW;

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

// ============================================================
// Web UI (giao diện cấu hình WiFi + MQTT)
// ============================================================
const char html[] PROGMEM = R"html(
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
            document.getElementById("info").innerHTML = "Đã quét xong danh sách WiFi!";
            try {
              var obj = JSON.parse(xhttp.responseText);
              var select = document.getElementById("ssid");
              select.innerHTML = '<option value="">-- Chọn WiFi trong danh sách --</option>';
              for (var i = 0; i < obj.length; ++i) {
                if (obj[i] && obj[i].length > 0) {
                  var opt = document.createElement('option');
                  opt.value = obj[i];
                  opt.innerHTML = obj[i];
                  select.appendChild(opt);
                }
              }
            } catch(e){}
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
)html";

// ============================================================
// Prototypes
// ============================================================
void enqueue(String text);
void blinkLed(uint32_t t);
void ledControl();
void WiFiEvent(WiFiEvent_t event);
String getWiFiStatusText(int status);
void setupWifi();
void setupWebServer();
void checkButton();
void clearAllCredentials();
bool setupEspNow();
bool sendWifiCredentials(const String& networkName, const String& networkPassword);
void checkEspNowButton();
void runNormalOperation();
void startAccessPoint();
void reconnectMQTT();
void connectWiFi();

// ============================================================
// Config Class
// ============================================================
class Config {
public:
    void begin();
    void run();
    void resetWifiSettings();
};
Config wifiConfig;

// ============================================================
// LED blink
// ============================================================
void blinkLed(uint32_t t) {
    if (millis() - blinkTime > t) {
        digitalWrite(ledPin, !digitalRead(ledPin));
        blinkTime = millis();
    }
}

void ledControl() {
    if (digitalRead(btnPin) == LOW) {
        blinkLed((millis() - lastTimePress < PUSHTIME) ? 1000 : 50);
    } else {
        if      (wifiMode == 0) blinkLed(50);
        else if (wifiMode == 1) blinkLed(3000);
        else                    blinkLed(300);
    }
}

// ============================================================
// WiFi Events
// ============================================================
void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case IP_EVENT_STA_GOT_IP:
            Serial.println("Connected to WiFi");
            Serial.print("IP Address: "); Serial.println(WiFi.localIP());
            wifiMode = 1;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            Serial.println("Disconnected from WiFi");
            wifiMode = 2;
            WiFi.begin(ssid.c_str(), password.c_str());
            break;
        default:
            break;
    }
}

void startAccessPoint() {
    WiFi.mode(WIFI_AP);
    uint8_t macAddr[6];
    WiFi.softAPmacAddress(macAddr);
    String ssid_ap = "TDTHINH - " + String(macAddr[4], HEX) + String(macAddr[5], HEX);
    ssid_ap.toUpperCase();
    WiFi.softAP(ssid_ap.c_str());
    Serial.println("📶 Access point name: " + ssid_ap);
    Serial.println("🌐 Web server address: " + WiFi.softAPIP().toString());
    wifiMode = 0;
}

String getWiFiStatusText(int status) {
    switch (status) {
        case WL_IDLE_STATUS:     return "WL_IDLE_STATUS";
        case WL_NO_SSID_AVAIL:   return "WL_NO_SSID_AVAIL - SSID not found";
        case WL_SCAN_COMPLETED:  return "WL_SCAN_COMPLETED";
        case WL_CONNECTED:       return "WL_CONNECTED";
        case WL_CONNECT_FAILED:  return "WL_CONNECT_FAILED - Wrong password";
        case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
        case WL_DISCONNECTED:    return "WL_DISCONNECTED";
        default:                 return "UNKNOWN: " + String(status);
    }
}

void setupWifi() {
    ssid.trim();
    password.trim();

    if (ssid.length() > 0) {
        Serial.println("=== WIFI DEBUG INFO ===");
        Serial.println("SSID: '" + ssid + "' (length: " + String(ssid.length()) + ")");
        Serial.println("MQTT Broker: " + mqtt_server);

        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true);
        delay(1000);

        WiFi.onEvent(WiFiEvent);
        WiFi.begin(ssid.c_str(), password.c_str());

        unsigned long startTime = millis();
        int dots = 0;
        while (WiFi.status() != WL_CONNECTED && millis() - startTime < 30000) {
            delay(500);
            Serial.print(".");
            if (++dots >= 10) {
                Serial.println();
                Serial.println("Status: " + getWiFiStatusText(WiFi.status()));
                dots = 0;
            }
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("✅ WiFi connected! IP: " + WiFi.localIP().toString());
            Serial.println("Signal: " + String(WiFi.RSSI()) + " dBm");
            wifiMode = 1;
            return;
        } else {
            Serial.println("❌ WiFi connection failed! → Switching to AP mode...");
            Serial.println("Final Status: " + getWiFiStatusText(WiFi.status()));
        }
    }

    Serial.println("Creating Access Point...");
    startAccessPoint();
}

// ============================================================
// ESP-NOW
// ============================================================
bool setupEspNow() {
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW initialization failed!");
        return false;
    }
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, wifiPeerAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("ESP-NOW peer registration failed!");
        esp_now_deinit();
        return false;
    }
    return true;
}

bool sendWifiCredentials(const String& networkName, const String& networkPassword) {
    if (!espNowEnabled) {
        Serial.println("ESP-NOW is off; credentials not sent.");
        return false;
    }
    WifiCredentialsMessage message = {};
    networkName.toCharArray(message.ssid, sizeof(message.ssid));
    networkPassword.toCharArray(message.password, sizeof(message.password));
    mqtt_server.toCharArray(message.mqtt_server, sizeof(message.mqtt_server));

    esp_err_t result = esp_now_send(
        wifiPeerAddress,
        reinterpret_cast<const uint8_t*>(&message),
        sizeof(message)
    );
    if (result != ESP_OK) {
        Serial.println("ESP-NOW send failed: " + String(result));
        return false;
    }
    return true;
}

void checkEspNowButton() {
    static int lastStableState = HIGH;
    static int lastFlickerState = HIGH;
    static unsigned long lastDebounceTime = 0;
    const unsigned long debounceDelay = 40;

    int currentReading = digitalRead(espNowBtnPin);

    // Nếu tín hiệu có thay đổi do rung nảy
    if (currentReading != lastFlickerState) {
        lastDebounceTime = millis();
        lastFlickerState = currentReading;
    }

    // Khi tín hiệu đã ổn định qua thời gian debounce
    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (currentReading != lastStableState) {
            lastStableState = currentReading;

            // Bắt sự kiện cạnh xuống (khi nhấn nút nối xuống GND)
            if (lastStableState == LOW) {
                espNowEnabled = !espNowEnabled;
                Serial.println(espNowEnabled ? "\n🔘 [NÚT GPIO 1] ESP-NOW: BẬT (ON)" : "\n🔘 [NÚT GPIO 1] ESP-NOW: TẮT (OFF)");

                if (espNowEnabled) {
                    if (!setupEspNow()) {
                        espNowEnabled = false;
                        Serial.println("❌ Khởi tạo ESP-NOW thất bại!");
                    } else {
                        lastEspNowSend = 0;
                        Serial.println("📡 Bắt đầu phát cấu hình WiFi & MQTT qua ESP-NOW...");
                    }
                } else {
                    esp_now_deinit();
                    Serial.println("🛑 Đã tắt ESP-NOW.");
                }
            }
        }
    }
}

// ============================================================
// Web Server
// ============================================================
void setupWebServer() {
    if (webServerStarted) return;

    webServer.on("/", []() {
        webServer.send(200, "text/html", html);
    });

    // Endpoint trả về cấu hình hiện tại (JSON) để Web UI tự động điền sẵn
    webServer.on("/getConfig", []() {
        DynamicJsonDocument doc(256);
        doc["ssid"] = ssid;
        doc["pass"] = password;
        doc["mqtt"] = mqtt_server;
        String json;
        serializeJson(doc, json);
        webServer.send(200, "application/json", json);
    });

    webServer.on("/scanWifi", []() {
        int wifi_nets = WiFi.scanNetworks(true, true);
        const unsigned long t = millis();
        while (wifi_nets < 0 && millis() - t < 10000) {
            delay(20);
            wifi_nets = WiFi.scanComplete();
        }
        DynamicJsonDocument doc(1024);
        for (int i = 0; i < wifi_nets; ++i) doc.add(WiFi.SSID(i));
        String wifiList;
        serializeJson(doc, wifiList);
        webServer.send(200, "application/json", wifiList);
    });

    webServer.on("/saveWifi", []() {
        String ssid_temp     = webServer.arg("ssid");
        String password_temp = webServer.arg("pass");
        String mqtt_temp     = webServer.arg("mqtt_server");

        ssid_temp.trim();
        password_temp.trim();
        mqtt_temp.trim();

        // Lưu toàn bộ cấu hình vào Preferences (NVS)
        preferences.begin("config", false);
        if (ssid_temp.length() > 0) {
            preferences.putString("ssid", ssid_temp);
            ssid = ssid_temp;
        }
        preferences.putString("pass", password_temp);
        password = password_temp;

        if (mqtt_temp.length() > 0) {
            preferences.putString("mqtt", mqtt_temp);
            mqtt_server = mqtt_temp;
            mqtt_server.toCharArray(mqttServerBuf, sizeof(mqttServerBuf));
            mqttClient.setServer(mqttServerBuf, 1883);
        }
        preferences.end();

        Serial.println("💾 Đã lưu cấu hình mới vào NVS:");
        Serial.println("   SSID: " + ssid);
        Serial.println("   Pass: " + password);
        Serial.println("   MQTT: " + mqtt_server);

        bool sent = sendWifiCredentials(ssid, password);
        webServer.send(200, "text/plain",
            !espNowEnabled ? "Đã lưu WiFi & MQTT thành công! (ESP-NOW đang tắt)"
            : sent ? "Đã lưu và gửi cấu hình qua ESP-NOW thành công!"
                   : "Đã lưu cấu hình, nhưng gửi ESP-NOW thất bại!");
    });

    webServer.on("/reStart", []() {
        webServer.send(200, "text/plain", "Đang khởi động lại thiết bị...");
        delay(2000);
        ESP.restart();
    });

    // Endpoint nhận câu trả lời từ Pi (TTS)
    webServer.on("/answer", HTTP_POST, []() {
        if (!webServer.hasArg("plain")) {
            webServer.send(400, "application/json", "{\"error\":\"no body\"}");
            return;
        }
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, webServer.arg("plain"))) {
            webServer.send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }
        String answer = doc["answer"] | "";
        if (answer.length() > 0) {
            Serial.println("📩 Web Answer: " + answer);
            enqueue(answer);
            webServer.send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            webServer.send(400, "application/json", "{\"error\":\"empty answer\"}");
        }
    });

    webServer.begin();
    webServerStarted = true;
}

// ============================================================
// Button (giữ 5 giây để reset WiFi & MQTT về mặc định)
// ============================================================
void checkButton() {
    if (digitalRead(btnPin) == LOW) {
        if (lastTimePress == 0) {
            lastTimePress = millis();
            Serial.println("Giữ nút 5 giây để xóa cài đặt và khôi phục mặc định!");
        } else if (millis() - lastTimePress >= PUSHTIME) {
            clearAllCredentials();
            delay(2000);
            ESP.restart();
        }
    } else {
        lastTimePress = 0;
    }
}

void clearAllCredentials() {
    preferences.begin("config", false);
    preferences.clear();
    preferences.end();
    Serial.println("✅ Toàn bộ cấu hình NVS đã được xóa sạch!");
}

// ============================================================
// Config class methods
// ============================================================
void Config::begin() {
    pinMode(ledPin, OUTPUT);
    pinMode(btnPin, INPUT_PULLUP);
    blinker.attach_ms(50, ledControl);

    // ĐỌC TOÀN BỘ CẤU HÌNH TỪ PREFERENCES (NVS) NGAY ĐẦU
    preferences.begin("config", true);
    String saved_ssid = preferences.getString("ssid", "");
    String saved_pass = preferences.getString("pass", "");
    String saved_mqtt = preferences.getString("mqtt", "");
    preferences.end();

    if (saved_ssid.length() > 0) ssid = saved_ssid;
    if (saved_pass.length() > 0) password = saved_pass;
    if (saved_mqtt.length() > 0) mqtt_server = saved_mqtt;

    Serial.println("=== CẤU HÌNH KHỞI ĐỘNG TỪ NVS ===");
    Serial.println("SSID: '" + ssid + "'");
    Serial.println("MQTT: '" + mqtt_server + "'");

    setupWifi();
    setupWebServer();
}

void Config::run() {
    checkButton();
    webServer.handleClient();
}

void Config::resetWifiSettings() {
    clearAllCredentials();
    delay(2000);
    ESP.restart();
}

// ============================================================
// MQTT Callback – Nhận lệnh từ main.c qua "esp32/speech"
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[length + 1];
    memcpy(msg, payload, length);
    msg[length] = '\0';
    String message = String(msg);

    Serial.println("\n📩 MQTT Received: " + message);

    if (String(topic) == "esp32/speech") {
        if (message == "TkN nN jc LiTS") {
            mqttClient.publish(pub_rgb, "ONRGB");
            enqueue("mở led grb");
            Serial.println("Turn on the lights");
            return;
        }  else if (message == "haVcKWfSpcN") {
            enqueue("bạn hỏi đi");
            Serial.println("have a question");
            return;
        } else if (message == "fND KWfSpcN") {
            enqueue("đã nghe");
            Serial.println("end question");
            return;
        } else if (message == "TkN eF jc LiTS") {
            enqueue("tắc led grb");
            mqttClient.publish(pub_rgb, "OFFRGB");
            Serial.println("Turn off the lights");
            return;
        } else if (message == "GRmN LiT") {
            enqueue("đèn xanh lá");
            Serial.println("green light");
            return;
        } else if (message == "RfD LiT") {
            enqueue("đèn đỏ");
            Serial.println("red light");
            return;
        } else if (message == "BLo LiT") {
            enqueue("đèn xanh dương");
            Serial.println("blue light");
            return;
        } else if (message == "eF LiT") {
            enqueue("tắc led");
            Serial.println("off light");
            return;
        } else if (message == "alexa") {
            enqueue("tôi nghe");
            Serial.println("alexa");
            return;
        } 

        // Xử lý JSON lệnh màu
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, message);

        if (!err) {
            if (doc.containsKey("r") && doc.containsKey("g") && doc.containsKey("b")) {
                uint8_t r = doc["r"].as<uint8_t>();
                uint8_t g = doc["g"].as<uint8_t>();
                uint8_t b = doc["b"].as<uint8_t>();
                setLEDColor(r, g, b);
                Serial.printf("🎨 LED S3: R=%d G=%d B=%d\n", r, g, b);
                mqttClient.publish(pub_rgb, message.c_str());
                return;
            }
            if (doc.containsKey("text")) {
                enqueue(doc["text"].as<String>());
                return;
            }
        }
        enqueue(message);
    }
}

// ============================================================
// MQTT Reconnect (có gate thời gian)
// ============================================================
void reconnectMQTT() {
    Serial.print("🔌 Connecting MQTT [");
    Serial.print(mqtt_server);
    Serial.print("] (attempt ");
    Serial.print(mqttFailedAttempts + 1);
    Serial.println(")...");

    if (mqttClient.connect("ESP32_S3_SOUND_MASTER")) {
        mqttClient.subscribe(sub_topic, 1);
        mqttFailedAttempts = 0;
        Serial.println("✅ MQTT connected!");
        return;
    }

    mqttFailedAttempts++;
    Serial.print("MQTT failed, rc="); Serial.println(mqttClient.state());

    if (mqttFailedAttempts >= MQTT_MAX_RETRIES) {
        Serial.println("⚠️ MQTT thất bại nhiều lần. Kiểm tra IP broker trong trang cấu hình Web.");
        mqttFailedAttempts = 0;
    }
}

// ============================================================
// Audio Callback
// ============================================================
void audio_eof_speech(const char *info) {
    // Chỉ tắt LED khi trong hàng đợi không còn câu nào nữa
    if (speechQueue.empty()) {
        ledOff();
    }
}

// ============================================================
// Speech Queue
// ============================================================
void enqueue(String text) {
    text.trim();
    if (text.length() == 0) return;
    if (speechQueue.size() < 15) {
        speechQueue.push(text);
    }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    pinMode(espNowBtnPin, INPUT_PULLUP);

    // 1. Khởi tạo cấu hình (Đọc SSID, Pass, MQTT từ Preferences NVS + Bật Web Server + Kết nối WiFi)
    wifiConfig.begin();

    // 2. NeoPixel
    pixels.begin();
    pixels.setBrightness(50);
    RED(); // Báo hiệu đang khởi động

    // 3. Audio I2S
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(15);

    // 4. Khởi tạo MQTT client với server IP đã load từ NVS
    mqtt_server.toCharArray(mqttServerBuf, sizeof(mqttServerBuf));
    mqttClient.setServer(mqttServerBuf, 1883);
    mqttClient.setCallback(mqttCallback);

    GREEN();
    delay(1000);
    enqueue("hệ thống đã sẵn sàng");
}

// ============================================================
// Normal Operation (chỉ chạy khi WiFi STA connected)
// ============================================================
void runNormalOperation() {
    if (wifiMode != 1) return;

    // 1. Duy trì luồng phát âm thanh liên tục
    audio.loop();

    // 2. Duy trì kết nối MQTT
    if (!mqttClient.connected() && millis() - lastMqttAttempt > MQTT_RETRY_INTERVAL_MS) {
        lastMqttAttempt = millis();
        reconnectMQTT();
    }
    mqttClient.loop();

    // 3. Khi loa RẢNH (không phát) VÀ có câu thoại trong hàng đợi -> Phát ngay
    if (!audio.isRunning() && !speechQueue.empty()) {
        String nextText = speechQueue.front();
        speechQueue.pop();

        if (nextText.length() > 0) {
            BLUE();
            Serial.println("🔊 Speaking: " + nextText);
            
            audio.stopSong(); // Dọn dẹp luồng cũ trước khi đọc câu mới
            delay(20);
            
            if (!audio.connecttospeech(nextText.c_str(), "vi")) {
                Serial.println("❌ Lỗi kết nối TTS");
                ledOff();
            }
        }
    }
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    // Nút ESP-NOW
    checkEspNowButton();

    // Nếu ESP-NOW đang bật → gửi cấu hình WiFi + MQTT định kỳ 500ms
    if (espNowEnabled && millis() - lastEspNowSend >= 500) {
        sendWifiCredentials(ssid, password);
        lastEspNowSend = millis();
    }

    // Xử lý Web server + button giữ reset
    wifiConfig.run();

    // Xử lý TTS + MQTT
    runNormalOperation();

    delay(1);
}
