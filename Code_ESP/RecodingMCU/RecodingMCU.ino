#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>      // [ESP-NOW]
#include <esp_wifi.h>     // [ESP-NOW]
#include <SPI.h>
#include <SD.h>
#include "driver/i2s_std.h"
#include <ArduinoJson.h>
#include "Audio.h"
#include <HardwareSerial.h>
#include <Preferences.h>
#include <PubSubClient.h>

Preferences preferences;

// ===== CẤU HÌNH GPIO ESP32-S3 =====
#define SD_CS        1
#define SD_MOSI      5
#define SD_SCK       4
#define SD_MISO      6

// INMP441 Microphone (I2S0 RX)
#define MIC_I2S_SCK  12
#define MIC_I2S_WS   11
#define MIC_I2S_SD   10
#define MIC_SHIFT    14
#define MIC_GAIN_X   5.0f

// MAX98357A Amplifier (I2S1 TX)
#define AMP_I2S_BCLK 15
#define AMP_I2S_LRC  9
#define AMP_I2S_DIN  8

#define SAMPLE_RATE  16000
#define WAV_FILE     "/rec.wav"

// Thông số ghi âm
#define REC_SKIP_MS  300
#define REC_TAIL_MS  2000
#define REC_MAX_MS   30000
#define REC_MIN_BYTES 9600

// UART
#define UART2_TX_PIN 18
#define UART2_RX_PIN 17
#define UART_BAUD    115200

// Định dạng lệnh màu gửi sang mạch LED
#define LED_COLOR_FMT "R%d G%d B%d\n"

HardwareSerial mySerial(2);

// ===== CONFIG NETWORK =====
char ssid[33]       = "PTIT.HCM_SV";
char password[65]   = "";
char serverIp[40]   = "10.251.7.232";
const int serverPort = 5000;
const char* uploadPath = "/voice";

// ===== MQTT =====
const int   mqttPort   = 1883;
#define MQTT_TOPIC "esp32/gemini"
WiFiClient   mqttNet;
PubSubClient mqtt(mqttNet);
String pendingSpeech = "";

// ===== [ESP-NOW] KÊNH & PEER =====
uint8_t espnowChannel = 0;                                   // 0 = chưa có kênh lưu trong NVS
uint8_t peerMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // broadcast, đổi thành MAC của mạch LED nếu cần
uint32_t lastChanCheck = 0;

// ===== LƯU TRẠNG THÁI MÀU GẦN NHẤT =====
int lastR = 255;
int lastG = 255;
int lastB = 255;

// ===== CHỐNG GHI ĐÈ MÀU BỞI LỆNH "BẬT ĐÈN" CHUNG =====
uint32_t lastColorMs = 0;      // thời điểm nhận màu cụ thể gần nhất
uint32_t lastLedSendMs = 0;    // thời điểm gửi LED gần nhất
int lastSentR = -1, lastSentG = -1, lastSentB = -1;
bool pendingOn = false;        // đang chờ xử lý lệnh to_on
uint32_t pendingOnAt = 0;

SPIClass sdSPI(FSPI);

Audio audio(I2S_NUM_1);

i2s_chan_handle_t rx_handle = NULL;

// ===== TRẠNG THÁI GHI ÂM =====
File recFile;
bool isRecording = false;
volatile bool recRun = false;
volatile bool recTaskDone = true;
volatile uint32_t recBytes = 0;
volatile int16_t recPeak = 0;
uint32_t recStartMs = 0;

bool pendingStart = false;
uint32_t pendingStartAt = 0;
uint32_t stopAt = 0;

// ===== NVS =====
void loadWiFiConfig() {
  preferences.begin("wifi_config", true);
  String s = preferences.getString("ssid", "PTIT.HCM_SV");
  String p = preferences.getString("pass", "");
  String i = preferences.getString("ip",   "10.251.7.232");

  lastR = preferences.getInt("lastR", 255);
  lastG = preferences.getInt("lastG", 255);
  lastB = preferences.getInt("lastB", 255);
  espnowChannel = preferences.getUChar("chan", 0);   // [ESP-NOW] kênh đã lưu
  preferences.end();

  s.toCharArray(ssid, sizeof(ssid));
  p.toCharArray(password, sizeof(password));
  i.toCharArray(serverIp, sizeof(serverIp));
  Serial.printf("📌 NVS -> SSID: %s | IP: %s | Màu gần nhất: R%d G%d B%d | Kênh: %d\n",
                ssid, serverIp, lastR, lastG, lastB, espnowChannel);
}

