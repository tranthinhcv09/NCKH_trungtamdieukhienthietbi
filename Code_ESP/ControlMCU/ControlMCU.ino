#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_arduino_version.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ============================================================
// PIN & CẤU HÌNH CẢM ỨNG (TTP223B) & THIẾT BỊ
// ============================================================
#define TOUCH_LED_PIN      4
#define TOUCH_FAN_PIN      15

#define FAN_MOSFET_PIN     22

#define TOUCH_DEBOUNCE_MS  50
#define HOLD_INTERVAL_MS   1000

#define RED_PIN            18
#define GREEN_PIN          19
#define BLUE_PIN           21

// Nút BOOT tích hợp trên mạch ESP32 (GPIO 0)
#define BOOT_BUTTON_PIN    0
#define SCAN_HOLD_MS       5000 // Giữ > 5 giây để dò lại kênh

// Ngưỡng "màu tối" (thống nhất với board voice: R,G,B đều < 40 là tắt)
#define DARK_THRESHOLD     40

// Sau bao lâu kể từ lần đổi màu cuối thì mới ghi NVS (tránh mòn flash)
#define COLOR_SAVE_DELAY_MS 3000

// ============================================================
// LƯU TRỮ KÊNH RF
// ============================================================
Preferences preferences;
uint8_t currentChannel = 1; 
bool channelFound = false;

// ============================================================
// ESP-NOW PACKET
// ============================================================
#define MSG_TYPE_TEXT 0

typedef struct {
    uint8_t type;
    char text[240];
} TextMessagePacket;

// ============================================================
// BROADCAST
// ============================================================
uint8_t broadcastMac[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF
};

// ============================================================
// TRẠNG THÁI LED & QUẠT
// ============================================================
uint8_t R = 255, G = 255, B = 255;   // màu gần nhất (mặc định trắng, khớp board voice)
bool ledOn = false;
bool fanOn = false;
uint8_t fanSpeedPercent = 50;

// Lưu màu xuống NVS có trì hoãn
bool colorDirty = false;
uint32_t colorDirtyAt = 0;

// ============================================================
// ESP-NOW RX QUEUE
// ============================================================
#define RX_QUEUE_SIZE 20

struct ReceivedMessage {
    char text[240];
};

QueueHandle_t espNowRxQueue = NULL;

volatile uint32_t rxPacketCount = 0;
volatile uint32_t rxQueueDropCount = 0;

// ============================================================
// PROTOTYPES
// ============================================================
void setColor(uint8_t r, uint8_t g, uint8_t b);
bool isDarkColor(uint8_t r, uint8_t g, uint8_t b);
bool parseRgbText(const char* s, int &r, int &g, int &b);
void applyRgb(int r, int g, int b);
void flushColorToNvs();
void applyFanSpeed(uint8_t percent);
void handleLedTap();
void handleFanTap();
void checkLedTouchSensor();
void checkFanTouchSensor();
void checkBootButton();
void processCommand(const char* commandStr);
bool sendEspNowText(const char* text);
void setupEspNow();
void processEspNowQueue();
void scanAndSaveChannel();

// ============================================================
// ESP-NOW CALLBACK
// ============================================================
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowDataRecv(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len) {
#else
void onEspNowDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
    if (data == nullptr || len < 1) return;
    if (data[0] != MSG_TYPE_TEXT) return;
    if (len > sizeof(TextMessagePacket)) return;

    ReceivedMessage msg = {};
    int textLen = len - 1;
    if (textLen <= 0) return;
    if (textLen >= sizeof(msg.text)) textLen = sizeof(msg.text) - 1;

    memcpy(msg.text, data + 1, textLen);
    msg.text[textLen] = '\0';

    if (xQueueSend(espNowRxQueue, &msg, 0) == pdTRUE) {
        rxPacketCount++;
    } else {
        rxQueueDropCount++;
    }
}

