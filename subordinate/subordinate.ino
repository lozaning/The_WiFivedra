/**
 * WiFivedra Subordinate Firmware
 *
 * Performs WiFi scanning on assigned channels and reports results to controller.
 *
 * Hardware: ESP32-C5
 * Communication: Daisy chain - Direct UART connection to left and right neighbors
 *
 * Daisy Chain Topology:
 *   Controller <-> Sub1 <-> Sub2 <-> ... <-> Sub48
 *
 * Each subordinate (except ends):
 *   - Upstream (left) connection: Receives from/sends to previous device (toward controller)
 *   - Downstream (right) connection: Receives from/sends to next device (away from controller)
 */

#include "../common/protocol_defs.h"
#include "../common/serial_protocol.h"
#include <WiFi.h>

// Forward declarations
void handleAddressAssignment(Packet& packet);
void handleCommand(Packet& packet);
void performScan();
int16_t findSeenNetwork(const uint8_t* bssid);
void moveSeenNetworkToTop(uint16_t index);
void addToSeenNetworks(const uint8_t* bssid, uint32_t timestamp);
bool processNetworkResult(const WiFiScanResult& result);
void sendBufferedResults();

// Pin Configuration
#define LED_PIN 2

// UART Pin Configuration
// Upstream Serial (to previous device / toward controller)
#define UPSTREAM_TX_PIN 21   // ESP32-C5 TX to previous device's RX
#define UPSTREAM_RX_PIN 20   // ESP32-C5 RX from previous device's TX

// Downstream Serial (to next device / away from controller)
#define DOWNSTREAM_TX_PIN 17 // ESP32-C5 TX to next device's RX
#define DOWNSTREAM_RX_PIN 16 // ESP32-C5 RX from next device's TX

// Serial communication
// Use Serial1 for upstream, Serial2 for downstream
HardwareSerial upstreamSerial(1);    // UART1
HardwareSerial downstreamSerial(2);  // UART2

// Start with unassigned address - will be auto-assigned during discovery
SerialProtocol protocol(&upstreamSerial, &downstreamSerial, UNASSIGNED_ADDRESS, false);

// Auto-discovery state
bool addressAssigned = false;
uint8_t myAssignedAddress = UNASSIGNED_ADDRESS;

// Scan parameters
ScanParams scanParams = {
  .band = BAND_5GHZ,
  .channel = 36,
  .scanMode = SCAN_MODE_ACTIVE,
  .scanTimeMs = 120,
  .intervalMs = 1000,
  .hidden = 1,
  .showHidden = 1
};

// Status information
StatusInfo status = {
  .state = STATE_IDLE,
  .channel = 0,
  .band = BAND_5GHZ,
  .scanCount = 0,
  .resultCount = 0,
  .uptime = 0,
  .lastError = PROTO_ERR_NONE,
  .freeHeap = 100
};

// Scanning state
bool scanningActive = false;
unsigned long lastScanTime = 0;
unsigned long bootTime = 0;

// Seen networks tracking
#define MAX_SEEN_NETWORKS 500
struct SeenNetwork {
  uint8_t bssid[6];        // MAC address (unique identifier)
  uint32_t lastSeen;       // Timestamp when last seen
  uint16_t seenCount;      // How many times we've seen this network
};

SeenNetwork seenNetworks[MAX_SEEN_NETWORKS];
uint16_t seenNetworksCount = 0;

// New networks buffer - only networks not previously seen
#define MAX_NEW_NETWORKS 100
WiFiScanResult newNetworksBuffer[MAX_NEW_NETWORKS];
uint16_t newNetworksCount = 0;

uint16_t totalNetworksScanned = 0;  // Total networks found in scans
uint16_t totalNewNetworks = 0;      // Total new (unique) networks discovered

// GPS caching - stores most recent GPS position from controller
GPSPosition cachedGPS = {
  .latitude = 0.0,
  .longitude = 0.0,
  .altitude = 0.0,
  .satellites = 0,
  .fixQuality = 0,
  .timestamp = 0
};
bool hasValidGPS = false;

