
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_board_init.h"
#include "model_path.h"
#include "esp_process_sdkconfig.h"
#include "driver/rmt.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "led_strip.h"
#include "cJSON.h"
#include <stdbool.h>

// ===================== CẤU HÌNH KÊNH ESP-NOW (CHUNG 1 KÊNH) =====================
#define ESP_NOW_DEFAULT_CHANNEL  1
#define ESP_NOW_CHANNEL_MIN      1
#define ESP_NOW_CHANNEL_MAX      13
#define CHANNEL_HOP_DWELL_MS     300    // Thời gian dừng lại ở mỗi kênh khi quét
#define CHANNEL_SCAN_TIMEOUT_MS  30000  // Tự động huỷ quét nếu quá 30s không tìm thấy Config

// KÊNH CỐ ĐỊNH KHI KHÔNG CÓ WIFI — PHẢI TRÙNG ESPNOW_OFFLINE_CHANNEL BÊN ESP32 DEVKIT
#define ESP_NOW_OFFLINE_CHANNEL  1
#define WIFI_MAX_RETRY           3        // số lần tự nối lại ngay khi rớt WiFi
#define WIFI_RETRY_INTERVAL_MS   120000   // dò lại router mỗi 120s khi offline (0 = tắt)

#define MQTT_PORT               1883
#define MQTT_TOPIC              "esp32/speech"
#define MQTT_TOPIC_AUDIO        "esp32/audio"

#define LED_GPIO                48
#define LED_COUNT               2
#define ESP_NOW_BUTTON_GPIO     GPIO_NUM_0

// ===================== CẤU HÌNH UART DÂY =====================
#define UART_PORT_NUM           UART_NUM_1
#define UART_TX_PIN              GPIO_NUM_17
#define UART_RX_PIN              GPIO_NUM_18
#define UART_BAUD_RATE           115200
#define UART_BUF_SIZE            1024

// ===================== STRUCT ĐỒNG BỘ ARDUINO =====================
#define ESPNOW_MSG_WIFI_CONFIG  0xA1
#define ESPNOW_MSG_TEXT         0x00

typedef struct __attribute__((packed)) {
    uint8_t type;           // 0xA1
    char ssid[33];
    char password[65];
    char mqtt_server[40];
} wifi_credentials_message_t;

typedef struct __attribute__((packed)) {
    uint8_t type;           // 0x00
    char text[240];
} esp_now_text_message_t;

// Gói tin đưa vào queue: config nhận được + kênh đã tìm thấy nó
typedef struct {
    wifi_credentials_message_t msg;
    uint8_t found_channel;
} config_queue_item_t;

// Text nhận từ ESP-NOW: callback chỉ đẩy vào queue, task riêng mới in/gửi UART
typedef struct {
    char text[241];
} text_queue_item_t;

static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ===================== BIẾN TOÀN CỤC & MUTEX =====================
static led_strip_t *led_strip = NULL;
static TaskHandle_t s_button_task_handle = NULL;
static QueueHandle_t g_config_queue = NULL;
static QueueHandle_t g_text_queue = NULL;
static SemaphoreHandle_t g_config_mutex = NULL;

static char g_wifi_ssid[33] = {0};
static char g_wifi_pass[65] = {0};
static char g_mqtt_host[40] = {0};
static uint8_t g_esp_now_channel = ESP_NOW_DEFAULT_CHANNEL;
static bool config_loaded = false;

static volatile bool esp_now_active = true;      // false = tạm khoá ESP-NOW để ưu tiên nghe Alexa
static volatile bool esp_now_rx_mode = false;    // true = đang ở chế độ QUÉT tìm config
static volatile bool reconnecting = false;
static volatile bool s_wifi_connecting = false;  // true = đang trong connect_wifi_blocking (không ép kênh)
static volatile bool is_question_mode = false;   // true = đang hỏi đáp -> tạm không chuyển text ESP-NOW ra UART

// Task quét kênh
static TaskHandle_t s_channel_hop_task_handle = NULL;
static volatile bool s_channel_hop_active = false;

static SemaphoreHandle_t s_esp_now_send_sem = NULL;
static volatile bool s_esp_now_last_send_ok = false;

/* WiFi & Event Group */
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
static int wifi_retry_count = 0;

/* MQTT */
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

/* Speech */
static int detect_flag = 0;
static esp_afe_sr_iface_t *afe_handle = NULL;
static volatile int task_flag = 0;
static srmodel_list_t *models = NULL;