// ============================================================
// DÒ VÀ LƯU KÊNH (CHỐNG CRASH)
// ============================================================
void scanAndSaveChannel() {
    Serial.println("\n🔍 [RF SCAN] Đang quét toàn bộ các kênh (1-13)...");
    channelFound = false;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);

    esp_now_deinit();
    delay(50);

    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW Init thất bại trong lúc Scan!");
        return;
    }

    esp_now_register_recv_cb(onEspNowDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastMac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (!esp_now_is_peer_exist(broadcastMac)) {
        esp_now_add_peer(&peerInfo);
    }

    for (uint8_t ch = 1; ch <= 13; ch++) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        Serial.printf("📡 Đang thử Kênh %d...\n", ch);

        xQueueReset(espNowRxQueue);

        sendEspNowText("PING_SCAN");

        unsigned long startWait = millis();
        while (millis() - startWait < 200) {
            ReceivedMessage msg;
            if (xQueueReceive(espNowRxQueue, &msg, 0) == pdTRUE) {
                Serial.printf("✅ [RF SCAN SUCCESS] Tìm thấy tín hiệu ở Kênh %d: %s\n", ch, msg.text);
                currentChannel = ch;
                channelFound = true;
                processCommand(msg.text);
                break;
            }
            delay(10);
        }

        if (channelFound) break;
    }

    if (channelFound) {
        preferences.begin("espnow_cfg", false);
        preferences.putUInt("channel", currentChannel);
        preferences.end();
        Serial.printf("💾 Đã lưu Kênh %d vào Flash NVS thành công!\n", currentChannel);
    } else {
        Serial.println("⚠️ Không phát hiện kênh nào gửi dữ liệu. Giữ nguyên kênh cũ!");
        if (currentChannel > 0) {
            esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
        }
    }
}

// ============================================================
// KHỞI TẠO ESP-NOW
// ============================================================
void setupEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(50);

    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW Init thất bại!");
        return;
    }

    esp_now_register_recv_cb(onEspNowDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastMac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (!esp_now_is_peer_exist(broadcastMac)) {
        esp_now_add_peer(&peerInfo);
    }

    Serial.printf("✅ ESP-NOW sẵn sàng ở Kênh %d | MAC: %s\n", currentChannel, WiFi.macAddress().c_str());
}

// ============================================================
// GỬI TEXT QUA ESP-NOW
// ============================================================
bool sendEspNowText(const char* text) {
    TextMessagePacket pkt = {};
    pkt.type = MSG_TYPE_TEXT;
    strncpy(pkt.text, text, sizeof(pkt.text) - 1);
    pkt.text[sizeof(pkt.text) - 1] = '\0';

    esp_err_t result = esp_now_send(broadcastMac, (uint8_t*)&pkt, sizeof(pkt));
    return (result == ESP_OK);
}

// ============================================================
// XỬ LÝ QUEUE ESP-NOW
// ============================================================
void processEspNowQueue() {
    ReceivedMessage msg;
    while (xQueueReceive(espNowRxQueue, &msg, 0) == pdTRUE) {
        processCommand(msg.text);
    }
}

// ============================================================
// ĐIỀU KHIỂN LED & MÀU
// ============================================================
bool isDarkColor(uint8_t r, uint8_t g, uint8_t b) {
    return (r < DARK_THRESHOLD && g < DARK_THRESHOLD && b < DARK_THRESHOLD);
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(RED_PIN, r);
    ledcWrite(GREEN_PIN, g);
    ledcWrite(BLUE_PIN, b);
}

// Đọc chuỗi dạng "R255 G0 B6" (hoặc chữ thường)
bool parseRgbText(const char* s, int &r, int &g, int &b) {
    return sscanf(s, "R%d G%d B%d", &r, &g, &b) == 3 ||
           sscanf(s, "r%d g%d b%d", &r, &g, &b) == 3;
}

// Áp dụng màu: màu tối = tắt đèn nhưng KHÔNG ghi đè màu đã lưu
void applyRgb(int r, int g, int b) {
    r = constrain(r, 0, 255);
    g = constrain(g, 0, 255);
    b = constrain(b, 0, 255);

    if (isDarkColor(r, g, b)) {
        ledOn = false;
        setColor(0, 0, 0);
        return;
    }

    R = r; G = g; B = b;
    ledOn = true;
    setColor(R, G, B);

    colorDirty = true;            // lưu NVS sau COLOR_SAVE_DELAY_MS
    colorDirtyAt = millis();
}

// Gọi trong loop(): ghi màu xuống flash khi đã ổn định
void flushColorToNvs() {
    if (!colorDirty) return;
    if (millis() - colorDirtyAt < COLOR_SAVE_DELAY_MS) return;

    colorDirty = false;
    preferences.begin("espnow_cfg", false);
    preferences.putUChar("R", R);
    preferences.putUChar("G", G);
    preferences.putUChar("B", B);
    preferences.end();
    Serial.printf("💾 Đã lưu màu vào NVS: R%d G%d B%d\n", R, G, B);
}

