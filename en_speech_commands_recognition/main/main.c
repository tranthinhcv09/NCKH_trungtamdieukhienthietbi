// ============================================================
//   main.c – ESP32-S3 (ESP-IDF + ESP-SR + ESP-NOW + MQTT)
//   [ESP-NOW 2 KÊNH - ĐỒNG BỘ 100% VỚI ARDUINO SENDER]
// ============================================================
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

#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "led_strip.h"
#include <stdbool.h>

// ===================== CẤU HÌNH KÊNH ESP-NOW =====================
#define ESP_NOW_CONFIG_CHANNEL  1    // Kênh 1: Nhận cấu hình từ Arduino Sender
#define ESP_NOW_TEXT_CHANNEL    10   // Kênh 10: Gửi/Nhận Text & Chạy chính

#define MQTT_PORT               1883
#define MQTT_TOPIC              "esp32/speech"
#define MQTT_TOPIC_AUDIO        "esp32/audio"

#define LED_GPIO                48
#define LED_COUNT               2
#define ESP_NOW_BUTTON_GPIO     GPIO_NUM_0

// ===================== STRUCT ĐỒNG BỘ ARDUINO =====================
// Header gói WiFi Config đã được đổi sang 0xA1 cho khớp với Arduino
#define ESPNOW_MSG_WIFI_CONFIG  0xA1
#define ESPNOW_MSG_TEXT         0x00

typedef struct __attribute__((packed)) {
    uint8_t type;           // 0xA1: Khớp với MSG_TYPE_WIFI bên Arduino
    char ssid[33];
    char password[65];
    char mqtt_server[40];
} wifi_credentials_message_t;

typedef struct __attribute__((packed)) {
    uint8_t type;           // 0x00: Khớp với MSG_TYPE_TEXT bên Arduino
    char text[240];
} esp_now_text_message_t;

static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ===================== BIẾN TOÀN CỤC & MUTEX =====================
static led_strip_t *led_strip = NULL;
static TaskHandle_t s_button_task_handle = NULL;
static QueueHandle_t g_config_queue = NULL;
static SemaphoreHandle_t g_config_mutex = NULL;

static char g_wifi_ssid[33] = {0};
static char g_wifi_pass[65] = {0};
static char g_mqtt_host[40] = {0};
static bool config_loaded = false;

static volatile bool esp_now_rx_mode = false;
static volatile bool reconnecting = false;

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
        printf("NVS chưa có cấu hình. Chuyển sang ESP-NOW Kênh %d nhận Cấu hình.\n", ESP_NOW_CONFIG_CHANNEL);
        return false;
    }

    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    size_t len_ssid = sizeof(g_wifi_ssid);
    size_t len_pass = sizeof(g_wifi_pass);
    size_t len_mqtt = sizeof(g_mqtt_host);

    esp_err_t err = nvs_get_str(handle, "ssid", g_wifi_ssid, &len_ssid);
    err |= nvs_get_str(handle, "pass", g_wifi_pass, &len_pass);
    err |= nvs_get_str(handle, "mqtt", g_mqtt_host, &len_mqtt);
    nvs_close(handle);

    if (err != ESP_OK || g_wifi_ssid[0] == '\0' || g_mqtt_host[0] == '\0') {
        memset(g_wifi_ssid, 0, sizeof(g_wifi_ssid));
        memset(g_wifi_pass, 0, sizeof(g_wifi_pass));
        memset(g_mqtt_host, 0, sizeof(g_mqtt_host));
        xSemaphoreGive(g_config_mutex);
        return false;
    }

    printf("Loaded NVS Config: SSID=%s | MQTT=%s\n", g_wifi_ssid, g_mqtt_host);
    xSemaphoreGive(g_config_mutex);
    return true;
}

static void save_config_to_nvs(const char* ssid, const char* pass, const char* mqtt) {
    nvs_handle_t handle;
    if (nvs_open("net_cfg", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "ssid", ssid);
        nvs_set_str(handle, "pass", pass);
        nvs_set_str(handle, "mqtt", mqtt);
        nvs_commit(handle);
        nvs_close(handle);
        printf("💾 Đã lưu cấu hình mới từ Arduino vào NVS thành công!\n");
    }
}

