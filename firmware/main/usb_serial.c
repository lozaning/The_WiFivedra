#include "usb_serial.h"
#include "protocol.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"

/* Scratch buffer for SLIP encoding — only sender task writes, no mutex needed */
static uint8_t s_slip_buf[SLIP_BUF_SIZE];

esp_err_t usb_serial_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 16384,
        .rx_buffer_size = 256,
    };
    return usb_serial_jtag_driver_install(&cfg);
}

void usb_serial_send_slip_frame(const uint8_t *data, size_t len)
{
    size_t idx = 0;

    /* Leading END byte for mid-stream resynchronization (RFC 1055) */
    s_slip_buf[idx++] = SLIP_END;

    for (size_t i = 0; i < len && idx < SLIP_BUF_SIZE - 2; i++) {
        switch (data[i]) {
        case SLIP_END:
            s_slip_buf[idx++] = SLIP_ESC;
            s_slip_buf[idx++] = SLIP_ESC_END;
            break;
        case SLIP_ESC:
            s_slip_buf[idx++] = SLIP_ESC;
            s_slip_buf[idx++] = SLIP_ESC_ESC;
            break;
        default:
            s_slip_buf[idx++] = data[i];
            break;
        }
    }

    /* Trailing END byte */
    if (idx < SLIP_BUF_SIZE) {
        s_slip_buf[idx++] = SLIP_END;
    }

    usb_serial_jtag_write_bytes(s_slip_buf, idx, pdMS_TO_TICKS(100));
}

int usb_serial_read(uint8_t *buf, size_t maxlen)
{
    return usb_serial_jtag_read_bytes(buf, maxlen, 0);
}