// ============================================================
// QUẠT
// ============================================================
void applyFanSpeed(uint8_t percent) {
    if (!fanOn || percent == 0) {
        ledcWrite(FAN_MOSFET_PIN, 0);
        digitalWrite(FAN_MOSFET_PIN, LOW); // Đảm bảo ngắt hoàn toàn MOSFET
    } else {
        if (percent == 100) {
            // Khi 100%, xuất mức HIGH trực tiếp giúp MOSFET mở tối đa
            digitalWrite(FAN_MOSFET_PIN, HIGH);
            ledcWrite(FAN_MOSFET_PIN, 255);
        } else {
            uint32_t pwmVal = map(percent, 0, 100, 0, 255);
            ledcWrite(FAN_MOSFET_PIN, pwmVal);
        }
    }
}

// ============================================================
// XỬ LÝ LỆNH NHẬN
// ============================================================
void processCommand(const char* commandStr) {
    if (commandStr == nullptr || commandStr[0] == '\0') return;

    Serial.printf("\n📩 [ESP-NOW IN]: %s\n", commandStr);

    // ========================================================
    // RGB OFF
    // ========================================================
    if (strcmp(commandStr, "OFFRGB") == 0) {
        setColor(0, 0, 0);
        ledOn = false;
        Serial.println("💡 [CMD] TẮT Đèn LED");
        return;
    }

    // ========================================================
    // RGB ON (bật lại màu gần nhất)
    // ========================================================
    if (strcmp(commandStr, "ONRGB") == 0) {
        if (isDarkColor(R, G, B)) {
            R = 255; G = 255; B = 255; // phòng hờ, mặc định trắng
        }
        setColor(R, G, B);
        ledOn = true;
        Serial.printf("💡 [CMD] BẬT Đèn LED (R:%d G:%d B:%d)\n", R, G, B);
        return;
    }

    // ========================================================
    // ĐỔI MÀU LED THEO MÃ GIỌNG NÓI ESP-NOW
    // ========================================================
    if (strcmp(commandStr, "WiT LiT") == 0) {
        applyRgb(255, 255, 255); // Trắng (255, 255, 255)
        Serial.println("💡 [CMD] Nhận mã WiT LiT -> BẬT Đèn LED Trắng");
        sendEspNowText("Đã bật LED màu trắng");
        return;
    } else if (strcmp(commandStr, "GRmN LiT") == 0) {
        applyRgb(0, 255, 0); // Xanh lá (0, 255, 0)
        Serial.println("💡 [CMD] Nhận mã GRmN LiT -> BẬT Đèn Xanh Lá");
        sendEspNowText("Đã bật LED màu xanh lá");
        return;
    } else if (strcmp(commandStr, "RfD LiT") == 0) {
        applyRgb(255, 0, 0); // Đỏ (255, 0, 0)
        Serial.println("💡 [CMD] Nhận mã RfD LiT -> BẬT Đèn Đỏ");
        sendEspNowText("Đã bật LED màu đỏ");
        return;
    } else if (strcmp(commandStr, "BLo LiT") == 0) {
        applyRgb(0, 0, 255); // Xanh dương (0, 0, 255)
        Serial.println("💡 [CMD] Nhận mã BLo LiT -> BẬT Đèn Xanh Dương");
        sendEspNowText("Đã bật LED màu xanh dương");
        return;
    } else if (strcmp(commandStr, "YfLb LiT") == 0) {
        applyRgb(255, 255, 0); // Vàng (255, 255, 0)
        Serial.println("💡 [CMD] Nhận mã YfLb LiT -> BẬT Đèn Vàng");
        sendEspNowText("Đã bật LED màu vàng");
        return;
    } else if (strcmp(commandStr, "eRcNq LiT") == 0) {
        applyRgb(255, 128, 0); // Cam (255, 128, 0)
        Serial.println("💡 [CMD] Nhận mã eRcNq LiT -> BẬT Đèn Cam");
        sendEspNowText("Đã bật LED màu cam");
        return;
    } else if (strcmp(commandStr, "PglK LiT") == 0) {
        applyRgb(128, 0, 128); // Tím (128, 0, 128)
        Serial.println("💡 [CMD] Nhận mã PglK LiT -> BẬT Đèn Tím");
        sendEspNowText("Đã bật LED màu tím");
        return;
    } else if (strcmp(commandStr, "PkPcL LiT") == 0) {
        applyRgb(255, 20, 147); // Hồng (255, 20, 147)
        Serial.println("💡 [CMD] Nhận mã PkPcL LiT -> BẬT Đèn Hồng");
        sendEspNowText("Đã bật LED màu hồng");
        return;
    }

    // ========================================================
    // FAN OFF / ON (LỆNH THUẦN)
    // ========================================================
    if (strcmp(commandStr, "fan_off") == 0) {
        fanOn = false;
        applyFanSpeed(0);
        Serial.println("🌀 [CMD] TẮT Quạt");
        return;
    }

    if (strcmp(commandStr, "fan_on") == 0) {
        fanOn = true;
        applyFanSpeed(fanSpeedPercent);
        Serial.printf("🌀 [CMD] BẬT Quạt (%d%%)\n", fanSpeedPercent);
        return;
    }

    // ========================================================
    // CHUỖI MÀU "R255 G0 B6"
    // ========================================================
    int tr = 0, tg = 0, tb = 0;
    if (parseRgbText(commandStr, tr, tg, tb)) {
        applyRgb(tr, tg, tb);
        Serial.printf("🎨 [TEXT] Đổi màu LED: R=%d, G=%d, B=%d | Trạng thái: %s\n",
                      R, G, B, ledOn ? "ON" : "OFF");
        return;
    }

    // ========================================================
    // JSON RGB (ĐỔI MÀU LED TỰ ĐỘNG BẬT LED)
    // ========================================================
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, commandStr);

    if (!err && doc.containsKey("r") && doc.containsKey("g") && doc.containsKey("b")) {
        int jr = doc["r"] | 0;
        int jg = doc["g"] | 0;
        int jb = doc["b"] | 0;

        applyRgb(jr, jg, jb);

        Serial.printf("🎨 [JSON] Đổi màu LED: R=%d, G=%d, B=%d | Trạng thái: %s\n",
                      jr, jg, jb, ledOn ? "ON" : "OFF");

        char feedback[100];
        snprintf(feedback, sizeof(feedback), "Đã đổi màu: R%d G%d B%d", jr, jg, jb);
        sendEspNowText(feedback);

        return;
    }

    // ========================================================
    // FAN SPEED & ON THEO % (HỖ TRỢ: fan_speed_X HOẶC fan_on_X)
    // ========================================================
    int speed = -1;

    if (strncmp(commandStr, "fan_speed_", 10) == 0) {
        speed = atoi(commandStr + 10);
    } else if (strncmp(commandStr, "fan_on_", 7) == 0) {
        speed = atoi(commandStr + 7);
    }

    if (speed == 25 || speed == 50 || speed == 75 || speed == 100) {
        fanSpeedPercent = speed;
        fanOn = true;
        applyFanSpeed(fanSpeedPercent);
        Serial.printf("🌀 [CMD] Tốc độ quạt: %d%%\n", fanSpeedPercent);
    }
}