// Forward declarations
static void mqtt_init(void);
static bool connect_wifi_blocking(uint32_t timeout_ms);
static bool esp_now_publish_text(const char *text);
static void publish_text_priority(const char *topic, const char *text);
static void set_esp_now_channel(uint8_t channel);
static void set_esp_now_active(bool enabled);
static void restore_esp_now_idle_channel(void);
static void uart_send_text(const char *data);
static void channel_hop_task(void *arg);
static void start_channel_scan(void);
static void stop_channel_scan(void);
static void resume_normal_mode_after_scan(void);

/* ===================== UART HARDWARE & TASK ===================== */
static void uart_hardware_init(void) {
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void uart_send_text(const char *data) {
    if (data == NULL) return;
    uart_write_bytes(UART_PORT_NUM, data, strlen(data));
    uart_write_bytes(UART_PORT_NUM, "\n", 1);
    printf("📤 [UART TX WIRE]: %s\n", data);
}

static void uart_rx_task(void *arg) {
    uint8_t *data = (uint8_t *) malloc(UART_BUF_SIZE);
    char rx_buf[256];
    int rx_idx = 0;

    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE - 1, pdMS_TO_TICKS(50));
        if (len < 0) {
            uart_flush_input(UART_PORT_NUM);
            rx_idx = 0;
            continue;
        }
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                if (data[i] >= 32 && data[i] <= 126) {
                    if (rx_idx < sizeof(rx_buf) - 1) {
                        rx_buf[rx_idx++] = (char)data[i];
                    }
                } else if (data[i] == '\n' || data[i] == '\r') {
                    if (rx_idx > 0) {
                        rx_buf[rx_idx] = '\0';
                        printf("📥 [UART RX TEXT CHUẨN]: %s\n", rx_buf);
                        rx_idx = 0;
                    }
                }
            }
        }
    }
    free(data);
    vTaskDelete(NULL);
}

/* ===================== LED ===================== */
static void led_init(void) {
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(LED_GPIO, RMT_CHANNEL_0);
    config.clk_div = 2;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(LED_COUNT, (led_strip_dev_t)config.channel);
    led_strip = led_strip_new_rmt_ws2812(&strip_config);
    if (!led_strip) return;

    ESP_ERROR_CHECK(led_strip->clear(led_strip, 100));
    ESP_ERROR_CHECK(led_strip->refresh(led_strip, 100));
}

static void led_set_color(uint8_t r, uint8_t g, uint8_t b) {
    if (!led_strip) return;
    led_strip->set_pixel(led_strip, 0, r, g, b);
    led_strip->refresh(led_strip, 100);
}

static void led_off(void) {
    if (!led_strip) return;
    led_strip->clear(led_strip, 100);
    led_strip->refresh(led_strip, 100);
}

/* ===================== NVS CONFIG ===================== */
static bool load_config_from_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open("net_cfg", NVS_READONLY, &handle) != ESP_OK) {
        printf("NVS chưa có cấu hình. Bấm GPIO0 để quét kênh nhận Config từ Arduino.\n");
        return false;
    }

    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    size_t len_ssid = sizeof(g_wifi_ssid);
    size_t len_pass = sizeof(g_wifi_pass);
    size_t len_mqtt = sizeof(g_mqtt_host);

    esp_err_t err = nvs_get_str(handle, "ssid", g_wifi_ssid, &len_ssid);
    err |= nvs_get_str(handle, "pass", g_wifi_pass, &len_pass);
    err |= nvs_get_str(handle, "mqtt", g_mqtt_host, &len_mqtt);

    uint8_t saved_channel = ESP_NOW_DEFAULT_CHANNEL;
    if (nvs_get_u8(handle, "chan", &saved_channel) == ESP_OK &&
        saved_channel >= ESP_NOW_CHANNEL_MIN && saved_channel <= ESP_NOW_CHANNEL_MAX) {
        g_esp_now_channel = saved_channel;
    } else {
        g_esp_now_channel = ESP_NOW_DEFAULT_CHANNEL;
    }

    nvs_close(handle);

    if (err != ESP_OK || g_wifi_ssid[0] == '\0' || g_mqtt_host[0] == '\0') {
        memset(g_wifi_ssid, 0, sizeof(g_wifi_ssid));
        memset(g_wifi_pass, 0, sizeof(g_wifi_pass));
        memset(g_mqtt_host, 0, sizeof(g_mqtt_host));
        xSemaphoreGive(g_config_mutex);
        return false;
    }

    printf("Loaded NVS Config: SSID=%s |PASS=%s| MQTT=%s | CHANNEL=%d\n",
           g_wifi_ssid, g_wifi_pass, g_mqtt_host, g_esp_now_channel);
    xSemaphoreGive(g_config_mutex);
    return true;
}