void saveWiFiConfig(String newSsid, String newPass, String newIp) {
  preferences.begin("wifi_config", false);
  preferences.putString("ssid", newSsid);
  preferences.putString("pass", newPass);
  preferences.putString("ip", newIp);
  preferences.remove("chan");                        // [ESP-NOW] đổi WiFi -> bỏ kênh cũ, quét lại
  preferences.end();

  newSsid.toCharArray(ssid, sizeof(ssid));
  newPass.toCharArray(password, sizeof(password));
  newIp.toCharArray(serverIp, sizeof(serverIp));
  espnowChannel = 0;
  Serial.println("💾 Đã lưu cấu hình WiFi mới vào NVS!");
}

void saveLastColor(int r, int g, int b) {
  // Bỏ qua màu tắt (R,G,B < 40)
  if (r < 40 && g < 40 && b < 40) return;

  lastR = r;
  lastG = g;
  lastB = b;

  preferences.begin("wifi_config", false);
  preferences.putInt("lastR", lastR);
  preferences.putInt("lastG", lastG);
  preferences.putInt("lastB", lastB);
  preferences.end();
  Serial.printf("💾 Đã lưu màu gần nhất: R%d G%d B%d\n", lastR, lastG, lastB);
}

// ===== [ESP-NOW] LƯU KÊNH + CHẠY KÊNH MỚI =====
void saveEspNowChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return;
  espnowChannel = ch;
  preferences.begin("wifi_config", false);
  preferences.putUChar("chan", ch);
  preferences.end();
  Serial.printf("💾 Đã lưu kênh ESP-NOW: %d\n", ch);
}

void initEspNow() {
  esp_now_deinit();                                  // an toàn khi gọi lại
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init lỗi");
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, 6);
  peer.channel = 0;                                  // 0 = dùng kênh hiện tại của WiFi
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&peer);
  Serial.printf("📡 ESP-NOW chạy trên kênh %d\n", WiFi.channel());
}

// Kênh WiFi đổi -> lưu + chạy lại ESP-NOW trên kênh mới
void syncEspNowChannel() {
  if (WiFi.status() != WL_CONNECTED) return;
  uint8_t cur = WiFi.channel();
  if (cur == espnowChannel) return;

  Serial.printf("🔁 Đổi kênh ESP-NOW: %d -> %d\n", espnowChannel, cur);
  saveEspNowChannel(cur);
  initEspNow();
  mySerial.printf("{\"channel\":%d}\n", cur);       // báo mạch LED (bỏ nếu mạch không cần)
}

// ===== MIC =====
bool startMic() {
  if (rx_handle != NULL) return true;

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = 8;
  chan_cfg.dma_frame_num = 512;

  if (i2s_new_channel(&chan_cfg, NULL, &rx_handle) != ESP_OK) {
    Serial.println("❌ i2s_new_channel lỗi");
    rx_handle = NULL;
    return false;
  }

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)MIC_I2S_SCK,
      .ws   = (gpio_num_t)MIC_I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)MIC_I2S_SD,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
    }
  };

  if (i2s_channel_init_std_mode(rx_handle, &std_cfg) != ESP_OK ||
      i2s_channel_enable(rx_handle) != ESP_OK) {
    Serial.println("❌ Không khởi tạo được I2S mic");
    i2s_del_channel(rx_handle);
    rx_handle = NULL;
    return false;
  }
  return true;
}

void stopMic() {
  if (rx_handle != NULL) {
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    rx_handle = NULL;
  }
}