// ============================================================
// CẢM ỨNG LED & QUẠT
// ============================================================
void handleLedTap() {
    if (!ledOn) {
        ledOn = true;
        if (isDarkColor(R, G, B)) { R = 255; G = 255; B = 255; }
        setColor(R, G, B);
        sendEspNowText("to_on");
        Serial.println("on led");
    } else {
        ledOn = false;
        setColor(0, 0, 0);
        sendEspNowText("to_off");
        Serial.println("off led");
    }
}

void checkLedTouchSensor() {
    static unsigned long touchStartTime = 0;
    static bool isTouching = false;

    bool rawState = (digitalRead(TOUCH_LED_PIN) == HIGH);

    if (rawState && !isTouching) {
        isTouching = true;
        touchStartTime = millis();
    }

    if (!rawState && isTouching) {
        if (millis() - touchStartTime >= TOUCH_DEBOUNCE_MS) {
            handleLedTap();
        }
        isTouching = false;
    }
}

void handleFanTap() {
    if (!fanOn) {
        fanOn = true;
        applyFanSpeed(fanSpeedPercent);
        char msg[32];
        snprintf(msg, sizeof(msg), "fan_on_%d", fanSpeedPercent);
        sendEspNowText(msg);
        Serial.println(msg);
    } else {
        fanOn = false;
        applyFanSpeed(0);
        sendEspNowText("fan_off");
        Serial.println("fan off");
    }
}

