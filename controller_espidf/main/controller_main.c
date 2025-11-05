/**
 * WiFivedra Controller Firmware - ESP-IDF Version
 *
 * Controls up to 52 ESP32-C5 subordinates for comprehensive WiFi scanning
 * across all 2.4GHz and 5GHz channels.
 *
 * Hardware: ESP32
 * Communication: Daisy chain - Direct UART connection to first subordinate
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "protocol_defs.h"

static const char *TAG = "WiFivedra-Ctrl";

// Pin Configuration
#define SD_MISO_PIN     GPIO_NUM_2
#define SD_MOSI_PIN     GPIO_NUM_15
#define SD_CLK_PIN      GPIO_NUM_14
#define SD_CS_PIN       GPIO_NUM_13
#define LED_PIN         GPIO_NUM_2

// Downstream UART (to first subordinate)
#define DOWNSTREAM_UART_NUM     UART_NUM_1
#define DOWNSTREAM_TX_PIN       GPIO_NUM_17
#define DOWNSTREAM_RX_PIN       GPIO_NUM_16
#define DOWNSTREAM_BUF_SIZE     2048

// GPS UART
#define GPS_UART_NUM            UART_NUM_2
#define GPS_TX_PIN              GPIO_NUM_19
#define GPS_RX_PIN              GPIO_NUM_18
#define GPS_BUF_SIZE            1024

// Subordinate tracking
typedef struct {
    uint8_t address;
    bool online;
    StatusInfo status;
    uint32_t lastSeen;
    uint32_t totalResults;
} SubordinateInfo;

// Global state
static SubordinateInfo subordinates[MAX_SUBORDINATES];
static uint8_t numSubordinates = 0;
static uint8_t lastSubordinateAddress = 0;
static uint8_t currentPollIndex = 0;
static uint8_t pendingResultsFrom = 0;
static bool waitingForResults = false;
static uint32_t totalScansReceived = 0;

// GPS state
static GPSPosition currentGPS = {0};
static bool hasValidGPS = false;

// GPS time tracking
typedef struct {
    uint8_t day;
    uint8_t month;
    uint16_t year;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint32_t referenceMillis;
    bool valid;
    bool dateValid;
} GPSTime;

static GPSTime gpsTime = {0};

// SD card
static FILE *logFile = NULL;
static bool sdCardAvailable = false;
static uint16_t currentSessionNumber = 0;
static sdmmc_card_t *card = NULL;

// State machine
typedef enum {
    CTRL_INIT,
    CTRL_AUTO_DISCOVERING,
    CTRL_DISCOVERING,
    CTRL_CONFIGURING,
    CTRL_SCANNING,
    CTRL_IDLE
} ControllerState;

static ControllerState state = CTRL_INIT;
static uint32_t stateStartTime = 0;

// Scan configuration
static ScanParams globalScanParams = {
    .band = BAND_5GHZ,
    .channel = 0,
    .scanMode = SCAN_MODE_ACTIVE,
    .scanTimeMs = 120,
    .intervalMs = 1000,
    .hidden = 1,
    .showHidden = 1
};

// Forward declarations
static void uart_init(void);
static void sd_card_init(void);
static bool send_packet(Packet *packet);
static bool receive_packet(Packet *packet, uint32_t timeout_ms);
static void handle_packet(Packet *packet);
static void auto_discover_subordinates(void);
static void configure_subordinates(void);
static void start_scanning(void);
static void poll_subordinate_for_results(uint8_t index);
static void handle_scan_result(uint8_t subAddr, WiFiScanResult *result);
static void log_scan_result(uint8_t subAddr, WiFiScanResult *result);
static void create_new_log_file(void);
static uint16_t find_next_session_number(void);
static void parse_nmea(const char *sentence);
static void timestamp_to_iso8601(uint32_t timestamp, char *buffer, size_t bufsize);
static const char* auth_mode_to_wigle(uint8_t authMode);
static float get_gps_accuracy(uint8_t gpsQuality);
static void gps_task(void *pvParameters);
static void main_task(void *pvParameters);

// UART initialization
static void uart_init(void) {
    // Configure downstream UART
    uart_config_t uart_config = {
        .baud_rate = SERIAL_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(DOWNSTREAM_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(DOWNSTREAM_UART_NUM, DOWNSTREAM_TX_PIN, DOWNSTREAM_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(DOWNSTREAM_UART_NUM, DOWNSTREAM_BUF_SIZE * 2, 0, 0, NULL, 0));

    // Configure GPS UART
    uart_config.baud_rate = 9600;
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM, GPS_TX_PIN, GPS_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, GPS_BUF_SIZE * 2, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "UART initialized");
}

// SD card initialization
static void sd_card_init(void) {
    esp_err_t ret;

    // Options for mounting the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus");
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);

    if (ret == ESP_OK) {
        sdCardAvailable = true;
        ESP_LOGI(TAG, "SD card mounted successfully");

        // Find next session number
        currentSessionNumber = find_next_session_number();
        create_new_log_file();
    } else {
        ESP_LOGW(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
    }
}

// Find next available session number
static uint16_t find_next_session_number(void) {
    uint16_t maxSession = 0;
    DIR *dir = opendir("/sdcard");

    if (dir == NULL) {
        return 1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "wigle_", 6) == 0 && strstr(entry->d_name, ".csv") != NULL) {
            int sessionNum = atoi(entry->d_name + 6);
            if (sessionNum > maxSession) {
                maxSession = sessionNum;
            }
        }
    }

    closedir(dir);
    return maxSession + 1;
}

// Create new WiGLE log file
static void create_new_log_file(void) {
    char filename[64];
    snprintf(filename, sizeof(filename), "/sdcard/wigle_%04d.csv", currentSessionNumber);

    logFile = fopen(filename, "w");
    if (logFile) {
        fprintf(logFile, "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n");
        fflush(logFile);
        ESP_LOGI(TAG, "Created WiGLE log file: %s", filename);
    } else {
        ESP_LOGE(TAG, "Failed to create log file");
    }
}

// Convert auth mode to WiGLE format
static const char* auth_mode_to_wigle(uint8_t authMode) {
    switch (authMode) {
        case 0: return "[Open]";
        case 1: return "[WEP]";
        case 2: return "[WPA]";
        case 3: return "[WPA2]";
        case 4: return "[WPA2]";
        case 5: return "[WPA2-EAP]";
        case 6: return "[WPA3]";
        case 7: return "[WPA3]";
        case 8: return "[WAPI]";
        default: return "[Unknown]";
    }
}

// Get GPS accuracy
static float get_gps_accuracy(uint8_t gpsQuality) {
    switch (gpsQuality) {
        case 0: return 0.0f;
        case 1: return 15.0f;
        case 2: return 3.0f;
        default: return 0.0f;
    }
}

// Convert timestamp to ISO 8601 format
static void timestamp_to_iso8601(uint32_t timestamp, char *buffer, size_t bufsize) {
    if (!gpsTime.valid) {
        snprintf(buffer, bufsize, "0000-00-00 00:00:00");
        return;
    }

    uint32_t elapsedMs = timestamp - gpsTime.referenceMillis;
    uint32_t totalSeconds = gpsTime.hour * 3600 + gpsTime.minute * 60 + gpsTime.second + (elapsedMs / 1000);

    uint32_t daysElapsed = totalSeconds / 86400;
    totalSeconds = totalSeconds % 86400;

    uint8_t hours = (totalSeconds / 3600) % 24;
    uint8_t minutes = (totalSeconds / 60) % 60;
    uint8_t seconds = totalSeconds % 60;

    if (gpsTime.dateValid) {
        uint16_t year = gpsTime.year;
        uint8_t month = gpsTime.month;
        uint8_t day = gpsTime.day + daysElapsed;

        const uint8_t daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

        while (day > daysInMonth[month - 1]) {
            day -= daysInMonth[month - 1];
            month++;
            if (month > 12) {
                month = 1;
                year++;
            }
        }

        snprintf(buffer, bufsize, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hours, minutes, seconds);
    } else {
        snprintf(buffer, bufsize, "0000-00-00 %02d:%02d:%02d", hours, minutes, seconds);
    }
}

// Log scan result
static void log_scan_result(uint8_t subAddr, WiFiScanResult *result) {
    if (!logFile) {
        create_new_log_file();
    }

    if (!logFile) return;

    char timestamp[32];
    timestamp_to_iso8601(result->timestamp, timestamp, sizeof(timestamp));

    const char* authMode = auth_mode_to_wigle(result->authMode);
    float accuracy = get_gps_accuracy(result->gpsQuality);

    fprintf(logFile, "%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%s,%d,%d,%.8f,%.8f,%.2f,%.1f,WIFI\n",
            result->bssid[0], result->bssid[1], result->bssid[2],
            result->bssid[3], result->bssid[4], result->bssid[5],
            result->ssid,
            authMode,
            timestamp,
            result->channel,
            result->rssi,
            result->latitude,
            result->longitude,
            result->altitude,
            accuracy);

    static uint32_t flushCounter = 0;
    if (++flushCounter % 50 == 0) {
        fflush(logFile);
    }
}

// Handle scan result
static void handle_scan_result(uint8_t subAddr, WiFiScanResult *result) {
    uint8_t subIndex = subAddr - 1;
    if (subIndex < MAX_SUBORDINATES) {
        subordinates[subIndex].totalResults++;
    }
    totalScansReceived++;

    ESP_LOGI(TAG, "[Sub%02d Ch%03d] %02X:%02X:%02X:%02X:%02X:%02X | %-32s | RSSI: %4d dBm",
             subAddr, result->channel,
             result->bssid[0], result->bssid[1], result->bssid[2],
             result->bssid[3], result->bssid[4], result->bssid[5],
             result->ssid, result->rssi);

    if (sdCardAvailable) {
        log_scan_result(subAddr, result);
    }
}

// Send packet
static bool send_packet(Packet *packet) {
    uint8_t buffer[sizeof(Packet)];
    memcpy(buffer, packet, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), packet->payload, packet->header.length);
    buffer[sizeof(PacketHeader) + packet->header.length] = packet->endMarker;

    size_t totalSize = sizeof(PacketHeader) + packet->header.length + 1;
    int written = uart_write_bytes(DOWNSTREAM_UART_NUM, buffer, totalSize);

    return (written == totalSize);
}

// Receive packet
static bool receive_packet(Packet *packet, uint32_t timeout_ms) {
    uint8_t buffer[MAX_PACKET_SIZE];
    int len = uart_read_bytes(DOWNSTREAM_UART_NUM, buffer, sizeof(PacketHeader), pdMS_TO_TICKS(timeout_ms));

    if (len != sizeof(PacketHeader)) {
        return false;
    }

    memcpy(&packet->header, buffer, sizeof(PacketHeader));

    if (packet->header.startMarker != PACKET_START_MARKER) {
        return false;
    }

    if (packet->header.length > 0) {
        len = uart_read_bytes(DOWNSTREAM_UART_NUM, packet->payload, packet->header.length, pdMS_TO_TICKS(100));
        if (len != packet->header.length) {
            return false;
        }
    }

    len = uart_read_bytes(DOWNSTREAM_UART_NUM, &packet->endMarker, 1, pdMS_TO_TICKS(10));
    if (len != 1 || packet->endMarker != PACKET_END_MARKER) {
        return false;
    }

    return true;
}

// Handle incoming packet
static void handle_packet(Packet *packet) {
    uint8_t subIndex = packet->header.srcAddr - 1;

    if (subIndex >= MAX_SUBORDINATES) {
        return;
    }

    subordinates[subIndex].online = true;
    subordinates[subIndex].lastSeen = xTaskGetTickCount() * portTICK_PERIOD_MS;

    switch (packet->header.type) {
        case RESP_ACK:
            if (pendingResultsFrom == packet->header.srcAddr) {
                // Send clear results command
                Packet clearPacket = {0};
                clearPacket.header.startMarker = PACKET_START_MARKER;
                clearPacket.header.version = PROTOCOL_VERSION;
                clearPacket.header.destAddr = packet->header.srcAddr;
                clearPacket.header.srcAddr = CONTROLLER_ADDRESS;
                clearPacket.header.type = CMD_CLEAR_RESULTS;
                clearPacket.header.length = 0;
                clearPacket.endMarker = PACKET_END_MARKER;
                send_packet(&clearPacket);

                pendingResultsFrom = 0;
                waitingForResults = false;
            }
            break;

        case RESP_ADDRESS_ASSIGNED:
            if (packet->header.length == sizeof(AddressAssignment)) {
                AddressAssignment *assignment = (AddressAssignment*)packet->payload;
                ESP_LOGI(TAG, "Subordinate #%d registered%s", assignment->assignedAddress,
                        assignment->isLastNode ? " (LAST NODE)" : "");
                if (assignment->isLastNode) {
                    lastSubordinateAddress = assignment->assignedAddress;
                }
                numSubordinates++;
            }
            break;

        case RESP_SCAN_RESULT:
            if (packet->header.length == sizeof(WiFiScanResult)) {
                WiFiScanResult result;
                memcpy(&result, packet->payload, sizeof(WiFiScanResult));
                handle_scan_result(packet->header.srcAddr, &result);
            }
            break;

        default:
            break;
    }
}

// Auto-discover subordinates
static void auto_discover_subordinates(void) {
    ESP_LOGI(TAG, "Auto-discovering subordinates...");

    Packet packet = {0};
    packet.header.startMarker = PACKET_START_MARKER;
    packet.header.version = PROTOCOL_VERSION;
    packet.header.destAddr = UNASSIGNED_ADDRESS;
    packet.header.srcAddr = CONTROLLER_ADDRESS;
    packet.header.type = CMD_ASSIGN_ADDRESS;
    packet.header.length = sizeof(AddressAssignment);
    packet.endMarker = PACKET_END_MARKER;

    AddressAssignment assignment = {
        .assignedAddress = 1,
        .isLastNode = 0
    };
    memcpy(packet.payload, &assignment, sizeof(AddressAssignment));

    send_packet(&packet);
    ESP_LOGI(TAG, "Sent address assignment command");
}

// Configure subordinates
static void configure_subordinates(void) {
    ESP_LOGI(TAG, "Configuring subordinates...");

    for (uint8_t i = 0; i < numSubordinates; i++) {
        if (subordinates[i].online) {
            ScanParams params = globalScanParams;
            params.channel = get5GHzChannel(i);
            params.band = BAND_5GHZ;

            Packet packet = {0};
            packet.header.startMarker = PACKET_START_MARKER;
            packet.header.version = PROTOCOL_VERSION;
            packet.header.destAddr = subordinates[i].address;
            packet.header.srcAddr = CONTROLLER_ADDRESS;
            packet.header.type = CMD_SET_SCAN_PARAMS;
            packet.header.length = sizeof(ScanParams);
            packet.endMarker = PACKET_END_MARKER;
            memcpy(packet.payload, &params, sizeof(ScanParams));

            send_packet(&packet);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// Start scanning
static void start_scanning(void) {
    ESP_LOGI(TAG, "Starting WiFi scanning");

    for (uint8_t i = 0; i < numSubordinates; i++) {
        if (subordinates[i].online) {
            Packet packet = {0};
            packet.header.startMarker = PACKET_START_MARKER;
            packet.header.version = PROTOCOL_VERSION;
            packet.header.destAddr = subordinates[i].address;
            packet.header.srcAddr = CONTROLLER_ADDRESS;
            packet.header.type = CMD_START_SCAN;
            packet.header.length = 0;
            packet.endMarker = PACKET_END_MARKER;

            send_packet(&packet);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// Poll subordinate for results
static void poll_subordinate_for_results(uint8_t index) {
    if (index >= numSubordinates || !subordinates[index].online) {
        waitingForResults = false;
        return;
    }

    Packet packet = {0};
    packet.header.startMarker = PACKET_START_MARKER;
    packet.header.version = PROTOCOL_VERSION;
    packet.header.destAddr = subordinates[index].address;
    packet.header.srcAddr = CONTROLLER_ADDRESS;
    packet.header.type = CMD_GET_SCAN_RESULTS;
    packet.header.length = 0;
    packet.endMarker = PACKET_END_MARKER;

    send_packet(&packet);
    pendingResultsFrom = subordinates[index].address;
}

// Parse NMEA sentence
static void parse_nmea(const char *sentence) {
    if (strncmp(sentence, "$GPGGA", 6) == 0 || strncmp(sentence, "$GNGGA", 6) == 0) {
        // Parse GGA for position
        char *tokens[15];
        char *str = strdup(sentence);
        char *token = strtok(str, ",");
        int count = 0;

        while (token != NULL && count < 15) {
            tokens[count++] = token;
            token = strtok(NULL, ",");
        }

        if (count >= 10) {
            // Parse time
            if (strlen(tokens[1]) >= 6) {
                gpsTime.hour = (tokens[1][0] - '0') * 10 + (tokens[1][1] - '0');
                gpsTime.minute = (tokens[1][2] - '0') * 10 + (tokens[1][3] - '0');
                gpsTime.second = (tokens[1][4] - '0') * 10 + (tokens[1][5] - '0');
                gpsTime.referenceMillis = xTaskGetTickCount() * portTICK_PERIOD_MS;
                gpsTime.valid = true;
            }

            // Parse position
            if (strlen(tokens[2]) > 0 && strlen(tokens[4]) > 0) {
                float lat = atof(tokens[2]);
                float latDeg = floor(lat / 100.0f);
                float latMin = lat - (latDeg * 100.0f);
                lat = latDeg + (latMin / 60.0f);
                if (tokens[3][0] == 'S') lat = -lat;

                float lon = atof(tokens[4]);
                float lonDeg = floor(lon / 100.0f);
                float lonMin = lon - (lonDeg * 100.0f);
                lon = lonDeg + (lonMin / 60.0f);
                if (tokens[5][0] == 'W') lon = -lon;

                currentGPS.latitude = lat;
                currentGPS.longitude = lon;
                currentGPS.altitude = atof(tokens[9]);
                currentGPS.satellites = atoi(tokens[7]);
                currentGPS.fixQuality = atoi(tokens[6]);
                currentGPS.timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;
                hasValidGPS = (currentGPS.fixQuality > 0);
            }
        }

        free(str);
    }
    else if (strncmp(sentence, "$GPRMC", 6) == 0 || strncmp(sentence, "$GNRMC", 6) == 0) {
        // Parse RMC for date
        char *tokens[15];
        char *str = strdup(sentence);
        char *token = strtok(str, ",");
        int count = 0;

        while (token != NULL && count < 15) {
            tokens[count++] = token;
            token = strtok(NULL, ",");
        }

        if (count >= 10 && tokens[2][0] == 'A') {
            // Parse date
            if (strlen(tokens[9]) >= 6) {
                gpsTime.day = (tokens[9][0] - '0') * 10 + (tokens[9][1] - '0');
                gpsTime.month = (tokens[9][2] - '0') * 10 + (tokens[9][3] - '0');
                uint8_t yearShort = (tokens[9][4] - '0') * 10 + (tokens[9][5] - '0');
                gpsTime.year = 2000 + yearShort;
                gpsTime.dateValid = true;
            }
        }

        free(str);
    }
}

// GPS task
static void gps_task(void *pvParameters) {
    uint8_t data[GPS_BUF_SIZE];
    char nmea_buffer[256] = {0};
    int nmea_idx = 0;
    uint32_t lastBroadcast = 0;

    while (1) {
        int len = uart_read_bytes(GPS_UART_NUM, data, GPS_BUF_SIZE, pdMS_TO_TICKS(100));

        for (int i = 0; i < len; i++) {
            if (data[i] == '\n') {
                nmea_buffer[nmea_idx] = '\0';
                parse_nmea(nmea_buffer);
                nmea_idx = 0;
            } else if (data[i] != '\r' && nmea_idx < sizeof(nmea_buffer) - 1) {
                nmea_buffer[nmea_idx++] = data[i];
            }
        }

        // Broadcast GPS every second
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - lastBroadcast >= 1000 && state == CTRL_SCANNING && numSubordinates > 0) {
            Packet packet = {0};
            packet.header.startMarker = PACKET_START_MARKER;
            packet.header.version = PROTOCOL_VERSION;
            packet.header.destAddr = 0xFF; // Broadcast
            packet.header.srcAddr = CONTROLLER_ADDRESS;
            packet.header.type = CMD_GPS_UPDATE;
            packet.header.length = sizeof(GPSPosition);
            packet.endMarker = PACKET_END_MARKER;
            memcpy(packet.payload, &currentGPS, sizeof(GPSPosition));

            send_packet(&packet);
            lastBroadcast = now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Main controller task
static void main_task(void *pvParameters) {
    // Initialize subordinate tracking
    for (int i = 0; i < MAX_SUBORDINATES; i++) {
        subordinates[i].address = i + 1;
        subordinates[i].online = false;
        subordinates[i].lastSeen = 0;
        subordinates[i].totalResults = 0;
    }

    // Start auto-discovery
    state = CTRL_AUTO_DISCOVERING;
    stateStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
    auto_discover_subordinates();

    uint32_t lastStatsPrint = 0;

    while (1) {
        // Handle incoming packets
        Packet packet;
        if (receive_packet(&packet, 10)) {
            handle_packet(&packet);
        }

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // State machine
        switch (state) {
            case CTRL_AUTO_DISCOVERING:
                if (now - stateStartTime > 10000) {
                    ESP_LOGI(TAG, "Auto-discovery complete. Found %d subordinates", numSubordinates);
                    state = CTRL_CONFIGURING;
                    stateStartTime = now;
                    configure_subordinates();
                }
                break;

            case CTRL_CONFIGURING:
                if (now - stateStartTime > 5000) {
                    state = CTRL_SCANNING;
                    stateStartTime = now;
                    start_scanning();
                }
                break;

            case CTRL_SCANNING:
                if (numSubordinates > 0 && !waitingForResults) {
                    poll_subordinate_for_results(currentPollIndex);
                    currentPollIndex = (currentPollIndex + 1) % numSubordinates;
                    waitingForResults = true;
                }

                // Print statistics every 30 seconds
                if (now - lastStatsPrint > 30000) {
                    ESP_LOGI(TAG, "Stats - Total networks: %lu, Active subs: %d/%d",
                            totalScansReceived, numSubordinates, numSubordinates);
                    lastStatsPrint = now;
                }
                break;

            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Application entry point
void app_main(void) {
    ESP_LOGI(TAG, "WiFivedra Controller Starting");

    // Initialize UART
    uart_init();

    // Initialize SD card
    sd_card_init();

    // Create tasks
    xTaskCreate(gps_task, "gps_task", 4096, NULL, 5, NULL);
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "Controller initialized");
}
