#ifndef PROTOCOL_DEFS_H
#define PROTOCOL_DEFS_H

#include <Arduino.h>

// Protocol Version
#define PROTOCOL_VERSION 1

// Serial Configuration
#define SERIAL_BAUD_RATE 115200
#define MAX_PACKET_SIZE 512
#define MAX_PAYLOAD_SIZE (MAX_PACKET_SIZE - 10) // Minus header and footer

// Packet Framing
#define PACKET_START_MARKER 0xAA
#define PACKET_END_MARKER 0x55

// Device Configuration
#define MAX_SUBORDINATES 52
#define CONTROLLER_ADDRESS 0x00
#define UNASSIGNED_ADDRESS 0xFE  // Subordinates boot with this address

// Timing Constants
#define COMMAND_TIMEOUT_MS 5000
#define SCAN_TIMEOUT_MS 10000
#define RESPONSE_DELAY_MS 10
#define ADDRESS_ASSIGNMENT_TIMEOUT_MS 1000  // Timeout to detect last node
#define DISCOVERY_RETRY_DELAY_MS 100

// Command Types (Controller -> Subordinate)
enum CommandType : uint8_t {
  CMD_PING = 0x01,                    // Test connectivity
  CMD_ASSIGN_ADDRESS = 0x02,          // Auto-discovery address assignment
  CMD_GPS_UPDATE = 0x03,              // Broadcast current GPS position
  CMD_SET_SCAN_PARAMS = 0x10,         // Set WiFi scan parameters
  CMD_START_SCAN = 0x11,              // Start WiFi scanning
  CMD_STOP_SCAN = 0x12,               // Stop WiFi scanning
  CMD_GET_STATUS = 0x13,              // Get subordinate status
  CMD_SET_CHANNEL = 0x14,             // Set specific channel to monitor
  CMD_GET_SCAN_RESULTS = 0x15,        // Request scan results
  CMD_CLEAR_RESULTS = 0x16,           // Clear stored scan results
  CMD_SET_SCAN_MODE = 0x17,           // Set scan mode (active/passive)
  CMD_SET_SCAN_INTERVAL = 0x18,       // Set scan interval
  CMD_RESET = 0xFF                    // Reset subordinate
};

// Response Types (Subordinate -> Controller)
enum ResponseType : uint8_t {
  RESP_ACK = 0x01,                    // Acknowledgment
  RESP_NACK = 0x02,                   // Negative acknowledgment
  RESP_ADDRESS_ASSIGNED = 0x03,       // Address assignment confirmation
  RESP_STATUS = 0x10,                 // Status information
  RESP_SCAN_RESULT = 0x20,            // WiFi scan result
  RESP_SCAN_COMPLETE = 0x21,          // Scan complete notification
  RESP_ERROR = 0xFE                   // Error response
};

// Error Codes
// Note: Prefixed with PROTO_ to avoid conflicts with system error codes (e.g., lwIP's ERR_TIMEOUT)
enum ErrorCode : uint8_t {
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
};

// WiFi Band
enum WiFiBand : uint8_t {
  BAND_2_4GHZ = 0x01,
  BAND_5GHZ = 0x02,
  BAND_BOTH = 0x03
};

// Scan Mode
enum ScanMode : uint8_t {
  SCAN_MODE_ACTIVE = 0x01,
  SCAN_MODE_PASSIVE = 0x02
};

// Subordinate State
enum SubordinateState : uint8_t {
  STATE_IDLE = 0x00,
  STATE_SCANNING = 0x01,
  STATE_PROCESSING = 0x02,
  STATE_ERROR = 0xFE
};

// Packet Header Structure
struct __attribute__((packed)) PacketHeader {
  uint8_t startMarker;    // Always PACKET_START_MARKER
  uint8_t version;        // Protocol version
  uint8_t destAddr;       // Destination address (0x00 = controller, 0x01-0x30 = subordinates)
  uint8_t srcAddr;        // Source address
  uint8_t type;           // Command or Response type
  uint16_t length;        // Payload length
  uint8_t seqNum;         // Sequence number
};

// Packet Footer Structure
struct __attribute__((packed)) PacketFooter {
  uint8_t checksum;       // Simple XOR checksum
  uint8_t endMarker;      // Always PACKET_END_MARKER
};