static void save_config_to_nvs(const char* ssid, const char* pass, const char* mqtt, uint8_t channel) {
    nvs_handle_t handle;
    if (nvs_open("net_cfg", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "ssid", ssid);
        nvs_set_str(handle, "pass", pass);
        nvs_set_str(handle, "mqtt", mqtt);
        nvs_set_u8(handle, "chan", channel);
        nvs_commit(handle);
        nvs_close(handle);
        printf("💾 Đã lưu cấu hình mới (kênh %d) từ Arduino vào NVS thành công!\n", channel);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "ssid", ssid);
        cJSON_AddStringToObject(root, "pass", pass);
        cJSON_AddStringToObject(root, "mqtt", mqtt);
        cJSON_AddNumberToObject(root, "channel", channel);

        char *json_string = cJSON_PrintUnformatted(root);
        if (json_string != NULL) {
            uart_send_text(json_string);
            printf("UART: %s\n", json_string);
            free(json_string);
        }
        cJSON_Delete(root);
    }
}

/* ===================== QUÉT KÊNH (CHANNEL HOP) KHI Ở CHẾ ĐỘ NHẬN CONFIG =====================
   FIX: có TIMEOUT. Nếu quét quá CHANNEL_SCAN_TIMEOUT_MS mà không tìm thấy Config,
   tự động huỷ và quay lại chế độ bình thường - tránh kẹt esp_now_rx_mode = true
   mãi mãi khiến detect_Task bỏ qua hoàn toàn việc nghe "alexa". */
static void channel_hop_task(void *arg) {
    uint8_t ch = ESP_NOW_CHANNEL_MIN;
    TickType_t start_tick = xTaskGetTickCount();
    printf("🔎 Bắt đầu QUÉT toàn bộ kênh ESP-NOW (1..13) để tìm Config (tối đa %d giây)...\n",
           CHANNEL_SCAN_TIMEOUT_MS / 1000);

    while (s_channel_hop_active) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

        led_set_color(255, 255, 0);
        vTaskDelay(pdMS_TO_TICKS(CHANNEL_HOP_DWELL_MS / 2));
        led_off();
        vTaskDelay(pdMS_TO_TICKS(CHANNEL_HOP_DWELL_MS / 2));

        ch++;
        if (ch > ESP_NOW_CHANNEL_MAX) ch = ESP_NOW_CHANNEL_MIN;

        if (s_channel_hop_active &&
            (xTaskGetTickCount() - start_tick) >= pdMS_TO_TICKS(CHANNEL_SCAN_TIMEOUT_MS)) {
            printf("⏰ Quét kênh quá %d giây không tìm thấy Config -> TỰ HUỶ, quay lại chờ 'alexa'.\n",
                   CHANNEL_SCAN_TIMEOUT_MS / 1000);
            s_channel_hop_active = false;
        }
    }

    printf("🔎 Dừng quét kênh.\n");
    led_off();

    s_channel_hop_task_handle = NULL;

    // Luôn reset về chế độ chờ Alexa sau khi quét kết thúc dù do timeout hay người dùng huỷ.
    esp_now_rx_mode = false;
    set_esp_now_active(false);
    resume_normal_mode_after_scan();

    vTaskDelete(NULL);
}

static void start_channel_scan(void) {
    if (s_channel_hop_task_handle != NULL) return; // đã chạy rồi
    s_channel_hop_active = true;
    xTaskCreate(channel_hop_task, "chan_hop", 2560, NULL, 4, &s_channel_hop_task_handle);
}