// ===== WAV =====
void writeLE16(uint8_t *b, uint16_t v) { b[0] = v & 0xFF; b[1] = v >> 8; }
void writeLE32(uint8_t *b, uint32_t v) {
  b[0] = v & 0xFF; b[1] = (v >> 8); b[2] = (v >> 16); b[3] = (v >> 24);
}

void makeWavHeader(uint8_t *h, uint32_t dataSize) {
  memcpy(h, "RIFF", 4);
  writeLE32(h + 4, 36 + dataSize);
  memcpy(h + 8, "WAVEfmt ", 8);
  writeLE32(h + 16, 16);
  writeLE16(h + 20, 1);
  writeLE16(h + 22, 1);
  writeLE32(h + 24, SAMPLE_RATE);
  writeLE32(h + 28, SAMPLE_RATE * 2);
  writeLE16(h + 32, 2);
  writeLE16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  writeLE32(h + 40, dataSize);
}

// ===== UPLOAD HTTP =====
String uploadAudioAndGetText(const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return "";

  WiFiClient httpClient;
  if (!httpClient.connect(serverIp, serverPort)) {
    f.close();
    return "";
  }

  String boundary = "----ESP32S3Boundary";
  String bodyStart = "--" + boundary +
                     "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"rec.wav\""
                     "\r\nContent-Type: audio/wav\r\n\r\n";
  String bodyEnd = "\r\n--" + boundary + "--\r\n";
  size_t totalLen = bodyStart.length() + f.size() + bodyEnd.length();

  httpClient.printf(
      "POST %s HTTP/1.1\r\nHost: %s\r\n"
      "Content-Type: multipart/form-data; boundary=%s\r\n"
      "Content-Length: %u\r\nConnection: close\r\n\r\n",
      uploadPath, serverIp, boundary.c_str(), (unsigned)totalLen
  );

  httpClient.print(bodyStart);
  uint8_t buffer[1024];
  while (f.available()) {
    int n = f.read(buffer, sizeof(buffer));
    if (n <= 0) break;
    httpClient.write(buffer, n);
  }
  httpClient.print(bodyEnd);
  f.close();

  unsigned long timeout = millis();
  while (httpClient.connected() && !httpClient.available()) {
    if (millis() - timeout > 10000) { httpClient.stop(); return ""; }
    delay(10);
  }

  while (httpClient.connected()) {
    String line = httpClient.readStringUntil('\n');
    if (line == "\r") break;
  }

  String responseBody = httpClient.readString();
  httpClient.stop();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, responseBody);
  if (!error) {
    const char* answer = doc["answer"];
    if (answer) return String(answer);
  }
  return "";
}

// ===== CHUYỂN RGB SANG TÊN MÀU TIẾNG VIỆT =====
String rgbToColorName(int r, int g, int b) {
  if (r > 200 && g > 200 && b > 200) return "màu trắng";
  if (r < 40 && g < 40 && b < 40)     return "màu đen";
  if (r > 200 && g < 50 && b < 50)   return "màu đỏ";
  if (r < 50 && g > 200 && b < 50)   return "màu xanh lá";
  if (r < 50 && g < 50 && b > 200)   return "màu xanh dương";
  if (r > 200 && g > 200 && b < 50)  return "màu vàng";
  if (r > 200 && g > 80 && b < 50)   return "màu cam";
  if (r > 150 && g < 50 && b > 150)  return "màu tím";
  if (r > 200 && g > 100 && b > 150) return "màu hồng";
  if (r < 50 && g > 180 && b > 180)  return "màu xanh ngọc";

  if (r >= g && r >= b) {
    if (g > 100) return "màu cam";
    return "màu đỏ";
  } else if (g >= r && g >= b) {
    return "màu xanh lá";
  } else {
    return "màu xanh dương";
  }
}

// Gửi màu sang mạch LED (có chống gửi lặp trong 1.5 giây)
void sendColorToLed(int r, int g, int b) {
  if (r == lastSentR && g == lastSentG && b == lastSentB &&
      millis() - lastLedSendMs < 1500) return;

  lastSentR = r; lastSentG = g; lastSentB = b;
  lastLedSendMs = millis();

  mySerial.printf(LED_COLOR_FMT, r, g, b);
  Serial.printf("💡 Gửi LED: R%d G%d B%d\n", r, g, b);
}