// Complete Packet Structure
struct Packet {
  PacketHeader header;
  uint8_t payload[MAX_PAYLOAD_SIZE];
  PacketFooter footer;

  // Calculate checksum
  uint8_t calculateChecksum() {
    uint8_t cs = 0;
    cs ^= header.version;
    cs ^= header.destAddr;
    cs ^= header.srcAddr;
    cs ^= header.type;
    cs ^= (header.length >> 8) & 0xFF;
    cs ^= header.length & 0xFF;
    cs ^= header.seqNum;
    for (uint16_t i = 0; i < header.length; i++) {
      cs ^= payload[i];
    }
    return cs;
  }

  // Verify checksum
  bool verifyChecksum() {
    return footer.checksum == calculateChecksum();
  }
};

// GPS Position Structure
struct __attribute__((packed)) GPSPosition {
  float latitude;           // Latitude in decimal degrees (-90 to +90)
  float longitude;          // Longitude in decimal degrees (-180 to +180)
  float altitude;           // Altitude in meters
  uint8_t satellites;       // Number of satellites
  uint8_t fixQuality;       // 0=no fix, 1=GPS fix, 2=DGPS fix
  uint32_t timestamp;       // GPS timestamp (milliseconds since epoch or boot)
};

// Address Assignment Structure
struct __attribute__((packed)) AddressAssignment {
  uint8_t assignedAddress;  // Address to assign to the receiving subordinate
  uint8_t isLastNode;       // 1 if this is determined to be the last node, 0 otherwise
};

// Scan Parameters Structure
struct __attribute__((packed)) ScanParams {
  uint8_t band;           // WiFiBand
  uint8_t channel;        // Specific channel (0 = all channels for band)
  uint8_t scanMode;       // ScanMode
  uint16_t scanTimeMs;    // Time to spend on each channel (ms)
  uint16_t intervalMs;    // Interval between scans (ms)
  uint8_t hidden;         // Scan for hidden networks (0 = no, 1 = yes)
  uint8_t showHidden;     // Show hidden networks in results (0 = no, 1 = yes)
};

// WiFi Scan Result Structure
struct __attribute__((packed)) WiFiScanResult {
  uint8_t bssid[6];       // MAC address
  char ssid[33];          // Network name (max 32 chars + null)
  int8_t rssi;            // Signal strength
  uint8_t channel;        // Channel number
  uint8_t band;           // WiFiBand
  uint8_t authMode;       // Authentication mode
  uint32_t timestamp;     // Time of scan (ms since boot)
  // GPS coordinates from when network was scanned
  float latitude;         // Latitude in decimal degrees
  float longitude;        // Longitude in decimal degrees
  float altitude;         // Altitude in meters
  uint8_t gpsQuality;     // GPS fix quality (0=no fix, 1=GPS, 2=DGPS)
};

// Status Information Structure
struct __attribute__((packed)) StatusInfo {
  uint8_t state;          // SubordinateState
  uint8_t channel;        // Current channel
  uint8_t band;           // Current band
  uint16_t scanCount;     // Number of scans performed
  uint16_t resultCount;   // Number of results in buffer
  uint32_t uptime;        // Uptime in seconds
  int8_t lastError;       // Last error code
  uint8_t freeHeap;       // Free heap percentage
};

// Helper function to get channel for 5GHz based on subordinate ID
inline uint8_t get5GHzChannel(uint8_t subId) {
  // 5GHz channels: 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
  //                132, 136, 140, 144, 149, 153, 157, 161, 165
  // We have 25 unique 5GHz channels, so we'll cycle through them for 52 subs
  const uint8_t channels5GHz[] = {
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128,
    132, 136, 140, 144, 149, 153, 157, 161, 165
  };

  // Use modulo to cycle through channels for subordinates > 25
  uint8_t channelIndex = subId % (sizeof(channels5GHz) / sizeof(channels5GHz[0]));
  return channels5GHz[channelIndex];
}

// Helper function to get channel for 2.4GHz based on subordinate ID
inline uint8_t get24GHzChannel(uint8_t subId) {
  // 2.4GHz channels: 1-13 (14 in some regions)
  if (subId >= 1 && subId <= 13) {
    return subId;
  }
  return 1; // Default
}

#endif // PROTOCOL_DEFS_H