static void stop_channel_scan(void) {
    if (s_channel_hop_task_handle == NULL) return;
    s_channel_hop_active = false;
    while (s_channel_hop_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Dùng chung cho cả "huỷ thủ công" (nút bấm) và "tự huỷ do timeout"
static void resume_normal_mode_after_scan(void) {
    set_esp_now_channel(g_esp_now_channel);

    if (config_loaded && !reconnecting) {
        reconnecting = true;
        if (connect_wifi_blocking(15000)) {
            mqtt_init();
        }
        reconnecting = false;
    }
}

/* ===================== WORKER TASK & ESP-NOW ===================== */
static void net_reconnect_worker_task(void *arg) {
    config_queue_item_t item;

    while (true) {
        if (xQueueReceive(g_config_queue, &item, portMAX_DELAY) == pdTRUE) {
            printf("\n📩 [KÊNH %d] Đã nhận Config từ Sender! SSID: %s | MQTT: %s\n",
                   item.found_channel, item.msg.ssid, item.msg.mqtt_server);

            stop_channel_scan();

            save_config_to_nvs(item.msg.ssid, item.msg.password, item.msg.mqtt_server, item.found_channel);

            xSemaphoreTake(g_config_mutex, portMAX_DELAY);
            snprintf(g_wifi_ssid, sizeof(g_wifi_ssid), "%s", item.msg.ssid);
            snprintf(g_wifi_pass, sizeof(g_wifi_pass), "%s", item.msg.password);
            snprintf(g_mqtt_host, sizeof(g_mqtt_host), "%s", item.msg.mqtt_server);
            g_esp_now_channel = item.found_channel;
            config_loaded = true;
            xSemaphoreGive(g_config_mutex);

            esp_now_rx_mode = false;
            set_esp_now_channel(g_esp_now_channel);

            led_set_color(128, 0, 128); // LED TÍM: Đang nối WiFi/MQTT

            if (connect_wifi_blocking(15000)) {
                mqtt_init();
                led_off();
            } else {
                printf("❌ WiFi nối thất bại sau khi nhận Config. Vẫn chạy ESP-NOW kênh %d, chờ 'alexa'.\n",
                       g_esp_now_channel);
                led_off();
            }
            reconnecting = false;
            printf("✅ Đã quay lại chế độ bình thường, chờ lệnh 'alexa'.\n");
        }
    }
}

// FIX: không ép esp_wifi_set_channel() khi STA đã kết nối AP — driver sẽ
// từ chối lệnh này (hoặc làm rớt kết nối) vì kênh phải theo AP lúc đó.
static void set_esp_now_channel(uint8_t channel) {
    bool sta_connected = wifi_event_group &&
        (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;

    (void)channel; // kênh router đã lưu KHÔNG dùng cho offline: DevKit rớt về kênh cố định
    if (!sta_connected && !s_wifi_connecting) {
        esp_wifi_set_channel(ESP_NOW_OFFLINE_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }

    if (esp_now_is_peer_exist(s_broadcast_mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, s_broadcast_mac, 6);
        peer.channel = 0; // 0 = luôn dùng kênh hiện tại của radio
        peer.encrypt = false;
        peer.ifidx = ESP_IF_WIFI_STA;
        esp_now_mod_peer(&peer);
    }
    printf("🔄 ESP-NOW: %s\n", sta_connected ? "theo kênh router hiện tại" : "kênh OFFLINE cố định");
}

static void set_esp_now_active(bool enabled) {
    if (enabled == esp_now_active) return;
    esp_now_active = enabled;
    if (enabled) {
        printf("📡 ESP-NOW: BẬT lại để gửi/nhận dữ liệu.\n");
        set_esp_now_channel(g_esp_now_channel);
    } else {
        printf("🔇 ESP-NOW: TẮT để tập trung nghe Alexa.\n");
    }
}

static void restore_esp_now_idle_channel(void) {
    if (esp_now_rx_mode) return;
    set_esp_now_channel(g_esp_now_channel);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void esp_now_send_cb(const uint8_t *mac_addr, const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
#else
static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
#endif
{
    (void)mac_addr;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    (void)tx_info;
#endif
    s_esp_now_last_send_ok = (status == ESP_NOW_SEND_SUCCESS);
    if (s_esp_now_send_sem) {
        xSemaphoreGive(s_esp_now_send_sem);
    }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
#else
static void esp_now_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
#endif
{
    if (data == NULL || len <= 0) return;

    uint8_t type = data[0];

    if (type == ESPNOW_MSG_WIFI_CONFIG && len == (int)sizeof(wifi_credentials_message_t)) {
        if (!esp_now_rx_mode) return;

        config_queue_item_t item;
        memcpy(&item.msg, data, sizeof(item.msg));
        item.msg.ssid[sizeof(item.msg.ssid) - 1] = '\0';
        item.msg.password[sizeof(item.msg.password) - 1] = '\0';
        item.msg.mqtt_server[sizeof(item.msg.mqtt_server) - 1] = '\0';

        uint8_t cur_chan = ESP_NOW_DEFAULT_CHANNEL;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&cur_chan, &second);
        item.found_channel = cur_chan;

        // Callback chạy trong task WiFi (không phải ISR) -> dùng xQueueSend, không FromISR
        xQueueSend(g_config_queue, &item, 0);
        return;
    }

    if (type == ESPNOW_MSG_TEXT && len == (int)sizeof(esp_now_text_message_t)) {
        // Luôn nhận text (kể cả khi chưa nói "alexa"); chỉ bỏ qua khi quét config / đang hỏi đáp
        if (esp_now_rx_mode || is_question_mode) return;
        const esp_now_text_message_t *tmsg = (const esp_now_text_message_t *)data;
        text_queue_item_t qi;
        snprintf(qi.text, sizeof(qi.text), "%.*s", (int)sizeof(tmsg->text), tmsg->text);
        // Không printf / UART trong callback: chỉ đẩy queue để task text_rx xử lý
        if (g_text_queue) xQueueSend(g_text_queue, &qi, 0);
    }
}

static void text_rx_task(void *arg) {
    text_queue_item_t qi;
    while (true) {
        if (xQueueReceive(g_text_queue, &qi, portMAX_DELAY) == pdTRUE) {
            printf("📥 [ESP-NOW RX TEXT] %s\n", qi.text);
            uart_send_text(qi.text);
        }
    }
}

static esp_err_t esp_now_init_config(void) {
    esp_err_t err = esp_now_init();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) return err;

    err = esp_now_register_recv_cb(esp_now_recv_cb);
    if (err != ESP_OK) return err;

    err = esp_now_register_send_cb(esp_now_send_cb);
    if (err != ESP_OK) return err;

    uint8_t init_channel = g_esp_now_channel;

    if (!esp_now_is_peer_exist(s_broadcast_mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, s_broadcast_mac, 6);
        peer.channel = 0; // 0 = theo kênh hiện tại của radio
        peer.encrypt = false;
        peer.ifidx = ESP_IF_WIFI_STA;
        err = esp_now_add_peer(&peer);
        if (err != ESP_OK) return err;
    }

    set_esp_now_channel(init_channel);
    return ESP_OK;
}

static bool esp_now_publish_text(const char *text) {
    if (!text || !esp_now_active || esp_now_rx_mode || !s_esp_now_send_sem) return false;

    if (esp_now_is_peer_exist(s_broadcast_mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, s_broadcast_mac, 6);
        peer.channel = 0; // dùng kênh hiện tại của radio
        peer.encrypt = false;
        peer.ifidx = ESP_IF_WIFI_STA;
        esp_now_mod_peer(&peer);
    }

    esp_now_text_message_t msg = {0};
    msg.type = ESPNOW_MSG_TEXT;
    strncpy(msg.text, text, sizeof(msg.text) - 1);

    s_esp_now_last_send_ok = false;
    xSemaphoreTake(s_esp_now_send_sem, 0);

    esp_err_t err = esp_now_send(s_broadcast_mac, (const uint8_t *)&msg, sizeof(msg));
    if (err != ESP_OK) {
        printf("⚠️ ESP-NOW send error: %d (%s)\n", err, esp_err_to_name(err));
        return false;
    }

    if (xSemaphoreTake(s_esp_now_send_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
        printf("⚠️ ESP-NOW timeout: không nhận được ACK gửi đi trong 200ms\n");
        return false;
    }

    return s_esp_now_last_send_ok;
}

/* ===================== BUTTON TASK & ISR ===================== */
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_button_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_button_task_handle, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void esp_now_button_task(void *arg) {
    TickType_t last_press = 0;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        TickType_t now = xTaskGetTickCount();
        if ((now - last_press) < pdMS_TO_TICKS(300)) continue;
        last_press = now;
        xTaskNotifyStateClear(NULL);

        esp_now_rx_mode = !esp_now_rx_mode;

        if (esp_now_rx_mode) {
            printf("\n📡 [NÚT BẤM] Bắt đầu QUÉT TẤT CẢ KÊNH chờ nhận Config từ Arduino...\n");
            set_esp_now_active(true);
            if (mqtt_client) {
                esp_mqtt_client_stop(mqtt_client);
            }
            esp_wifi_disconnect();
            start_channel_scan();
        } else {
            printf("\n🛑 [NÚT BẤM] Huỷ quét! Quay lại KÊNH CHUNG %d (WiFi/MQTT/Text)...\n", g_esp_now_channel);
            stop_channel_scan();
            esp_now_rx_mode = false;
            set_esp_now_active(false);
            resume_normal_mode_after_scan();
        }
    }
}

static void esp_now_config_start(void) {
    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << ESP_NOW_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));

    xTaskCreate(esp_now_button_task, "btn_task", 3072, NULL, configMAX_PRIORITIES - 2, &s_button_task_handle);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(ESP_NOW_BUTTON_GPIO, gpio_isr_handler, NULL);
}

/* ===================== WIFI & MQTT ===================== */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (config_loaded && !esp_now_rx_mode) {
            esp_wifi_connect();
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        mqtt_connected = false;
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        if (!config_loaded || esp_now_rx_mode) return;

        if (wifi_retry_count < WIFI_MAX_RETRY) {
            wifi_retry_count++;
            esp_wifi_connect();
        } else {
            // Bỏ dò router -> ép radio về kênh offline cố định để ESP-NOW ổn định
            led_off();
            set_esp_now_channel(g_esp_now_channel);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    return ESP_OK;
}

static bool connect_wifi_blocking(uint32_t timeout_ms) {
    s_wifi_connecting = true;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    wifi_config_t wifi_config = {0};

    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    strncpy((char*)wifi_config.sta.ssid, g_wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, g_wifi_pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
    xSemaphoreGive(g_config_mutex);

    wifi_retry_count = 0;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
    // (Đã bỏ lệnh ép kênh ngay sau esp_wifi_connect() ở đây - nó can thiệp
    //  vào lúc STA đang bắt tay với AP và không giúp ích gì.)

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, WIFI_CONNECTED_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)
    );
    bool ok = (bits & WIFI_CONNECTED_BIT) != 0;
    s_wifi_connecting = false;

    if (!ok) {
        // Không có WiFi: dừng dò kênh và khoá radio về kênh offline cố định
        wifi_retry_count = WIFI_MAX_RETRY;   // chặn handler tự nối lại
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));      // chờ event DISCONNECTED xử lý xong
        set_esp_now_channel(g_esp_now_channel);
    }
    return ok;
}

#if WIFI_RETRY_INTERVAL_MS > 0
// Khi offline: thỉnh thoảng thử nối lại router (mỗi lần tối đa ~4s lệch kênh)
static void wifi_retry_task(void *arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_INTERVAL_MS));
        if (!config_loaded || esp_now_rx_mode || reconnecting || s_channel_hop_task_handle != NULL) continue;
        if (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) continue;

        printf("🔁 Offline: thử nối lại router...\n");
        reconnecting = true;
        if (connect_wifi_blocking(4000)) {
            mqtt_init();
        }
        reconnecting = false;
    }
}
#endif

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    if (event->event_id == MQTT_EVENT_CONNECTED) {
        printf("MQTT: Kết nối thành công.\n");
        mqtt_connected = true;
    } else if (event->event_id == MQTT_EVENT_DISCONNECTED) {
        printf("MQTT: Mất kết nối.\n");
        mqtt_connected = false;
    }
}

