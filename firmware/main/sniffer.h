#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t sniffer_init(uint8_t initial_channel);
esp_err_t sniffer_set_channel(uint8_t channel);
esp_err_t sniffer_set_filter(uint32_t mask);
void      sniffer_set_snaplen(uint16_t snaplen);
void      sniffer_set_compress(bool enable);

uint8_t   sniffer_get_channel(void);
uint32_t  sniffer_get_filter(void);
uint16_t  sniffer_get_snaplen(void);
bool      sniffer_get_compress(void);
uint32_t  sniffer_get_captured(void);
uint32_t  sniffer_get_dropped(void);
uint32_t  sniffer_get_queue_depth(void);
uint32_t  sniffer_get_free_heap(void);