/* ===================== WORKER TASK & ESP-NOW ===================== */
static void net_reconnect_worker_task(void *arg) {
    wifi_credentials_message_t msg;

    while (true) {
        if (xQueueReceive(g_config_queue, &msg, portMAX_DELAY) == pdTRUE) {
            printf("\n📩 [KÊNH %d] Đã nhận Config từ Sender! SSID: %s | MQTT: %s\n",
                   ESP_NOW_CONFIG_CHANNEL, msg.ssid, msg.mqtt_server);

            save_config_to_nvs(msg.ssid, msg.password, msg.mqtt_server);

            xSemaphoreTake(g_config_mutex, portMAX_DELAY);
            snprintf(g_wifi_ssid, sizeof(g_wifi_ssid), "%s", msg.ssid);
            snprintf(g_wifi_pass, sizeof(g_wifi_pass), "%s", msg.password);
            snprintf(g_mqtt_host, sizeof(g_mqtt_host), "%s", msg.mqtt_server);
            config_loaded = true;
            xSemaphoreGive(g_config_mutex);

            esp_now_rx_mode = false;
            set_esp_now_channel(ESP_NOW_TEXT_CHANNEL);

            led_set_color(128, 0, 128); // LED TÍM: Đang nối WiFi/MQTT

            if (connect_wifi_blocking(15000)) {
                mqtt_init();
                led_off();
            } else {
                printf("❌ WiFi nối thất bại sau khi nhận Config. Vẫn chạy ESP-NOW Kênh %d.\n", ESP_NOW_TEXT_CHANNEL);
                led_off();
            }
            reconnecting = false;
        }
    }
}