static void mqtt_init(void) {
    char uri_buffer[64];
    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    snprintf(uri_buffer, sizeof(uri_buffer), "mqtt://%s", g_mqtt_host);
    xSemaphoreGive(g_config_mutex);

    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = uri_buffer,
        .port = MQTT_PORT,
        .network_timeout_ms = 3000,
    };

    if (mqtt_client != NULL) {
        esp_mqtt_client_set_uri(mqtt_client, uri_buffer);
        esp_mqtt_client_start(mqtt_client);
        return;
    }

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

/* ===================== HÀM XUẤT DỮ LIỆU ĐA KÊNH ===================== */
static void publish_text_priority(const char *topic, const char *text) {
    if (!text) return;

    bool sent_via_espnow = esp_now_publish_text(text);

    if (sent_via_espnow) {
        printf("⚡ [TX ESP-NOW KÊNH %d SUCCESS]: %s\n", g_esp_now_channel, text);
    } else {
        if (mqtt_connected) {
            esp_mqtt_client_publish(mqtt_client, topic, text, 0, 1, 0);
            printf("↩️ [FALLBACK MQTT SUCCESS]: %s\n", text);
        } else {
            printf("⚠️ [TX FAILED] ESP-NOW/MQTT không khả dụng: %s\n", text);
        }
    }
}

