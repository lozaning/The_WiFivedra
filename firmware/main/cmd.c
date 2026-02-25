#include "cmd.h"
#include "protocol.h"
#include "usb_serial.h"
#include "sniffer.h"

#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define CMD_TASK_STACK    2048
#define CMD_TASK_PRIORITY 3
#define CMD_LINE_MAX      128
#define RESP_BUF_MAX      200

static void send_response(const char *text)
{
    size_t text_len = strlen(text);
    uint8_t buf[1 + RESP_BUF_MAX];

    buf[0] = MSG_TYPE_RESPONSE;
    if (text_len > RESP_BUF_MAX - 1) {
        text_len = RESP_BUF_MAX - 1;
    }
    memcpy(buf + 1, text, text_len);

    usb_serial_send_slip_frame(buf, 1 + text_len);
}

/* Parse space-separated filter tokens into a bitmask */
static uint32_t parse_filter_mask(const char *args)
{
    uint32_t mask = 0;
    char buf[64];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = strtok(buf, " ");
    while (token) {
        if (strcasecmp(token, "ALL") == 0) {
            return WIFI_PROMIS_FILTER_MASK_MGMT |
                   WIFI_PROMIS_FILTER_MASK_DATA |
                   WIFI_PROMIS_FILTER_MASK_CTRL;
        } else if (strcasecmp(token, "MGMT") == 0) {
            mask |= WIFI_PROMIS_FILTER_MASK_MGMT;
        } else if (strcasecmp(token, "DATA") == 0) {
            mask |= WIFI_PROMIS_FILTER_MASK_DATA;
        } else if (strcasecmp(token, "CTRL") == 0) {
            mask |= WIFI_PROMIS_FILTER_MASK_CTRL;
        }
        token = strtok(NULL, " ");
    }
    return mask;
}

static const char *filter_mask_str(uint32_t mask, char *out, size_t out_len)
{
    out[0] = '\0';
    if (mask & WIFI_PROMIS_FILTER_MASK_MGMT) strlcat(out, "MGMT ", out_len);
    if (mask & WIFI_PROMIS_FILTER_MASK_DATA) strlcat(out, "DATA ", out_len);
    if (mask & WIFI_PROMIS_FILTER_MASK_CTRL) strlcat(out, "CTRL ", out_len);
    /* Trim trailing space */
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] == ' ') out[len - 1] = '\0';
    return out;
}

static void handle_command(const char *line)
{
    /* CH <n> — switch channel */
    if (strncasecmp(line, "CH ", 3) == 0) {
        int ch = atoi(line + 3);
        if (ch < 1 || ch > 196) {
            send_response("ERR invalid channel");
            return;
        }
        esp_err_t err = sniffer_set_channel((uint8_t)ch);
        if (err != ESP_OK) {
            send_response("ERR channel switch failed");
            return;
        }
        char resp[32];
        snprintf(resp, sizeof(resp), "OK CH %d", ch);
        send_response(resp);
        return;
    }

    /* FILTER <MGMT|DATA|CTRL|ALL> — set frame type filter */
    if (strncasecmp(line, "FILTER ", 7) == 0) {
        uint32_t mask = parse_filter_mask(line + 7);
        if (mask == 0) {
            send_response("ERR invalid filter (use MGMT DATA CTRL ALL)");
            return;
        }
        esp_err_t err = sniffer_set_filter(mask);
        if (err != ESP_OK) {
            send_response("ERR filter set failed");
            return;
        }
        char fstr[32];
        char resp[64];
        filter_mask_str(mask, fstr, sizeof(fstr));
        snprintf(resp, sizeof(resp), "OK FILTER %s", fstr);
        send_response(resp);
        return;
    }

    /* SNAPLEN <n> — set capture truncation (0 = full frames) */
    if (strncasecmp(line, "SNAPLEN ", 8) == 0) {
        int val = atoi(line + 8);
        if (val < 0 || val > MAX_80211_FRAME_LEN) {
            send_response("ERR snaplen out of range (0-2500)");
            return;
        }
        sniffer_set_snaplen((uint16_t)val);
        char resp[32];
        snprintf(resp, sizeof(resp), "OK SNAPLEN %d", val);
        send_response(resp);
        return;
    }

    /* COMPRESS — not supported (tdefl_compressor needs ~160KB, too large) */
    if (strncasecmp(line, "COMPRESS ", 9) == 0) {
        send_response("ERR compression not available (insufficient RAM)");
        return;
    }

    /* STATUS — return current state */
    if (strncasecmp(line, "STATUS", 6) == 0) {
        char resp[RESP_BUF_MAX];
        char fstr[32];
        uint8_t ch = sniffer_get_channel();
        filter_mask_str(sniffer_get_filter(), fstr, sizeof(fstr));
        snprintf(resp, sizeof(resp),
                 "CH %d BAND %s FILTER %s SNAPLEN %u COMPRESS %s "
                 "QUEUE %lu CAP %lu DROP %lu HEAP %lu",
                 ch,
                 ch >= 36 ? "5G" : "2.4G",
                 fstr,
                 sniffer_get_snaplen(),
                 sniffer_get_compress() ? "ON" : "OFF",
                 (unsigned long)sniffer_get_queue_depth(),
                 (unsigned long)sniffer_get_captured(),
                 (unsigned long)sniffer_get_dropped(),
                 (unsigned long)sniffer_get_free_heap());
        send_response(resp);
        return;
    }

    send_response("ERR unknown command");
}

static void cmd_task(void *arg)
{
    uint8_t read_buf[64];
    char line_buf[CMD_LINE_MAX];
    size_t line_pos = 0;

    while (true) {
        int n = usb_serial_read(read_buf, sizeof(read_buf));
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                uint8_t c = read_buf[i];
                if (c == '\n' || c == '\r') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        handle_command(line_buf);
                        line_pos = 0;
                    }
                } else if (line_pos < CMD_LINE_MAX - 1) {
                    line_buf[line_pos++] = (char)c;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t cmd_init(void)
{
    BaseType_t ret = xTaskCreate(cmd_task, "cmd", CMD_TASK_STACK, NULL, CMD_TASK_PRIORITY, NULL);
    return (ret == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