// Áp dụng một màu cụ thể: lưu + gửi LED + nói
void applyColor(int r, int g, int b) {
  if (r < 40 && g < 40 && b < 40) {
    Serial.println("🔊 tắt đèn");
    audio.connecttospeech("tắt đèn", "vi");
    return;
  }

  saveLastColor(r, g, b);
  lastColorMs = millis();
  pendingOn = false;                 // huỷ lệnh to_on đang chờ
  sendColorToLed(r, g, b);           // luôn gửi màu cụ thể sang LED

  String speechText = "Đã bật đèn " + rgbToColorName(r, g, b);
  Serial.printf("🔊 %s\n", speechText.c_str());
  audio.connecttospeech(speechText.c_str(), "vi");
}

// Bật lại màu đã lưu: gửi LED + nói (dùng cho to_on)
void restoreLastColor(const String &prefix) {
  String colorName = rgbToColorName(lastR, lastG, lastB);
  String speechText = prefix + colorName;

  Serial.printf("🔊 %s (R%d G%d B%d)\n", speechText.c_str(), lastR, lastG, lastB);
  sendColorToLed(lastR, lastG, lastB);
  audio.connecttospeech(speechText.c_str(), "vi");
}

// ===== MQTT CALLBACK =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  if (msg.startsWith("{")) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg);
    if (!err && doc.containsKey("answer")) {
      const char* a = doc["answer"];
      if (a) msg = String(a);
    }
  }

  if (msg.length() > 0) {
    Serial.printf("📨 MQTT [%s]: %s\n", topic, msg.c_str());
    if (pendingSpeech.length() > 0) pendingSpeech += "\n";   // nối thêm, không ghi đè tin trước
    pendingSpeech += msg;
  }
}

void mqttEnsureConnected() {
  if (mqtt.connected()) return;
  static uint32_t lastTry = 0;
  if (millis() - lastTry < 5000) return;
  lastTry = millis();

  String id = "esp32s3-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(id.c_str())) {
    mqtt.subscribe(MQTT_TOPIC);
    Serial.println("✅ MQTT đã kết nối, subscribe " MQTT_TOPIC);
  } else {
    Serial.printf("⚠️ MQTT lỗi, rc=%d\n", mqtt.state());
  }
}

// ===== TASK GHI ÂM =====
void recordTask(void *param) {
  static int32_t i2s_buf[512];
  static int16_t out_buf[1024];
  size_t outCount = 0;

  float last_input = 0, last_output = 0;
  uint32_t skipFrames = (uint32_t)SAMPLE_RATE * REC_SKIP_MS / 1000;

  while (recRun) {
    size_t bytesRead = 0;
    esp_err_t e = i2s_channel_read(rx_handle, i2s_buf, sizeof(i2s_buf), &bytesRead, 100);
    if (e != ESP_OK || bytesRead == 0) continue;

    int frames = bytesRead / 8;
    for (int f = 0; f < frames; f++) {
      float cur = (float)(i2s_buf[f * 2] >> MIC_SHIFT);

      float filtered = cur - last_input + 0.95f * last_output;
      last_input = cur;
      last_output = filtered;

      if (skipFrames > 0) { skipFrames--; continue; }

      int32_t amp = (int32_t)(filtered * MIC_GAIN_X);
      if (amp > 32767)  amp = 32767;
      if (amp < -32768) amp = -32768;

      int16_t a = (amp < 0) ? -amp : amp;
      if (a > recPeak) recPeak = a;

      out_buf[outCount++] = (int16_t)amp;
    }

    if (outCount > 768) {
      recFile.write((uint8_t*)out_buf, outCount * 2);
      recBytes += outCount * 2;
      outCount = 0;
    }
  }

  if (outCount > 0) {
    recFile.write((uint8_t*)out_buf, outCount * 2);
    recBytes += outCount * 2;
  }

  recTaskDone = true;
  vTaskDelete(NULL);
}

