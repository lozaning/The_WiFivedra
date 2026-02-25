#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t usb_serial_init(void);
void usb_serial_send_slip_frame(const uint8_t *data, size_t len);
int usb_serial_read(uint8_t *buf, size_t maxlen);
