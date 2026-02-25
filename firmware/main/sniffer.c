#include "sniffer.h"
#include "protocol.h"
#include "usb_serial.h"

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_mac.h"

#include <string.h>
#include <stdlib.h>

#define SENDER_TASK_STACK     4096
#define SENDER_TASK_PRIORITY  5
#define HEAP_RESERVE          40960   /* 40KB headroom for stacks, buffers, etc. */
#define MIN_QUEUE_DEPTH       32
#define MAX_QUEUE_DEPTH       2048
#define AVG_PAYLOAD_ESTIMATE  256     /* For queue sizing when snaplen=0 */

typedef struct {
    uint8_t  *payload;
    uint16_t  length;
    uint8_t   channel;
    int8_t    rssi;
    uint32_t  timestamp_us;
} queued_packet_t;

static QueueHandle_t s_packet_queue;
static uint8_t   s_current_channel;
static uint32_t  s_captured;
static uint32_t  s_dropped;
static uint32_t  s_queue_depth;
static uint16_t  s_snaplen;          /* 0 = no truncation */

/* ---- Promiscuous callback (runs in WiFi task context) ---- */
static void IRAM_ATTR wifi_sniffer_cb(void *recv_buf, wifi_promiscuous_pkt_type_t type)
{
    if (type == WIFI_PKT_MISC) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)recv_buf;

    if (pkt->rx_ctrl.rx_state != 0) {
        return;
    }

    uint16_t sig_len = pkt->rx_ctrl.sig_len;
    if (sig_len <= IEEE80211_FCS_LEN) {
        return;
    }
    sig_len -= IEEE80211_FCS_LEN;

    if (sig_len > MAX_80211_FRAME_LEN) {
        sig_len = MAX_80211_FRAME_LEN;
    }

    /* Apply snaplen truncation before malloc — saves RAM and bandwidth */
    uint16_t copy_len = sig_len;
    if (s_snaplen > 0 && copy_len > s_snaplen) {
        copy_len = s_snaplen;
    }

    uint8_t *copy = malloc(copy_len);
    if (!copy) {
        s_dropped++;
        return;
    }
    memcpy(copy, pkt->payload, copy_len);

    queued_packet_t item = {
        .payload      = copy,
        .length       = copy_len,
        .channel      = s_current_channel,
        .rssi         = pkt->rx_ctrl.rssi,
        .timestamp_us = pkt->rx_ctrl.timestamp,
    };

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xQueueSendFromISR(s_packet_queue, &item, &xHigherPriorityTaskWoken) != pdTRUE) {
        free(copy);
        s_dropped++;
    } else {
        s_captured++;
    }

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* ---- Sender task: dequeue packets and SLIP-send ---- */
static void sender_task(void *arg)
{
    static uint8_t frame_buf[sizeof(pkt_header_t) + MAX_80211_FRAME_LEN];

    queued_packet_t item;
    while (true) {
        if (xQueueReceive(s_packet_queue, &item, portMAX_DELAY) == pdTRUE) {
            pkt_header_t hdr = {
                .msg_type  = MSG_TYPE_PACKET,
                .channel   = item.channel,
                .rssi      = item.rssi,
                .flags     = 0,
                .sig_len   = item.length,
                .timestamp = item.timestamp_us,
            };

            memcpy(frame_buf, &hdr, sizeof(hdr));
            memcpy(frame_buf + sizeof(hdr), item.payload, item.length);

            usb_serial_send_slip_frame(frame_buf, sizeof(hdr) + item.length);
            free(item.payload);
        }
    }
}

/* ---- Calculate optimal queue depth based on free heap ---- */
static uint32_t calculate_queue_depth(void)
{
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t available = (free_heap > HEAP_RESERVE) ? (free_heap - HEAP_RESERVE) : 0;

    /* Each queued packet costs: queue slot (sizeof struct) + heap-allocated payload.
     * Use snaplen if set, otherwise conservative average estimate. */
    uint32_t payload_est = (s_snaplen > 0) ? s_snaplen : AVG_PAYLOAD_ESTIMATE;
    uint32_t per_packet = sizeof(queued_packet_t) + payload_est;

    uint32_t depth = available / per_packet;
    if (depth < MIN_QUEUE_DEPTH) depth = MIN_QUEUE_DEPTH;
    if (depth > MAX_QUEUE_DEPTH) depth = MAX_QUEUE_DEPTH;
    return depth;
}

/* ---- Public API ---- */

esp_err_t sniffer_init(uint8_t initial_channel)
{
    esp_err_t ret;

    /* NVS — required by WiFi driver even if NVS storage is disabled */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Calculate queue depth based on remaining heap after WiFi init */
    s_queue_depth = calculate_queue_depth();
    s_packet_queue = xQueueCreate(s_queue_depth, sizeof(queued_packet_t));
    if (!s_packet_queue) {
        return ESP_ERR_NO_MEM;
    }

    /* Set up promiscuous mode — default: all frame types */
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA |
                       WIFI_PROMIS_FILTER_MASK_CTRL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    /* Set initial channel */
    s_current_channel = initial_channel;
    ESP_ERROR_CHECK(esp_wifi_set_channel(initial_channel, WIFI_SECOND_CHAN_NONE));

    /* Start sender task */
    xTaskCreate(sender_task, "sniffer_send", SENDER_TASK_STACK, NULL, SENDER_TASK_PRIORITY, NULL);

    return ESP_OK;
}

esp_err_t sniffer_set_channel(uint8_t channel)
{
    bool new_is_5g = (channel >= 36);
    bool old_is_5g = (s_current_channel >= 36);

    if (new_is_5g != old_is_5g) {
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
        if (new_is_5g) {
            ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY));
        } else {
            ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY));
        }
        ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    }

    s_current_channel = channel;
    return ESP_OK;
}

esp_err_t sniffer_set_filter(uint32_t mask)
{
    wifi_promiscuous_filter_t filter = { .filter_mask = mask };
    return esp_wifi_set_promiscuous_filter(&filter);
}

void sniffer_set_snaplen(uint16_t snaplen)
{
    s_snaplen = snaplen;
}

void sniffer_set_compress(bool enable)
{
    /* Compression disabled — tdefl_compressor is ~160KB, too large for ESP32-C5 */
    (void)enable;
}

uint8_t sniffer_get_channel(void)
{
    return s_current_channel;
}

uint32_t sniffer_get_filter(void)
{
    wifi_promiscuous_filter_t filter;
    esp_wifi_get_promiscuous_filter(&filter);
    return filter.filter_mask;
}

uint16_t sniffer_get_snaplen(void)
{
    return s_snaplen;
}

bool sniffer_get_compress(void)
{
    return false;
}

uint32_t sniffer_get_captured(void)
{
    return s_captured;
}

uint32_t sniffer_get_dropped(void)
{
    return s_dropped;
}

uint32_t sniffer_get_queue_depth(void)
{
    return s_queue_depth;
}

uint32_t sniffer_get_free_heap(void)
{
    return esp_get_free_heap_size();
}