void setup() {
  pinMode(LED_PIN, OUTPUT);
  bootTime = millis();

  // Configure UART pins - always configure both initially
  upstreamSerial.begin(SERIAL_BAUD_RATE, SERIAL_8N1, UPSTREAM_RX_PIN, UPSTREAM_TX_PIN);
  downstreamSerial.begin(SERIAL_BAUD_RATE, SERIAL_8N1, DOWNSTREAM_RX_PIN, DOWNSTREAM_TX_PIN);

  // Initialize serial protocol
  protocol.begin(SERIAL_BAUD_RATE);

  // Set WiFi to station mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Initialize status
  status.channel = scanParams.channel;
  status.band = scanParams.band;

  // Wait for address assignment - blink LED rapidly
  while (!addressAssigned) {
    digitalWrite(LED_PIN, (millis() / 100) % 2);

    Packet packet;
    if (protocol.receivePacket(packet)) {
      if (packet.header.type == CMD_ASSIGN_ADDRESS) {
        handleAddressAssignment(packet);
      }
    }
    delay(10);
  }

  delay(100);
}

void handleAddressAssignment(Packet& packet) {
  if (packet.header.length != sizeof(AddressAssignment)) {
    return;  // Invalid payload
  }

  AddressAssignment assignment;
  memcpy(&assignment, packet.payload, sizeof(AddressAssignment));

  // Adopt the assigned address
  myAssignedAddress = assignment.assignedAddress;
  protocol.setAddress(myAssignedAddress);
  addressAssigned = true;

  // Try to assign address to next subordinate downstream
  bool hasDownstream = protocol.tryAssignDownstream(myAssignedAddress + 1);

  if (!hasDownstream) {
    // No downstream device responded - this is the last node
    protocol.setEndNode(true);
  }

  // Send confirmation upstream to controller
  AddressAssignment confirmation;
  confirmation.assignedAddress = myAssignedAddress;
  confirmation.isLastNode = protocol.getIsEndNode() ? 1 : 0;

  protocol.sendResponse(CONTROLLER_ADDRESS, RESP_ADDRESS_ASSIGNED,
                       &confirmation, sizeof(confirmation));
}

void loop() {
  // Handle incoming commands
  Packet packet;
  if (protocol.receivePacket(packet)) {
    handleCommand(packet);
  }

  // Perform scanning if active
  if (scanningActive) {
    if (millis() - lastScanTime >= scanParams.intervalMs) {
      performScan();
      lastScanTime = millis();
    }
  }

  // Update status
  status.uptime = (millis() - bootTime) / 1000;
  status.freeHeap = (ESP.getFreeHeap() * 100) / ESP.getHeapSize();

  // LED indicator
  if (!addressAssigned) {
    // Very fast blink when waiting for address
    digitalWrite(LED_PIN, (millis() / 100) % 2);
  } else if (scanningActive) {
    // Fast blink when scanning
    digitalWrite(LED_PIN, (millis() / 200) % 2);
  } else {
    // Slow blink when idle and addressed
    digitalWrite(LED_PIN, (millis() / 1000) % 2);
  }

  delay(1);
}

