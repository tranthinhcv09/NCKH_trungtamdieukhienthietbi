#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_arduino_version.h>
#include <ArduinoJson.h>

#define LIGHT_BUTTON_PIN   0
#define LIGHT_HOLD_MS      300

#define RED_PIN            18
#define GREEN_PIN          19
#define BLUE_PIN           21

#define ESPNOW_CHANNEL 10

#define MSG_TYPE_TEXT 0
typedef struct {
    uint8_t type;
    char    text[240];
} TextMessagePacket;

uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ===== TRẠNG THÁI MÀU =====
uint8_t R = 0, G = 255, B = 255;
bool lastTouchState = false;

// ===== Nhận ESP-NOW: copy vào buffer, xử lý ở loop() (không xử lý
// nặng trực tiếp trong callback, tránh gọi esp_now_send() lồng trong
// context nhận, có thể gây treo/không ổn định) =====
volatile bool pendingTextAvailable = false;
char          pendingTextBuffer[240];

// ===== PROTOTYPES =====
void setColor(uint8_t r, uint8_t g, uint8_t b);
bool isDarkColor(uint8_t r, uint8_t g, uint8_t b);
void handleTouchTap();
void checkLightButton();
void processCommand(const char* commandStr);
bool sendEspNowText(const char* text);
void setupEspNow();

// ============================================================
// ESP-NOW CALLBACKS
// ============================================================
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowDataRecv(const esp_now_recv_info_t*, const uint8_t* data, int len) {
#else
void onEspNowDataRecv(const uint8_t*, const uint8_t* data, int len) {
#endif
    if (len != sizeof(TextMessagePacket)) return;
    if (data[0] != MSG_TYPE_TEXT) return;

    TextMessagePacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    strncpy(pendingTextBuffer, pkt.text, sizeof(pendingTextBuffer) - 1);
    pendingTextBuffer[sizeof(pendingTextBuffer) - 1] = '\0';
    pendingTextAvailable = true;
}

// ============================================================
// KHỞI TẠO ESP-NOW THUẦN (KHÔNG WIFI, KHÔNG MQTT)
// ============================================================
void setupEspNow() {
    // Cần WiFi.mode(WIFI_STA) để có radio, nhưng KHÔNG kết nối AP nào.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(100);

    esp_err_t chErr = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.printf("📡 Ép kênh RF sang Kênh %d: %s\n",
                  ESPNOW_CHANNEL, (chErr == ESP_OK) ? "OK" : "LỖI");

    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW Init thất bại!");
        return;
    }

    esp_now_register_recv_cb(onEspNowDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastMac, 6);
    peerInfo.channel = 0; // 0 = dùng kênh RF hiện tại của radio, cho phép
                          // gửi trên nhiều kênh khác nhau khi quét kênh
    peerInfo.encrypt = false;

    if (!esp_now_is_peer_exist(broadcastMac)) {
        esp_now_add_peer(&peerInfo);
    }

    Serial.printf("✅ ESP-NOW sẵn sàng ở Kênh %d | MAC: %s\n",
                  ESPNOW_CHANNEL, WiFi.macAddress().c_str());
}

// ============================================================
// GỬI TEXT QUA ESP-NOW (đúng định dạng TextMessagePacket của hub)
// ============================================================
// Gửi bằng cách QUÉT QUA TẤT CẢ KÊNH 1–13. Lý do: kênh RF của hub không cố
// định (bám theo mạng WiFi mà hub đang kết nối), nên ta không biết chắc hub
// đang ở kênh nào tại thời điểm gửi. Gửi trên mọi kênh đảm bảo luôn "chạm"
// đúng kênh hub đang lắng nghe, dù không dùng chung kênh cố định với hub.
// Lưu ý: gói tin là broadcast nên KHÔNG có ACK thật sự để xác nhận hub đã
// nhận — hàm này chỉ đảm bảo đã phát trên mọi kênh, không đảm bảo hub nhận.
bool sendEspNowText(const char* text) {
    TextMessagePacket pkt;
    pkt.type = MSG_TYPE_TEXT;
    strncpy(pkt.text, text, sizeof(pkt.text) - 1);
    pkt.text[sizeof(pkt.text) - 1] = '\0';

    bool anySendOk = false;

    for (uint8_t ch = 1; ch <= 13; ch++) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        delay(15); // để radio ổn định trên kênh mới

        esp_err_t result = esp_now_send(broadcastMac, (uint8_t*)&pkt, sizeof(pkt));
        if (result == ESP_OK) anySendOk = true;

        delay(15); // chờ gói tin thực sự phát xong trước khi đổi kênh tiếp
    }

    // Quay lại kênh cố định để tiếp tục LẮNG NGHE bình thường
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    Serial.printf("📡 [ESP-NOW] Đã quét gửi qua 13 kênh: %s (%s)\n",
                  text, anySendOk ? "OK" : "LỖI GỬI");
    return anySendOk;
}

// ============================================================
// XỬ LÝ LỆNH NHẬN ĐƯỢC
// ============================================================
void processCommand(const char* commandStr) {
    String message = String(commandStr);
    Serial.printf("\n📩 [ESP-NOW IN]: %s\n", commandStr);

    if (message == "OFFRGB") {
        setColor(0, 0, 0);
        lastTouchState = false;
        return;
    } else if (message == "ONRGB") {
        setColor(R, G, B);
        lastTouchState = true;
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
        sendEspNowText(feedback.c_str());
    }
}

// ============================================================
// LED & NÚT BẤM
// ============================================================
bool isDarkColor(uint8_t r, uint8_t g, uint8_t b) { return ((int)r + g + b) < 50; }

void setColor(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(RED_PIN,   r);
    ledcWrite(GREEN_PIN, g);
    ledcWrite(BLUE_PIN,  b);
}

void handleTouchTap() {
    if (!lastTouchState) {
        Serial.println("👉 BẬT đèn...");
        if (isDarkColor(R, G, B)) { R = 0; G = 255; B = 255; }
        setColor(R, G, B);
        sendEspNowText("to_on");
        lastTouchState = true;
    } else {
        Serial.println("👉 TẮT đèn...");
        setColor(0, 0, 0);
        sendEspNowText("to_off");
        lastTouchState = false;
    }
}

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
        handleTouchTap();
        return;
    }

    lastState = currentState;
}

// ============================================================
// SETUP & LOOP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(2000);

    pinMode(LIGHT_BUTTON_PIN, INPUT_PULLUP);

    ledcAttach(RED_PIN,   5000, 8);
    ledcAttach(GREEN_PIN, 5000, 8);
    ledcAttach(BLUE_PIN,  5000, 8);
    setColor(0, 0, 0);

    setupEspNow();
}

void loop() {
    checkLightButton();

    if (pendingTextAvailable) {
        pendingTextAvailable = false;
        processCommand(pendingTextBuffer);
    }

    delay(1);
}