void checkFanTouchSensor() {
    static unsigned long touchStartTime = 0;
    static unsigned long lastStepTime = 0;
    static bool isTouching = false;
    static bool isHolding = false;

    bool rawState = (digitalRead(TOUCH_FAN_PIN) == HIGH);

    if (rawState && !isTouching) {
        isTouching = true;
        isHolding = false;
        touchStartTime = millis();
        lastStepTime = millis();
    }

    if (rawState && isTouching) {
        if (millis() - touchStartTime >= HOLD_INTERVAL_MS) {
            isHolding = true;
            if (millis() - lastStepTime >= HOLD_INTERVAL_MS) {
                lastStepTime = millis();
                if (!fanOn) fanOn = true;
                fanSpeedPercent += 25;
                if (fanSpeedPercent > 100) fanSpeedPercent = 25;

                applyFanSpeed(fanSpeedPercent);
                char msg[32];
                snprintf(msg, sizeof(msg), "fan_speed_%d", fanSpeedPercent);
                sendEspNowText(msg);
            }
        }
    }

    if (!rawState && isTouching) {
        if (!isHolding && (millis() - touchStartTime >= TOUCH_DEBOUNCE_MS)) {
            handleFanTap();
        }
        isTouching = false;
        isHolding = false;
    }
}

// ============================================================
// KIỂM TRA GIỮ NÚT BOOT (GPIO 0) > 5 GIÂY
// ============================================================
void checkBootButton() {
    static unsigned long pressStartTime = 0;
    static bool isPressing = false;
    static bool scanExecuted = false;

    bool isBootPressed = (digitalRead(BOOT_BUTTON_PIN) == LOW);

    if (isBootPressed && !isPressing) {
        isPressing = true;
        scanExecuted = false;
        pressStartTime = millis();
    }

    if (isBootPressed && isPressing) {
        if (millis() - pressStartTime >= SCAN_HOLD_MS && !scanExecuted) {
            scanExecuted = true;
            Serial.println("\n🔘 [BOOT BUTTON] Giữ nút BOOT > 5 giây -> Dò lại kênh RF!");
            scanAndSaveChannel();
        }
    }

    if (!isBootPressed && isPressing) {
        isPressing = false;
        scanExecuted = false;
    }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(TOUCH_LED_PIN, INPUT);
    pinMode(TOUCH_FAN_PIN, INPUT);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    
    // Cấu hình OUTPUT cho chân điều khiển MOSFET quạt
    pinMode(FAN_MOSFET_PIN, OUTPUT);

    ledcAttach(RED_PIN, 5000, 8);
    ledcAttach(GREEN_PIN, 5000, 8);
    ledcAttach(BLUE_PIN, 5000, 8);
    setColor(0, 0, 0);

    // PWM 1kHz cho quạt
    ledcAttach(FAN_MOSFET_PIN, 1000, 8);
    applyFanSpeed(0);

    // Nạp lại màu LED gần nhất từ Flash
    preferences.begin("espnow_cfg", true);
    R = preferences.getUChar("R", 255);
    G = preferences.getUChar("G", 255);
    B = preferences.getUChar("B", 255);
    preferences.end();
    if (isDarkColor(R, G, B)) { R = 255; G = 255; B = 255; }
    Serial.printf("🎨 Màu LED đã lưu: R%d G%d B%d\n", R, G, B);

    espNowRxQueue = xQueueCreate(RX_QUEUE_SIZE, sizeof(ReceivedMessage));

    preferences.begin("espnow_cfg", true);
    currentChannel = preferences.getUInt("channel", 0);
    preferences.end();

    if (currentChannel == 0) {
        scanAndSaveChannel();
    } else {
        Serial.printf("💾 Sử dụng kênh đã lưu trong Flash: Kênh %d\n", currentChannel);
        setupEspNow();
    }
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    checkLedTouchSensor();
    checkFanTouchSensor();
    checkBootButton();
    processEspNowQueue();
    flushColorToNvs();

    delay(1);
}
