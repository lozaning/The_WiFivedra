#include "usb_serial.h"
#include "sniffer.h"
#include "cmd.h"
#include "esp_err.h"

void app_main(void)
{
    ESP_ERROR_CHECK(usb_serial_init());
    ESP_ERROR_CHECK(sniffer_init(1));
    ESP_ERROR_CHECK(cmd_init());
}
