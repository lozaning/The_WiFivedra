idf_component_register(
    SRCS "main.c" "sniffer.c" "usb_serial.c" "cmd.c"
    PRIV_REQUIRES esp_wifi nvs_flash esp_driver_usb_serial_jtag esp_system
    INCLUDE_DIRS "."
)