// ===== BẮT ĐẦU GHI =====
void startRecordingProcess() {
  if (isRecording) return;

  Serial.println("\n🔴 Bắt đầu ghi âm...");

  recFile = SD.open(WAV_FILE, FILE_WRITE);
  if (!recFile) {
    Serial.println("❌ Không tạo được file trên thẻ SD!");
    return;
  }

  uint8_t header[44];
  makeWavHeader(header, 0);
  recFile.write(header, 44);

  if (!startMic()) {
    recFile.close();
    return;
  }

  recBytes = 0;
  recPeak = 0;
  recRun = true;
  recTaskDone = false;
  isRecording = true;
  recStartMs = millis();
  stopAt = 0;

  xTaskCreatePinnedToCore(recordTask, "recTask", 6144, NULL, 5, NULL, 1);
}

// ===== NÓI VÀ CHỜ NÓI XONG =====
void speakAndWait(const char *text) {
  audio.connecttospeech(text, "vi");
  uint32_t t0 = millis();
  delay(200);
  while (audio.isRunning() && millis() - t0 < 8000) {
    audio.loop();
    delay(1);
  }
}

// ===== CALLBACK LOG CỦA THƯ VIỆN AUDIO (hiện lỗi TTS nếu có) =====
void audio_info(const char *info) {
  Serial.print("ℹ️ audio_info: ");
  Serial.println(info);
}
void audio_eof_speech(const char *info) {
  Serial.print("✅ Đọc xong: ");
  Serial.println(info);
}

// ===== HÀNG ĐỢI NÓI: CHIA NHỎ CÂU DÀI (Google TTS giới hạn ~200 ký tự) =====
#define TTS_CHUNK_MAX    150   // byte, thấp hơn giới hạn cho an toàn (tiếng Việt có dấu 2-3 byte/ký tự)
#define SPEECH_QUEUE_MAX 12

String speechQueue[SPEECH_QUEUE_MAX];
int sqHead = 0, sqTail = 0;
bool speechActive = false;
uint32_t speechStartedAt = 0;

bool sqEmpty() { return sqHead == sqTail; }

void sqPush(const String &s) {
  int next = (sqTail + 1) % SPEECH_QUEUE_MAX;
  if (next == sqHead) {
    Serial.println("⚠️ Hàng đợi nói đầy, bỏ đoạn cuối");
    return;
  }
  speechQueue[sqTail] = s;
  sqTail = next;
}

// Tách văn bản thành các đoạn <= TTS_CHUNK_MAX, ưu tiên cắt ở cuối câu
void enqueueSpeech(const String &text) {
  String buf = "";
  int n = text.length();
  int i = 0;

  while (i < n) {
    // tìm cuối câu (không cắt ở dấu chấm thập phân như 3.5)
    int j = i;
    while (j < n) {
      char c = text[j];
      bool isEnd = (c == '!' || c == '?' || c == '\n' || c == ';' ||
                    (c == '.' && !(j > 0 && j + 1 < n && isDigit(text[j - 1]) && isDigit(text[j + 1]))));
      j++;
      if (isEnd) break;
    }
    String sent = text.substring(i, j);
    i = j;
    sent.trim();
    if (sent.length() == 0) continue;

    // câu quá dài: cắt tại khoảng trắng
    while ((int)sent.length() > TTS_CHUNK_MAX) {
      int cut = sent.lastIndexOf(' ', TTS_CHUNK_MAX);
      if (cut <= 0) {
        cut = TTS_CHUNK_MAX;
        while (cut > 0 && (((uint8_t)sent[cut]) & 0xC0) == 0x80) cut--;   // không cắt giữa ký tự UTF-8
        if (cut <= 0) cut = TTS_CHUNK_MAX;
      }
      String part = sent.substring(0, cut);
      part.trim();
      if (buf.length() > 0) { sqPush(buf); buf = ""; }
      if (part.length() > 0) sqPush(part);
      sent = sent.substring(cut);
      sent.trim();
    }
    if (sent.length() == 0) continue;

    // gộp các câu ngắn vào một đoạn
    if (buf.length() + 1 + sent.length() > TTS_CHUNK_MAX) {
      if (buf.length() > 0) sqPush(buf);
      buf = sent;
    } else {
      buf = (buf.length() > 0) ? (buf + " " + sent) : sent;
    }
  }
  if (buf.length() > 0) sqPush(buf);
}