void handleCommand(Packet& packet) {
  CommandType cmd = (CommandType)packet.header.type;

  switch (cmd) {
    case CMD_PING:
      protocol.sendAck(CONTROLLER_ADDRESS);
      break;

    case CMD_GPS_UPDATE:
      // Update cached GPS position from controller
      if (packet.header.length == sizeof(GPSPosition)) {
        memcpy(&cachedGPS, packet.payload, sizeof(GPSPosition));
        hasValidGPS = (cachedGPS.fixQuality > 0);
        // No ACK needed for broadcast GPS updates (reduces traffic)
      }
      break;

    case CMD_SET_SCAN_PARAMS:
      if (packet.header.length == sizeof(ScanParams)) {
        memcpy(&scanParams, packet.payload, sizeof(ScanParams));
        status.channel = scanParams.channel;
        status.band = scanParams.band;
        protocol.sendAck(CONTROLLER_ADDRESS);
      } else {
        protocol.sendNack(CONTROLLER_ADDRESS, PROTO_ERR_INVALID_PARAMS);
      }
      break;

    case CMD_START_SCAN:
      if (!scanningActive) {
        scanningActive = true;
        status.state = STATE_SCANNING;
        lastScanTime = 0; // Trigger immediate scan
        protocol.sendAck(CONTROLLER_ADDRESS);
      } else {
        protocol.sendNack(CONTROLLER_ADDRESS, PROTO_ERR_BUSY);
      }
      break;

    case CMD_STOP_SCAN:
      scanningActive = false;
      status.state = STATE_IDLE;
      protocol.sendAck(CONTROLLER_ADDRESS);
      break;

    case CMD_GET_STATUS:
      protocol.sendResponse(CONTROLLER_ADDRESS, RESP_STATUS, &status, sizeof(status));
      break;

    case CMD_SET_CHANNEL:
      if (packet.header.length >= 1) {
        scanParams.channel = packet.payload[0];
        status.channel = scanParams.channel;
        protocol.sendAck(CONTROLLER_ADDRESS);
      } else {
        protocol.sendNack(CONTROLLER_ADDRESS, PROTO_ERR_INVALID_PARAMS);
      }
      break;

    case CMD_GET_SCAN_RESULTS:
      sendBufferedResults();
      break;

    case CMD_CLEAR_RESULTS:
      // Clear only the NEW networks buffer (keep seen networks list intact)
      newNetworksCount = 0;
      status.resultCount = 0;
      protocol.sendAck(CONTROLLER_ADDRESS);
      break;

    case CMD_SET_SCAN_MODE:
      if (packet.header.length >= 1) {
        scanParams.scanMode = (ScanMode)packet.payload[0];
        protocol.sendAck(CONTROLLER_ADDRESS);
      } else {
        protocol.sendNack(CONTROLLER_ADDRESS, PROTO_ERR_INVALID_PARAMS);
      }
      break;

    case CMD_SET_SCAN_INTERVAL:
      if (packet.header.length >= 2) {
        memcpy(&scanParams.intervalMs, packet.payload, 2);
        protocol.sendAck(CONTROLLER_ADDRESS);
      } else {
        protocol.sendNack(CONTROLLER_ADDRESS, PROTO_ERR_INVALID_PARAMS);
      }
      break;

    case CMD_RESET:
      protocol.sendAck(CONTROLLER_ADDRESS);
      delay(100);
      ESP.restart();
      break;

    default:
      protocol.sendNack(CONTROLLER_ADDRESS, PROTO_ERR_INVALID_COMMAND);
      break;
  }
}

void performScan() {
  status.state = STATE_SCANNING;

  // Configure WiFi for specific channel if needed
  // Note: ESP32-C5 specific WiFi configuration may be needed here

  int16_t scanResult;

  if (scanParams.scanMode == SCAN_MODE_ACTIVE) {
    // Active scan
    scanResult = WiFi.scanNetworks(false, scanParams.showHidden, false,
                                   scanParams.scanTimeMs, scanParams.channel);
  } else {
    // Passive scan
    scanResult = WiFi.scanNetworks(false, scanParams.showHidden, true,
                                   scanParams.scanTimeMs, scanParams.channel);
  }

  if (scanResult < 0) {
    // Scan failed
    status.lastError = PROTO_ERR_SCAN_FAILED;
    return;
  }

  status.scanCount++;

  // Process scan results
  for (int i = 0; i < scanResult; i++) {
    WiFiScanResult result;

    // Get BSSID
    uint8_t* bssid = WiFi.BSSID(i);
    if (bssid) {
      memcpy(result.bssid, bssid, 6);
    }

    // Get SSID
    String ssid = WiFi.SSID(i);
    strncpy(result.ssid, ssid.c_str(), sizeof(result.ssid) - 1);
    result.ssid[sizeof(result.ssid) - 1] = '\0';

    // Get other parameters
    result.rssi = WiFi.RSSI(i);
    result.channel = WiFi.channel(i);
    result.authMode = WiFi.encryptionType(i);
    result.timestamp = millis();

    // Determine band based on channel
    if (result.channel <= 13) {
      result.band = BAND_2_4GHZ;
    } else {
      result.band = BAND_5GHZ;
    }

    // Add GPS coordinates from cached position
    // These are from when the scan happened, not when transmitted to controller
    result.latitude = cachedGPS.latitude;
    result.longitude = cachedGPS.longitude;
    result.altitude = cachedGPS.altitude;
    result.gpsQuality = cachedGPS.fixQuality;

    // Check if this is a new network or one we've seen before
    bool isNewNetwork = processNetworkResult(result);

    // Only buffer NEW networks (not previously seen)
    if (isNewNetwork) {
      if (newNetworksCount < MAX_NEW_NETWORKS) {
        newNetworksBuffer[newNetworksCount++] = result;
        status.resultCount = newNetworksCount;
      } else {
        // Buffer full - set error but continue scanning
        status.lastError = PROTO_ERR_BUFFER_FULL;
      }
    }
  }

  // Clean up
  WiFi.scanDelete();

  // Mark scan as complete in status (controller will poll for this)
  status.state = STATE_IDLE;

  // NOTE: We don't send RESP_SCAN_COMPLETE to avoid wire collisions
  // Controller will poll status to check if scan is complete
}