static void mqtt_publish_speech(const char *text) {
    set_esp_now_active(true);
    publish_text_priority(MQTT_TOPIC, text);
    uart_send_text(text);
}

static void mqtt_publish_audio(const char *text) {
    publish_text_priority(MQTT_TOPIC_AUDIO, text);
}

/* ===================== SPEECH TASKS ===================== */
void feed_Task(void *arg) {
    esp_afe_sr_data_t *afe_data = arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_channel = esp_get_feed_channel();
    int16_t *i2s_buff = malloc(audio_chunksize * sizeof(int16_t) * feed_channel);
    assert(i2s_buff);

    while (task_flag) {
        esp_get_feed_data(false, i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);
        afe_handle->feed(afe_data, i2s_buff);
    }

    free(i2s_buff);
    vTaskDelete(NULL);
}

void detect_Task(void *arg) {
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 6000);
    int mu_chunksize = multinet->get_samp_chunksize(model_data);
    esp_mn_commands_update_from_sdkconfig(multinet, model_data);
    assert(mu_chunksize == afe_chunksize);

    TickType_t question_start_tick = 0;

    printf("------------ Say alexa to start ------------\n");
    stop_channel_scan();
    esp_now_rx_mode = false;
    set_esp_now_active(false);

    while (task_flag) {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) break;

        if (esp_now_rx_mode) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            set_esp_now_active(true);
            led_set_color(80, 80, 80);
            mqtt_publish_speech("alexa");
            uart_send_text("alexa \n");
            printf("⚡ WAKEWORD alexa detected!\n");
            multinet->clean(model_data);
            detect_flag = 1;
            is_question_mode = false;
        }

        if (detect_flag == 1) {
            esp_mn_state_t mn_state = multinet->detect(model_data, res->data);

            if (mn_state == ESP_MN_STATE_DETECTING) {
                continue;
            }

            if (mn_state == ESP_MN_STATE_TIMEOUT) {
                multinet->clean(model_data);

                if (is_question_mode) {
                    if ((xTaskGetTickCount() - question_start_tick) >= pdMS_TO_TICKS(300000)) {
                        printf("⏰ Hết 5 phút chế độ hỏi đáp -> Dừng lắng nghe.\n");
                        led_off();
                        mqtt_publish_speech("bạn chưa nói end question");
                        detect_flag = 0;
                        is_question_mode = false;
                    }
                } else {
                    printf("⏰ MultiNet Timeout.\n");
                    led_off();
                    mqtt_publish_speech("gọi lại alexa để tiếp tục");
                    detect_flag = 0;
                }
                continue;
            }

            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *mn_result = multinet->get_results(model_data);
                if (mn_result->num > 0) {
                    int id = mn_result->command_id[0];

                    switch (id) {
                        case 0:
                            led_off();
                            uart_send_text("haVcKWfSpcN");
                            led_set_color(255, 0, 0);
                            detect_flag = 1;
                            is_question_mode = true;
                            question_start_tick = xTaskGetTickCount();
                            multinet->clean(model_data);
                            set_esp_now_active(false);
                            break;

                        case 1:
                            led_off();
                            led_set_color(0, 0, 255);
                            uart_send_text("fNDKWfSpcN");
                            printf("⚡ Command 1 detected: fND KWfSpcN\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            led_off();
                            break;

                        case 2:
                            led_off();
                            mqtt_publish_speech("TkN eF jc LiTS");
                            printf("⚡ Command 2 detected: TkN eF jc LiTS\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 3:
                            led_off();
                            mqtt_publish_speech("GRmN LiT");
                            printf("⚡ Command 3 detected: GRmN LiT\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 4:
                            led_off();
                            mqtt_publish_speech("RfD LiT");
                            printf("⚡ Command 4 detected: RfD LiT\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 5:
                            led_off();
                            mqtt_publish_speech("BLo LiT");
                            printf("⚡ Command 5 detected: BLo LiT\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 6:
                            led_off();
                            mqtt_publish_speech("WiT LiT");
                            printf("⚡ Command 6 detected: WiT LiT\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 7:
                            led_off();
                            mqtt_publish_speech("TkN nN jc LiTS");
                            printf("⚡ Command 7 detected: TkN nN jc LiTS\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 8:
                            led_off();
                            cJSON *root = cJSON_CreateObject();
                            if (root != NULL) {
                                cJSON_AddStringToObject(root, "ssid", g_wifi_ssid);
                                cJSON_AddStringToObject(root, "pass", g_wifi_pass);
                                cJSON_AddStringToObject(root, "mqtt", g_mqtt_host);
                                cJSON_AddNumberToObject(root, "channel", g_esp_now_channel);

                                char *json_string = cJSON_PrintUnformatted(root);
                                if (json_string != NULL) {
                                    uart_send_text(json_string);
                                    printf("UART WiFm; JSON: %s\n", json_string);
                                    free(json_string);
                                }
                                cJSON_Delete(root);
                            }
                            break;
                        case 9:
                            led_off();
                            mqtt_publish_speech("fan_off");
                            printf("⚡ Command 9 detected: FaN eF\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;
                        case 10:
                            led_off();
                            mqtt_publish_speech("fan_on_25");
                            printf("⚡ Command 10 detected: FaN nN WcN\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;
                        case 11:
                            led_off();
                            mqtt_publish_speech("fan_on_50");
                            printf("⚡ Command 11 detected: FaN nN To\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;
                        case 12:
                            led_off();
                            mqtt_publish_speech("fan_on_75");
                            printf("⚡ Command 12 detected: FaN nN vRm\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;
                        case 13:
                            led_off();
                            mqtt_publish_speech("fan_on_100");
                            printf("⚡ Command 13 detected: FaN nN FeR\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;
                        case 14:
                            led_off();
                            mqtt_publish_speech("fan_on");
                            printf("⚡ Command 14 turn on the fan detected: TkN nN jc FaN\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;
                        case 15:
                            led_off();
                            mqtt_publish_speech("fan_off");
                            printf("⚡ Command 15 turn off the fan detected: TkN eF jc FaN\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;
                        case 16:
                            led_off();
                            mqtt_publish_speech("YfLb LiT");
                            printf("⚡ Command 16 detected: đèn vàng\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 17:
                            led_off();
                            mqtt_publish_speech("eRcNq LiT");
                            printf("⚡ Command 17 detected: eRcNq LiT\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 18:
                            led_off();
                            mqtt_publish_speech("PglK LiT");
                            printf("⚡ Command 18 detected: PglK LiT\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 19:
                            led_off();
                            mqtt_publish_speech("PkPcL LiT");
                            printf("⚡ Command 19 detected: PkPcL LiT\n");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        default:
                            detect_flag = 0;
                            is_question_mode = false;
                            led_off();
                            break;
                    }
                }
            }
        }
    }

    if (model_data) multinet->destroy(model_data);
    vTaskDelete(NULL);
}

/* ===================== APP MAIN ===================== */
void app_main(void) {
    g_config_mutex = xSemaphoreCreateMutex();
    s_esp_now_send_sem = xSemaphoreCreateBinary();
    g_config_queue = xQueueCreate(2, sizeof(config_queue_item_t));
    g_text_queue = xQueueCreate(8, sizeof(text_queue_item_t));

    uart_hardware_init();
    xTaskCreate(uart_rx_task, "uart_rx_task", 3072, NULL, 4, NULL);
    xTaskCreate(text_rx_task, "text_rx", 3072, NULL, 4, NULL);
    printf("✅ Initialized UART Wire (TX: GPIO%d, RX: GPIO%d)\n", UART_TX_PIN, UART_RX_PIN);

    xTaskCreate(net_reconnect_worker_task, "net_worker", 4096, NULL, 3, NULL);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    config_loaded = load_config_from_nvs();
    led_init();

    if (!config_loaded) {
        led_set_color(255, 0, 255); // TÍM: báo thiếu Config, chờ bấm nút để quét
    }

    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(esp_now_init_config());
    set_esp_now_active(false);

    esp_now_config_start();
#if WIFI_RETRY_INTERVAL_MS > 0
    xTaskCreate(wifi_retry_task, "wifi_retry", 3072, NULL, 2, NULL);
#endif

    if (config_loaded && !esp_now_rx_mode) {
        if (connect_wifi_blocking(10000)) {
            mqtt_init();
        }
    }

    models = esp_srmodel_init("model");
    ESP_ERROR_CHECK(esp_board_init(AUDIO_HAL_16K_SAMPLES, 1, 16));

    afe_handle = (esp_afe_sr_iface_t *)&ESP_AFE_SR_HANDLE;

    afe_config_t afe_config = AFE_CONFIG_DEFAULT();
    afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    afe_config.aec_init = false;
    afe_config.pcm_config.total_ch_num = 2;
    afe_config.pcm_config.mic_num = 1;
    afe_config.pcm_config.ref_num = 1;

    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(&afe_config);

    task_flag = 1;

    xTaskCreatePinnedToCore(&feed_Task,   "feed",   8 * 1024, (void *)afe_data, 10, NULL, 1);
    xTaskCreatePinnedToCore(&detect_Task, "detect", 8 * 1024, (void *)afe_data, 5,  NULL, 1);
}