// Gọi mỗi vòng loop(): đọc đoạn tiếp theo khi đoạn trước đã xong
void speechPump() {
  if (isRecording || pendingStart) return;

  if (speechActive) {
    bool finished = (millis() - speechStartedAt > 800) && !audio.isRunning();
    bool timeout  = (millis() - speechStartedAt > 20000);
    if (!finished && !timeout) return;
    speechActive = false;
  }

  if (sqEmpty()) return;

  String s = speechQueue[sqHead];
  speechQueue[sqHead] = "";
  sqHead = (sqHead + 1) % SPEECH_QUEUE_MAX;

  Serial.printf("🗣️ TTS (%d byte): %s\n", (int)s.length(), s.c_str());
  bool ok = audio.connecttospeech(s.c_str(), "vi");
  if (!ok) Serial.println("⚠️ connecttospeech trả về false (không kết nối được TTS)");
  speechActive = true;
  speechStartedAt = millis();
}

// ===== DỪNG GHI + UPLOAD =====
void stopRecordingProcess() {
  if (!isRecording) return;

  isRecording = false;
  recRun = false;

  uint32_t t0 = millis();
  while (!recTaskDone && millis() - t0 < 1000) delay(5);

  stopMic();

  if (recFile) {
    uint8_t header[44];
    makeWavHeader(header, recBytes);
    recFile.seek(0);
    recFile.write(header, 44);
    recFile.close();
  }

  Serial.printf("⬛ Đã dừng ghi âm | %u byte (~%.1f s) | Peak: %d/32767\n",
                (unsigned)recBytes, recBytes / (SAMPLE_RATE * 2.0f), (int)recPeak);

  if (recBytes < REC_MIN_BYTES) {
    Serial.println("⚠️ File ghi âm quá ngắn/trống!");
    return;
  }

  speakAndWait("kết thúc câu hỏi");

  Serial.println("🌐 Upload WAV...");
  uploadAudioAndGetText(WAV_FILE);
  Serial.println("⏳ Chờ câu trả lời từ MQTT...");
}

// ===== UART: CHỈ NHẬN CẤU HÌNH WIFI (dùng khi chưa nối được WiFi) =====
#define UART_BOOT_WAIT_MS 2000

