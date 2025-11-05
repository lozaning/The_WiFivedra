#ifndef PROTOCOL_DEFS_H
#define PROTOCOL_DEFS_H

#include <stdint.h>
#include <stdbool.h>

// Protocol Version
#define PROTOCOL_VERSION 1

// Serial Configuration
#define SERIAL_BAUD_RATE 115200
#define MAX_PACKET_SIZE 512
#define MAX_PAYLOAD_SIZE (MAX_PACKET_SIZE - 10)

// Packet Framing
#define PACKET_START_MARKER 0xAA
#define PACKET_END_MARKER 0x55

// Device Configuration
#define MAX_SUBORDINATES 52
#define CONTROLLER_ADDRESS 0x00
#define UNASSIGNED_ADDRESS 0xFE

// Timing Constants
#define COMMAND_TIMEOUT_MS 5000
#define SCAN_TIMEOUT_MS 10000
#define RESPONSE_DELAY_MS 10
#define ADDRESS_ASSIGNMENT_TIMEOUT_MS 1000
#define DISCOVERY_RETRY_DELAY_MS 100

// Command Types
typedef enum {
    CMD_PING = 0x01,
    CMD_ASSIGN_ADDRESS = 0x02,
    CMD_GPS_UPDATE = 0x03,
    CMD_SET_SCAN_PARAMS = 0x10,
    CMD_START_SCAN = 0x11,
    CMD_STOP_SCAN = 0x12,
    CMD_GET_STATUS = 0x13,
    CMD_SET_CHANNEL = 0x14,
    CMD_GET_SCAN_RESULTS = 0x15,
    CMD_CLEAR_RESULTS = 0x16,
    CMD_SET_SCAN_MODE = 0x17,
    CMD_SET_SCAN_INTERVAL = 0x18,
    CMD_RESET = 0xFF
} CommandType;

// Response Types
typedef enum {
    RESP_ACK = 0x01,
    RESP_NACK = 0x02,
    RESP_ADDRESS_ASSIGNED = 0x03,
    RESP_STATUS = 0x10,
    RESP_SCAN_RESULT = 0x20,
    RESP_SCAN_COMPLETE = 0x21,
    RESP_ERROR = 0xFE
} ResponseType;

// Error Codes
typedef enum {
    PROTO_ERR_NONE = 0x00,
    PROTO_ERR_INVALID_COMMAND = 0x01,
    PROTO_ERR_INVALID_PARAMS = 0x02,
    PROTO_ERR_TIMEOUT = 0x03,
    PROTO_ERR_BUSY = 0x04,
    PROTO_ERR_NOT_READY = 0x05,
    PROTO_ERR_SCAN_FAILED = 0x06,
    PROTO_ERR_BUFFER_FULL = 0x07,
    PROTO_ERR_CHECKSUM = 0x08,
    PROTO_ERR_UNKNOWN = 0xFF
} ErrorCode;

// WiFi Band
typedef enum {
    BAND_2_4GHZ = 0x01,
    BAND_5GHZ = 0x02,
    BAND_BOTH = 0x03
} WiFiBand;

// Scan Mode
typedef enum {
    SCAN_MODE_ACTIVE = 0x01,
    SCAN_MODE_PASSIVE = 0x02
} ScanMode;

// Subordinate State
typedef enum {
    STATE_IDLE = 0x00,
    STATE_SCANNING = 0x01,
    STATE_PROCESSING = 0x02,
    STATE_ERROR = 0xFE
} SubordinateState;

// Packet Header Structure
typedef struct __attribute__((packed)) {
    uint8_t startMarker;
    uint8_t version;
    uint8_t destAddr;
    uint8_t srcAddr;
    uint8_t type;
    uint16_t length;
    uint8_t checksum;
} PacketHeader;

// Full Packet Structure
typedef struct __attribute__((packed)) {
    PacketHeader header;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint8_t endMarker;
} Packet;

// GPS Position Structure
typedef struct __attribute__((packed)) {
    float latitude;
    float longitude;
    float altitude;
    uint8_t satellites;
    uint8_t fixQuality;
    uint32_t timestamp;
} GPSPosition;

// Address Assignment Structure
typedef struct __attribute__((packed)) {
    uint8_t assignedAddress;
    uint8_t isLastNode;
} AddressAssignment;

// Scan Parameters Structure
typedef struct __attribute__((packed)) {
    uint8_t band;
    uint8_t channel;
    uint8_t scanMode;
    uint16_t scanTimeMs;
    uint16_t intervalMs;
    uint8_t hidden;
    uint8_t showHidden;
} ScanParams;

// WiFi Scan Result Structure
typedef struct __attribute__((packed)) {
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t band;
    uint8_t authMode;
    uint32_t timestamp;
    float latitude;
    float longitude;
    float altitude;
    uint8_t gpsQuality;
} WiFiScanResult;

// Status Information Structure
typedef struct __attribute__((packed)) {
    uint8_t state;
    uint8_t channel;
    uint8_t band;
    uint16_t scanCount;
    uint16_t resultCount;
    uint32_t uptime;
    int8_t lastError;
    uint8_t freeHeap;
} StatusInfo;

// Helper function to get channel for 5GHz based on subordinate ID
static inline uint8_t get5GHzChannel(uint8_t subId) {
    const uint8_t channels5GHz[] = {
        36, 40, 44, 48, 52, 56, 60, 64,
        100, 104, 108, 112, 116, 120, 124, 128,
        132, 136, 140, 144, 149, 153, 157, 161, 165
    };
    return channels5GHz[subId % 25];
}

// Calculate simple checksum
static inline uint8_t calculateChecksum(const uint8_t* data, uint16_t length) {
    uint8_t sum = 0;
    for (uint16_t i = 0; i < length; i++) {
        sum ^= data[i];
    }
    return sum;
}

#endif // PROTOCOL_DEFS_H