// Check if a BSSID exists in the seen networks list
// Returns index if found, -1 if not found
int16_t findSeenNetwork(const uint8_t* bssid) {
  for (uint16_t i = 0; i < seenNetworksCount; i++) {
    if (memcmp(seenNetworks[i].bssid, bssid, 6) == 0) {
      return i;
    }
  }
  return -1;
}

// Move a seen network to the top of the list (most recent)
void moveSeenNetworkToTop(uint16_t index) {
  if (index == 0) return;  // Already at top

  // Save the network
  SeenNetwork temp = seenNetworks[index];

  // Shift all networks above it down one position
  for (uint16_t i = index; i > 0; i--) {
    seenNetworks[i] = seenNetworks[i - 1];
  }

  // Place the network at the top
  seenNetworks[0] = temp;
}

// Add a new network to the seen list (at the top)
void addToSeenNetworks(const uint8_t* bssid, uint32_t timestamp) {
  // If list is full, the oldest entry (at the end) gets pushed out
  if (seenNetworksCount < MAX_SEEN_NETWORKS) {
    seenNetworksCount++;
  }

  // Shift all entries down one position
  for (uint16_t i = seenNetworksCount - 1; i > 0; i--) {
    seenNetworks[i] = seenNetworks[i - 1];
  }

  // Add new network at top
  memcpy(seenNetworks[0].bssid, bssid, 6);
  seenNetworks[0].lastSeen = timestamp;
  seenNetworks[0].seenCount = 1;
}

// Process a scan result: check if new or seen before
// Returns true if this is a NEW network that should be reported
bool processNetworkResult(const WiFiScanResult& result) {
  totalNetworksScanned++;

  int16_t seenIndex = findSeenNetwork(result.bssid);

  if (seenIndex >= 0) {
    // Network has been seen before
    // Update the seen count and timestamp
    seenNetworks[seenIndex].seenCount++;
    seenNetworks[seenIndex].lastSeen = result.timestamp;

    // Move to top of seen list (most recent)
    moveSeenNetworkToTop(seenIndex);

    return false;  // Not a new network
  } else {
    // This is a NEW network we haven't seen before
    addToSeenNetworks(result.bssid, result.timestamp);
    totalNewNetworks++;

    return true;  // New network - should be reported
  }
}

void sendBufferedResults() {
  // Send only NEW networks (not previously seen)
  for (uint16_t i = 0; i < newNetworksCount; i++) {
    protocol.sendResponse(CONTROLLER_ADDRESS, RESP_SCAN_RESULT,
                         &newNetworksBuffer[i], sizeof(WiFiScanResult));
    // No delay needed - controller polls continuously
  }

  // Send final ACK to indicate transmission complete
  protocol.sendAck(CONTROLLER_ADDRESS);
}