void pollUartWifiConfig() {
  if (mySerial.available() <= 0) return;

  String line = mySerial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  Serial.print("📥 [UART] ");
  Serial.println(line);

  if (!(line.startsWith("{") && line.endsWith("}"))) return;

  JsonDocument doc;
  if (deserializeJson(doc, line)) return;

  if (doc.containsKey("ssid") && doc.containsKey("pass")) {
    const char* n_ssid = doc["ssid"];
    const char* n_pass = doc["pass"];
    const char* n_mqtt = doc["mqtt"];
    if (!n_ssid || !n_pass) return;
    saveWiFiConfig(String(n_ssid), String(n_pass), String(n_mqtt ? n_mqtt : serverIp));
    Serial.println("🔄 Khởi động lại để áp dụng WiFi mới...");
    delay(1000);
    ESP.restart();
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);

  pinMode(UART2_RX_PIN, INPUT_PULLUP);
  mySerial.begin(UART_BAUD, SERIAL_8N1, UART2_RX_PIN, UART2_TX_PIN);
  mySerial.setTimeout(200);          // readStringUntil không chặn quá lâu

  loadWiFiConfig();

  // ƯU TIÊN UART: nghe cấu hình WiFi mới trước khi nối WiFi
  Serial.printf("📥 Chờ cấu hình WiFi qua UART %d ms...\n", UART_BOOT_WAIT_MS);
  uint32_t tUart = millis();
  while (millis() - tUart < UART_BOOT_WAIT_MS) {
    pollUartWifiConfig();
    delay(10);
  }

  // Nối WiFi như code gốc (quét tự động mọi kênh), có in trạng thái + tự thử lại
  // Kênh đã lưu chỉ dùng để so sánh/theo dõi, KHÔNG ép router vào kênh đó nữa
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  uint32_t lastRetry = millis();
  uint32_t lastLog = 0;
  while (WiFi.status() != WL_CONNECTED) {
    pollUartWifiConfig();            // vẫn nghe UART trong lúc chờ WiFi
    delay(10);

    if (millis() - lastLog > 500) {
      lastLog = millis();
      Serial.printf("⏳ Đang nối WiFi... status=%d\n", (int)WiFi.status());
    }

    // Quá 15 giây chưa nối được -> reset hẳn WiFi rồi thử lại từ đầu
    if (millis() - lastRetry > 15000) {
      Serial.println("🔁 Reset WiFi và thử lại...");
      WiFi.disconnect(true, true);
      delay(300);
      WiFi.mode(WIFI_OFF);
      delay(300);
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
      lastRetry = millis();
    }
  }
  Serial.printf("\n🌐 WiFi sẵn sàng! IP: %s | Kênh: %d\n",
                WiFi.localIP().toString().c_str(), WiFi.channel());

  // [ESP-NOW] Khởi tạo ESP-NOW; nếu kênh khác bản lưu thì lưu kênh mới
  initEspNow();
  syncEspNowChannel();

  mqtt.setServer(serverIp, mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(2048);
  mqtt.setKeepAlive(60);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 20000000)) {
    Serial.println("❌ Thẻ SD: Không tìm thấy hoặc lỗi gắn!");
  } else {
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("💾 Thẻ SD sẵn sàng! Dung lượng: %llu MB\n", cardSize);
  }

  audio.setPinout(AMP_I2S_BCLK, AMP_I2S_LRC, AMP_I2S_DIN);
  audio.setVolume(15);
  Serial.println("🔊 Loa MAX98357A: Đã cấu hình chân I2S");

  Serial.println("🎤 Đang kiểm tra Microphone...");
  if (startMic()) Serial.println("✅ Mic OK");
  stopMic();

  Serial.println("\n=== Sẵn sàng ===");
  mySerial.println("ready");
}

// ===== LOOP =====
void handleUart();

void loop() {
  handleUart();          // ƯU TIÊN: xử lý UART trước mọi việc khác
  audio.loop();

  mqttEnsureConnected();
  mqtt.loop();

  // [ESP-NOW] Cứ 3 giây kiểm tra kênh WiFi, đổi thì lưu + chạy lại ESP-NOW
  if (millis() - lastChanCheck > 3000) {
    lastChanCheck = millis();
    syncEspNowChannel();
  }

  // Xử lý lệnh "bật đèn" chung sau khi chờ xem có màu cụ thể đi kèm không
  if (pendingOn && millis() >= pendingOnAt) {
    pendingOn = false;
    if (!isRecording) restoreLastColor("bật đèn ");
  }

  if (pendingSpeech.length() > 0 && !isRecording && !pendingStart) {
    String text = pendingSpeech;
    pendingSpeech = "";
    Serial.printf("💬 Server: %s\n", text.c_str());
    mySerial.println(text);
    enqueueSpeech(text);
  }

  speechPump();

  if (pendingStart && millis() >= pendingStartAt && !audio.isRunning()) {
    pendingStart = false;
    startRecordingProcess();
  }

  if (isRecording && stopAt == 0 && millis() - recStartMs > REC_MAX_MS) {
    Serial.println("⏱️ Quá thời gian tối đa, tự dừng");
    stopAt = millis();
  }

  if (stopAt != 0 && millis() >= stopAt) {
    stopAt = 0;
    stopRecordingProcess();
  }

  delay(1);
}

