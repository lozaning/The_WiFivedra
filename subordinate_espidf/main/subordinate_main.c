/**
 * WiFivedra Subordinate Firmware - ESP-IDF Version
 *
 * Performs WiFi scanning on assigned channels and reports results to controller.
 *
 * Hardware: ESP32-C5
 * Communication: Daisy chain - Direct UART connection to left and right neighbors
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "protocol_defs.h"

static const char *TAG = "WiFivedra-Sub";

// Pin Configuration
#define LED_PIN GPIO_NUM_2

// Upstream UART (toward controller)
#define UPSTREAM_UART_NUM       UART_NUM_1
#define UPSTREAM_TX_PIN         GPIO_NUM_21
#define UPSTREAM_RX_PIN         GPIO_NUM_20
#define UPSTREAM_BUF_SIZE       2048

// Downstream UART (away from controller)
#define DOWNSTREAM_UART_NUM     UART_NUM_2
#define DOWNSTREAM_TX_PIN       GPIO_NUM_17
#define DOWNSTREAM_RX_PIN       GPIO_NUM_16
#define DOWNSTREAM_BUF_SIZE     2048

// Seen networks tracking
#define MAX_SEEN_NETWORKS 500
typedef struct {
    uint8_t bssid[6];
    uint32_t lastSeen;
    uint16_t seenCount;
} SeenNetwork;

// New networks buffer
#define MAX_NEW_NETWORKS 100

// Global state
static uint8_t myAddress = UNASSIGNED_ADDRESS;
static bool isLastNode = false;
static bool addressAssigned = false;

static ScanParams scanParams = {
    .band = BAND_5GHZ,
    .channel = 36,
    .scanMode = SCAN_MODE_ACTIVE,
    .scanTimeMs = 120,
    .intervalMs = 1000,
    .hidden = 1,
    .showHidden = 1
};

static StatusInfo status = {
    .state = STATE_IDLE,
    .channel = 0,
    .band = BAND_5GHZ,
    .scanCount = 0,
    .resultCount = 0,
    .uptime = 0,
    .lastError = PROTO_ERR_NONE,
    .freeHeap = 100
};

static bool scanningActive = false;
static uint32_t lastScanTime = 0;

static SeenNetwork seenNetworks[MAX_SEEN_NETWORKS];
static uint16_t seenNetworksCount = 0;

static WiFiScanResult newNetworksBuffer[MAX_NEW_NETWORKS];
static uint16_t newNetworksCount = 0;

static GPSPosition cachedGPS = {0};
static bool hasValidGPS = false;

// Forward declarations
static void uart_init(void);
static void wifi_init(void);
static bool send_packet_upstream(Packet *packet);
static bool send_packet_downstream(Packet *packet);
static bool receive_packet(uart_port_t uart_num, Packet *packet, uint32_t timeout_ms);
static void forward_packet(Packet *packet);
static void handle_command(Packet *packet);
static void handle_address_assignment(Packet *packet);
static void perform_scan(void);
static int16_t find_seen_network(const uint8_t *bssid);
static void move_seen_network_to_top(uint16_t index);
static void add_to_seen_networks(const uint8_t *bssid, uint32_t timestamp);
static bool process_network_result(const WiFiScanResult *result);
static void send_buffered_results(void);
static void uart_task(void *pvParameters);
static void scan_task(void *pvParameters);

// UART initialization
static void uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = SERIAL_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    // Upstream UART
    ESP_ERROR_CHECK(uart_param_config(UPSTREAM_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UPSTREAM_UART_NUM, UPSTREAM_TX_PIN, UPSTREAM_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UPSTREAM_UART_NUM, UPSTREAM_BUF_SIZE * 2, 0, 0, NULL, 0));

    // Downstream UART
    ESP_ERROR_CHECK(uart_param_config(DOWNSTREAM_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(DOWNSTREAM_UART_NUM, DOWNSTREAM_TX_PIN, DOWNSTREAM_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(DOWNSTREAM_UART_NUM, DOWNSTREAM_BUF_SIZE * 2, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "UART initialized");
}

// WiFi initialization
static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialized");
}

// Send packet upstream
static bool send_packet_upstream(Packet *packet) {
    uint8_t buffer[sizeof(Packet)];
    memcpy(buffer, packet, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), packet->payload, packet->header.length);
    buffer[sizeof(PacketHeader) + packet->header.length] = packet->endMarker;

    size_t totalSize = sizeof(PacketHeader) + packet->header.length + 1;
    int written = uart_write_bytes(UPSTREAM_UART_NUM, buffer, totalSize);

    return (written == totalSize);
}

// Send packet downstream
static bool send_packet_downstream(Packet *packet) {
    if (isLastNode) {
        return false;
    }

    uint8_t buffer[sizeof(Packet)];
    memcpy(buffer, packet, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), packet->payload, packet->header.length);
    buffer[sizeof(PacketHeader) + packet->header.length] = packet->endMarker;

    size_t totalSize = sizeof(PacketHeader) + packet->header.length + 1;
    int written = uart_write_bytes(DOWNSTREAM_UART_NUM, buffer, totalSize);

    return (written == totalSize);
}

// Receive packet
static bool receive_packet(uart_port_t uart_num, Packet *packet, uint32_t timeout_ms) {
    uint8_t buffer[MAX_PACKET_SIZE];
    int len = uart_read_bytes(uart_num, buffer, sizeof(PacketHeader), pdMS_TO_TICKS(timeout_ms));

    if (len != sizeof(PacketHeader)) {
        return false;
    }

    memcpy(&packet->header, buffer, sizeof(PacketHeader));

    if (packet->header.startMarker != PACKET_START_MARKER) {
        return false;
    }

    if (packet->header.length > 0) {
        len = uart_read_bytes(uart_num, packet->payload, packet->header.length, pdMS_TO_TICKS(100));
        if (len != packet->header.length) {
            return false;
        }
    }

    len = uart_read_bytes(uart_num, &packet->endMarker, 1, pdMS_TO_TICKS(10));
    if (len != 1 || packet->endMarker != PACKET_END_MARKER) {
        return false;
    }

    return true;
}

// Forward packet
static void forward_packet(Packet *packet) {
    if (packet->header.destAddr < myAddress) {
        send_packet_upstream(packet);
    } else if (packet->header.destAddr > myAddress) {
        send_packet_downstream(packet);
    }
}

// Handle address assignment
static void handle_address_assignment(Packet *packet) {
    if (packet->header.length != sizeof(AddressAssignment)) {
        return;
    }

    AddressAssignment *assignment = (AddressAssignment*)packet->payload;

    myAddress = assignment->assignedAddress;
    addressAssigned = true;

    ESP_LOGI(TAG, "Address assigned: %d", myAddress);

    // Try to assign downstream
    Packet downstreamPacket = {0};
    downstreamPacket.header.startMarker = PACKET_START_MARKER;
    downstreamPacket.header.version = PROTOCOL_VERSION;
    downstreamPacket.header.destAddr = UNASSIGNED_ADDRESS;
    downstreamPacket.header.srcAddr = myAddress;
    downstreamPacket.header.type = CMD_ASSIGN_ADDRESS;
    downstreamPacket.header.length = sizeof(AddressAssignment);
    downstreamPacket.endMarker = PACKET_END_MARKER;

    AddressAssignment nextAssignment = {
        .assignedAddress = myAddress + 1,
        .isLastNode = 0
    };
    memcpy(downstreamPacket.payload, &nextAssignment, sizeof(AddressAssignment));

    send_packet_downstream(&downstreamPacket);

    // Wait for response
    vTaskDelay(pdMS_TO_TICKS(ADDRESS_ASSIGNMENT_TIMEOUT_MS));

    Packet responsePacket;
    if (!receive_packet(DOWNSTREAM_UART_NUM, &responsePacket, ADDRESS_ASSIGNMENT_TIMEOUT_MS)) {
        isLastNode = true;
        ESP_LOGI(TAG, "I am the last node");
    }

    // Confirm assignment to controller
    Packet confirmPacket = {0};
    confirmPacket.header.startMarker = PACKET_START_MARKER;
    confirmPacket.header.version = PROTOCOL_VERSION;
    confirmPacket.header.destAddr = CONTROLLER_ADDRESS;
    confirmPacket.header.srcAddr = myAddress;
    confirmPacket.header.type = RESP_ADDRESS_ASSIGNED;
    confirmPacket.header.length = sizeof(AddressAssignment);
    confirmPacket.endMarker = PACKET_END_MARKER;

    AddressAssignment confirmation = {
        .assignedAddress = myAddress,
        .isLastNode = isLastNode
    };
    memcpy(confirmPacket.payload, &confirmation, sizeof(AddressAssignment));

    send_packet_upstream(&confirmPacket);
}

// Handle command
static void handle_command(Packet *packet) {
    switch (packet->header.type) {
        case CMD_SET_SCAN_PARAMS:
            if (packet->header.length == sizeof(ScanParams)) {
                memcpy(&scanParams, packet->payload, sizeof(ScanParams));
                status.channel = scanParams.channel;
                status.band = scanParams.band;

                Packet ackPacket = {0};
                ackPacket.header.startMarker = PACKET_START_MARKER;
                ackPacket.header.version = PROTOCOL_VERSION;
                ackPacket.header.destAddr = CONTROLLER_ADDRESS;
                ackPacket.header.srcAddr = myAddress;
                ackPacket.header.type = RESP_ACK;
                ackPacket.header.length = 0;
                ackPacket.endMarker = PACKET_END_MARKER;
                send_packet_upstream(&ackPacket);
            }
            break;

        case CMD_START_SCAN:
            if (!scanningActive) {
                scanningActive = true;
                status.state = STATE_SCANNING;
                lastScanTime = 0;

                Packet ackPacket = {0};
                ackPacket.header.startMarker = PACKET_START_MARKER;
                ackPacket.header.version = PROTOCOL_VERSION;
                ackPacket.header.destAddr = CONTROLLER_ADDRESS;
                ackPacket.header.srcAddr = myAddress;
                ackPacket.header.type = RESP_ACK;
                ackPacket.header.length = 0;
                ackPacket.endMarker = PACKET_END_MARKER;
                send_packet_upstream(&ackPacket);
            }
            break;

        case CMD_STOP_SCAN:
            scanningActive = false;
            status.state = STATE_IDLE;

            Packet ackPacket = {0};
            ackPacket.header.startMarker = PACKET_START_MARKER;
            ackPacket.header.version = PROTOCOL_VERSION;
            ackPacket.header.destAddr = CONTROLLER_ADDRESS;
            ackPacket.header.srcAddr = myAddress;
            ackPacket.header.type = RESP_ACK;
            ackPacket.header.length = 0;
            ackPacket.endMarker = PACKET_END_MARKER;
            send_packet_upstream(&ackPacket);
            break;

        case CMD_GET_SCAN_RESULTS:
            send_buffered_results();
            break;

        case CMD_CLEAR_RESULTS:
            newNetworksCount = 0;
            status.resultCount = 0;
            break;

        case CMD_GPS_UPDATE:
            if (packet->header.length == sizeof(GPSPosition)) {
                memcpy(&cachedGPS, packet->payload, sizeof(GPSPosition));
                hasValidGPS = (cachedGPS.fixQuality > 0);
            }
            break;

        default:
            break;
    }
}

// Find seen network
static int16_t find_seen_network(const uint8_t *bssid) {
    for (uint16_t i = 0; i < seenNetworksCount; i++) {
        if (memcmp(seenNetworks[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}

// Move seen network to top (LRU)
static void move_seen_network_to_top(uint16_t index) {
    if (index == 0) return;

    SeenNetwork temp = seenNetworks[index];
    memmove(&seenNetworks[1], &seenNetworks[0], index * sizeof(SeenNetwork));
    seenNetworks[0] = temp;
}

// Add to seen networks
static void add_to_seen_networks(const uint8_t *bssid, uint32_t timestamp) {
    if (seenNetworksCount < MAX_SEEN_NETWORKS) {
        memmove(&seenNetworks[1], &seenNetworks[0], seenNetworksCount * sizeof(SeenNetwork));
        seenNetworksCount++;
    } else {
        memmove(&seenNetworks[1], &seenNetworks[0], (MAX_SEEN_NETWORKS - 1) * sizeof(SeenNetwork));
    }

    memcpy(seenNetworks[0].bssid, bssid, 6);
    seenNetworks[0].lastSeen = timestamp;
    seenNetworks[0].seenCount = 1;
}

// Process network result
static bool process_network_result(const WiFiScanResult *result) {
    int16_t index = find_seen_network(result->bssid);

    if (index >= 0) {
        seenNetworks[index].lastSeen = result->timestamp;
        seenNetworks[index].seenCount++;
        move_seen_network_to_top(index);
        return false; // Already seen
    } else {
        add_to_seen_networks(result->bssid, result->timestamp);
        return true; // New network
    }
}

// Perform WiFi scan
static void perform_scan(void) {
    status.state = STATE_SCANNING;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = scanParams.channel,
        .show_hidden = scanParams.showHidden,
        .scan_type = (scanParams.scanMode == SCAN_MODE_ACTIVE) ? WIFI_SCAN_TYPE_ACTIVE : WIFI_SCAN_TYPE_PASSIVE,
        .scan_time.active.min = scanParams.scanTimeMs,
        .scan_time.active.max = scanParams.scanTimeMs,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);

    if (ret != ESP_OK) {
        status.lastError = PROTO_ERR_SCAN_FAILED;
        return;
    }

    status.scanCount++;

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count > 0) {
        wifi_ap_record_t *ap_records = malloc(ap_count * sizeof(wifi_ap_record_t));
        if (ap_records) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_records);

            for (uint16_t i = 0; i < ap_count && newNetworksCount < MAX_NEW_NETWORKS; i++) {
                WiFiScanResult result = {0};

                memcpy(result.bssid, ap_records[i].bssid, 6);
                strncpy(result.ssid, (char*)ap_records[i].ssid, 32);
                result.rssi = ap_records[i].rssi;
                result.channel = ap_records[i].primary;
                result.band = (ap_records[i].second == WIFI_SECOND_CHAN_NONE) ? BAND_2_4GHZ : BAND_5GHZ;
                result.authMode = ap_records[i].authmode;
                result.timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Add GPS coordinates
                result.latitude = cachedGPS.latitude;
                result.longitude = cachedGPS.longitude;
                result.altitude = cachedGPS.altitude;
                result.gpsQuality = cachedGPS.fixQuality;

                if (process_network_result(&result)) {
                    newNetworksBuffer[newNetworksCount++] = result;
                    status.resultCount = newNetworksCount;
                }
            }

            free(ap_records);
        }
    }
}

// Send buffered results
static void send_buffered_results(void) {
    for (uint16_t i = 0; i < newNetworksCount; i++) {
        Packet packet = {0};
        packet.header.startMarker = PACKET_START_MARKER;
        packet.header.version = PROTOCOL_VERSION;
        packet.header.destAddr = CONTROLLER_ADDRESS;
        packet.header.srcAddr = myAddress;
        packet.header.type = RESP_SCAN_RESULT;
        packet.header.length = sizeof(WiFiScanResult);
        packet.endMarker = PACKET_END_MARKER;

        memcpy(packet.payload, &newNetworksBuffer[i], sizeof(WiFiScanResult));
        send_packet_upstream(&packet);

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Send final ACK
    Packet ackPacket = {0};
    ackPacket.header.startMarker = PACKET_START_MARKER;
    ackPacket.header.version = PROTOCOL_VERSION;
    ackPacket.header.destAddr = CONTROLLER_ADDRESS;
    ackPacket.header.srcAddr = myAddress;
    ackPacket.header.type = RESP_ACK;
    ackPacket.header.length = 0;
    ackPacket.endMarker = PACKET_END_MARKER;
    send_packet_upstream(&ackPacket);
}

// UART task - handles packet routing
static void uart_task(void *pvParameters) {
    while (1) {
        // Check upstream for packets
        Packet upstreamPacket;
        if (receive_packet(UPSTREAM_UART_NUM, &upstreamPacket, 10)) {
            if (upstreamPacket.header.destAddr == UNASSIGNED_ADDRESS && !addressAssigned) {
                handle_address_assignment(&upstreamPacket);
            } else if (upstreamPacket.header.destAddr == myAddress || upstreamPacket.header.destAddr == 0xFF) {
                handle_command(&upstreamPacket);
            } else {
                forward_packet(&upstreamPacket);
            }
        }

        // Check downstream for packets (responses from further subordinates)
        Packet downstreamPacket;
        if (receive_packet(DOWNSTREAM_UART_NUM, &downstreamPacket, 10)) {
            forward_packet(&downstreamPacket);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Scan task
static void scan_task(void *pvParameters) {
    while (1) {
        if (scanningActive && addressAssigned) {
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - lastScanTime >= scanParams.intervalMs) {
                perform_scan();
                lastScanTime = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Application entry point
void app_main(void) {
    ESP_LOGI(TAG, "WiFivedra Subordinate Starting");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize UART
    uart_init();

    // Initialize WiFi
    wifi_init();

    // Create tasks
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 5, NULL);
    xTaskCreate(scan_task, "scan_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Subordinate initialized, waiting for address assignment");
}