static void set_esp_now_channel(uint8_t channel) {
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_is_peer_exist(s_broadcast_mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, s_broadcast_mac, 6);
        peer.channel = channel;
        peer.encrypt = false;
        peer.ifidx = ESP_IF_WIFI_STA;
        esp_now_mod_peer(&peer);
    }
    printf("🔄 ESP-NOW đã chuyển sang KÊNH: %d\n", channel);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void esp_now_send_cb(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
#else
static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
#endif
{
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

    // SỬA: Bắt gói cấu hình với Header 0xA1 khớp với Arduino Sender
    if (type == ESPNOW_MSG_WIFI_CONFIG && len == (int)sizeof(wifi_credentials_message_t)) {
        wifi_credentials_message_t msg;
        memcpy(&msg, data, sizeof(msg));
        msg.ssid[sizeof(msg.ssid) - 1] = '\0';
        msg.password[sizeof(msg.password) - 1] = '\0';
        msg.mqtt_server[sizeof(msg.mqtt_server) - 1] = '\0';

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(g_config_queue, &msg, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return;
    }

    // SỬA: Bắt gói Text với Header 0x00 khớp với Arduino Sender
    if (type == ESPNOW_MSG_TEXT && len == (int)sizeof(esp_now_text_message_t)) {
        const esp_now_text_message_t *tmsg = (const esp_now_text_message_t *)data;
        printf("📥 [ESP-NOW RX TEXT - KÊNH %d] %.*s\n", ESP_NOW_TEXT_CHANNEL, (int)sizeof(tmsg->text), tmsg->text);
    }
}

static esp_err_t esp_now_init_config(void) {
    esp_err_t err = esp_now_init();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) return err;

    err = esp_now_register_recv_cb(esp_now_recv_cb);
    if (err != ESP_OK) return err;

    err = esp_now_register_send_cb(esp_now_send_cb);
    if (err != ESP_OK) return err;

    uint8_t init_channel = esp_now_rx_mode ? ESP_NOW_CONFIG_CHANNEL : ESP_NOW_TEXT_CHANNEL;

    if (!esp_now_is_peer_exist(s_broadcast_mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, s_broadcast_mac, 6);
        peer.channel = init_channel;
        peer.encrypt = false;
        peer.ifidx = ESP_IF_WIFI_STA;
        err = esp_now_add_peer(&peer);
        if (err != ESP_OK) return err;
    }

    set_esp_now_channel(init_channel);
    return ESP_OK;
}

static bool esp_now_publish_text(const char *text) {
    if (!text || esp_now_rx_mode) return false;

    // Lấy kênh RF Home Channel hiện tại của phàn cứng Wi-Fi
    uint8_t current_chan = 0;
    wifi_second_chan_t second_chan;
    esp_wifi_get_channel(&current_chan, &second_chan);

    // Cập nhật lại Peer Channel thành 0 (hoặc bằng kênh hiện tại) để tránh lỗi lệch kênh
    if (esp_now_is_peer_exist(s_broadcast_mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, s_broadcast_mac, 6);
        peer.channel = 0; // 0 = Đi theo Kênh RF Home Channel hiện tại
        peer.encrypt = false;
        peer.ifidx = ESP_IF_WIFI_STA;
        esp_now_mod_peer(&peer);
    }

    esp_now_text_message_t msg = {0};
    msg.type = ESPNOW_MSG_TEXT;
    strncpy(msg.text, text, sizeof(msg.text) - 1);

    xSemaphoreTake(s_esp_now_send_sem, 0);

    esp_err_t err = esp_now_send(s_broadcast_mac, (const uint8_t *)&msg, sizeof(msg));
    if (err != ESP_OK) return false;

    if (xSemaphoreTake(s_esp_now_send_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
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
            printf("\n📡 [NÚT BẤM] Chuyển sang KÊNH %d chờ nhận Config từ Arduino...\n", ESP_NOW_CONFIG_CHANNEL);
            if (mqtt_client) {
                esp_mqtt_client_stop(mqtt_client);
            }
            esp_wifi_disconnect();
            set_esp_now_channel(ESP_NOW_CONFIG_CHANNEL);
            led_set_color(255, 255, 0); // VÀNG: Báo Config Mode
        } else {
            printf("\n🛑 [NÚT BẤM] TẮT Config Mode! Chuyển KÊNH %d (WiFi/MQTT/Text)...\n", ESP_NOW_TEXT_CHANNEL);
            led_off();
            set_esp_now_channel(ESP_NOW_TEXT_CHANNEL);

            if (config_loaded && !reconnecting) {
                reconnecting = true;
                if (connect_wifi_blocking(15000)) {
                    mqtt_init();
                }
                reconnecting = false;
            }
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

        if (wifi_retry_count < 5) {
            wifi_retry_count++;
            esp_wifi_connect();
        } else {
            led_off();
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

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, WIFI_CONNECTED_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)
    );
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

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

static void publish_text_priority(const char *topic, const char *text) {
    if (!text) return;

    bool sent_via_espnow = esp_now_publish_text(text);

    if (sent_via_espnow) {
        printf("⚡ [TX ESP-NOW KÊNH %d SUCCESS]: %s\n", ESP_NOW_TEXT_CHANNEL, text);
    } else {
        if (mqtt_connected) {
            esp_mqtt_client_publish(mqtt_client, topic, text, 0, 1, 0);
            printf("↩️ [FALLBACK MQTT SUCCESS]: %s\n", text);
        } else {
            printf("⚠️ [TX FAIL] ESP-NOW và MQTT đều thất bại: %s\n", text);
        }
    }
}

static void mqtt_publish_speech(const char *text) {
    publish_text_priority(MQTT_TOPIC, text);
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

    bool is_question_mode = false;
    TickType_t question_start_tick = 0;

    printf("------------ Say alexa to start ------------\n");

    while (task_flag) {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) break;

        if (esp_now_rx_mode) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            led_set_color(80, 80, 80);
            mqtt_publish_speech("alexa");
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
                            //mqtt_publish_audio("haVcKWfSpcN");
                            mqtt_publish_speech("haVcKWfSpcN");
                            led_set_color(255, 0, 0);
                            detect_flag = 1;
                            is_question_mode = true;
                            question_start_tick = xTaskGetTickCount();
                            multinet->clean(model_data);
                            break;

                        case 1:
                            led_off();
                            led_set_color(0, 0, 255);
                            //mqtt_publish_audio("fND KWfSpcN");
                            mqtt_publish_speech("fND KWfSpcN");
                            detect_flag = 0;
                            is_question_mode = false;
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            led_off();
                            break;

                        case 2:
                            led_off();
                            mqtt_publish_speech("TkN eF jc LiTS");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 3:
                            led_off();
                            mqtt_publish_speech("GRmN LiT");
                            led_set_color(0, 255, 0);
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 4:
                            led_off();
                            mqtt_publish_speech("RfD LiT");
                            led_set_color(255, 0, 0);
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 5:
                            led_off();
                            mqtt_publish_speech("BLo LiT");
                            led_set_color(0, 0, 255);
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 6:
                            led_off();
                            mqtt_publish_speech("eF LiT");
                            detect_flag = 0;
                            is_question_mode = false;
                            break;

                        case 7:
                            led_off();
                            mqtt_publish_speech("TkN nN jc LiTS");
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
    g_config_queue = xQueueCreate(2, sizeof(wifi_credentials_message_t));

    xTaskCreate(net_reconnect_worker_task, "net_worker", 4096, NULL, 3, NULL);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    config_loaded = load_config_from_nvs();
    led_init();

    if (!config_loaded) {
        esp_now_rx_mode = true;
        led_set_color(255, 255, 0); // VÀNG: Báo thiếu Config (Chạy Kênh 1)
    }

    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(esp_now_init_config());

    esp_now_config_start();

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