// ===== NHẬN LỆNH UART TỪ THIẾT BỊ NGOẠI VI =====
void handleUart() {
  if (mySerial.available() > 0) {
    String dataIn = mySerial.readStringUntil('\n');
    dataIn.trim();

    if (dataIn.length() > 0) {
      Serial.print("📥 [UART] ");
      Serial.println(dataIn);

      // A. JSON từ UART
      if (dataIn.startsWith("{") && dataIn.endsWith("}")) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, dataIn);
        if (!err) {
          if (doc.containsKey("ssid") && doc.containsKey("pass")) {
            const char* n_ssid = doc["ssid"];
            const char* n_pass = doc["pass"];
            const char* n_mqtt = doc["mqtt"];
            saveWiFiConfig(String(n_ssid), String(n_pass), String(n_mqtt ? n_mqtt : serverIp));
            Serial.println("🔄 Khởi động lại để áp dụng WiFi mới...");
            delay(1000);
            ESP.restart();
          }
          else if (doc.containsKey("r") && doc.containsKey("g") && doc.containsKey("b")) {
            if (!isRecording) {
              applyColor(doc["r"], doc["g"], doc["b"]);
            }
          }
        }
      }
      // B. Lệnh "alexa"
      else if (dataIn.startsWith("alexa")) {
        if (!isRecording) {
          audio.connecttospeech("em nghe", "vi");
        }
      }
      else if (dataIn == "haVcKWfSpcN") {            // Bắt đầu hỏi
        if (!isRecording && !pendingStart) {
          audio.connecttospeech("bạn hỏi đi", "vi");
          pendingStart = true;
          pendingStartAt = millis() + 500;
        }
      }
      else if (dataIn == "fNDKWfSpcN") {             // Kết thúc hỏi
        if (isRecording && stopAt == 0) {
          stopAt = millis() + REC_TAIL_MS;
        } else if (pendingStart) {
          pendingStart = false;
        }
      }
      // Lệnh bật đèn chung -> chờ 400ms, nếu không có màu cụ thể thì bật lại màu đã lưu
      else if (dataIn.startsWith("to_on") || dataIn == "ONRGB") {
        if (!isRecording && millis() - lastColorMs > 1500) {
          pendingOn = true;
          pendingOnAt = millis() + 400;
        }
      }
      else if (dataIn.startsWith("to_off") || dataIn == "OFFRGB") {
        pendingOn = false;
        if (!isRecording) {
          Serial.println("🔊 tắt đèn");
          audio.connecttospeech("tắt đèn", "vi");
        }
      }
      // C. Chuỗi màu "R.. G.. B.." (phải bắt đầu bằng R/r)
      else if (dataIn.startsWith("R") || dataIn.startsWith("r")) {
        if (!isRecording) {
          int r = 0, g = 0, b = 0;
          if (sscanf(dataIn.c_str(), "R%d G%d B%d", &r, &g, &b) == 3 ||
              sscanf(dataIn.c_str(), "r%d g%d b%d", &r, &g, &b) == 3) {
            applyColor(r, g, b);
          }
        }
      }
      else if (dataIn.startsWith("fan_off")) {
        if (!isRecording) {
          Serial.println("🔊 tắt quạt");
          audio.connecttospeech("tắt quạt", "vi");
        }
      }
      else if (dataIn.startsWith("fan_on_25")) {
        if (!isRecording) {
          Serial.println("🔊 bật quạt mức 25%");
          audio.connecttospeech("bật quạt mức 25", "vi");
        }
      }
      else if (dataIn.startsWith("fan_on_50")) {
        if (!isRecording) {
          Serial.println("🔊 bật quạt mức 50%");
          audio.connecttospeech("bật quạt mức 50", "vi");
        }
      }
      else if (dataIn.startsWith("fan_on_75")) {
        if (!isRecording) {
          Serial.println("🔊 bật quạt mức 75%");
          audio.connecttospeech("bật quạt mức 75", "vi");
        }
      }
      else if (dataIn.startsWith("fan_on_100")) {
        if (!isRecording) {
          Serial.println("🔊 bật quạt mức 100%");
          audio.connecttospeech("bật quạt mức 100", "vi");
        }
      }
      else if (dataIn.startsWith("fan_on")) {
        if (!isRecording) {
          Serial.println("🔊 bật quạt");
          audio.connecttospeech("bật quạt", "vi");
        }
      }
    }
  }
}
