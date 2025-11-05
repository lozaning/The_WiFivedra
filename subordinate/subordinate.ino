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
  .lastError = ERR_NONE,
  .freeHeap = 100
};

// Scanning state
bool scanningActive = false;
unsigned long lastScanTime = 0;
unsigned long bootTime = 0;

// Result buffer - increased to handle more results between polls
#define MAX_RESULT_BUFFER 100
WiFiScanResult resultBuffer[MAX_RESULT_BUFFER];
uint16_t resultBufferCount = 0;
uint16_t totalResultsScanned = 0;  // Track total results even if buffer overflows

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

    case CMD_SET_SCAN_PARAMS:
      if (packet.header.length == sizeof(ScanParams)) {
        memcpy(&scanParams, packet.payload, sizeof(ScanParams));
        status.channel = scanParams.channel;
        status.band = scanParams.band;
        protocol.sendAck(CONTROLLER_ADDRESS);
      } else {
        protocol.sendNack(CONTROLLER_ADDRESS, ERR_INVALID_PARAMS);
      }
      break;

    case CMD_START_SCAN:
      if (!scanningActive) {
        scanningActive = true;
        status.state = STATE_SCANNING;
        lastScanTime = 0; // Trigger immediate scan
        protocol.sendAck(CONTROLLER_ADDRESS);
      } else {
        protocol.sendNack(CONTROLLER_ADDRESS, ERR_BUSY);
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
        protocol.sendNack(CONTROLLER_ADDRESS, ERR_INVALID_PARAMS);
      }
      break;

    case CMD_GET_SCAN_RESULTS:
      sendBufferedResults();
      break;

    case CMD_CLEAR_RESULTS:
      resultBufferCount = 0;
      status.resultCount = 0;
      protocol.sendAck(CONTROLLER_ADDRESS);
      break;

    case CMD_SET_SCAN_MODE:
      if (packet.header.length >= 1) {
        scanParams.scanMode = (ScanMode)packet.payload[0];
        protocol.sendAck(CONTROLLER_ADDRESS);
      } else {
        protocol.sendNack(CONTROLLER_ADDRESS, ERR_INVALID_PARAMS);
      }
      break;

    case CMD_SET_SCAN_INTERVAL:
      if (packet.header.length >= 2) {
        memcpy(&scanParams.intervalMs, packet.payload, 2);
        protocol.sendAck(CONTROLLER_ADDRESS);
      } else {
        protocol.sendNack(CONTROLLER_ADDRESS, ERR_INVALID_PARAMS);
      }
      break;

    case CMD_RESET:
      protocol.sendAck(CONTROLLER_ADDRESS);
      delay(100);
      ESP.restart();
      break;

    default:
      protocol.sendNack(CONTROLLER_ADDRESS, ERR_INVALID_COMMAND);
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
    status.lastError = ERR_SCAN_FAILED;
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

    // Track total scanned
    totalResultsScanned++;

    // Buffer result if there's space
    if (resultBufferCount < MAX_RESULT_BUFFER) {
      resultBuffer[resultBufferCount++] = result;
      status.resultCount = resultBufferCount;
    } else {
      // Buffer full - set error but continue scanning
      status.lastError = ERR_BUFFER_FULL;
    }
  }

  // Clean up
  WiFi.scanDelete();

  // Mark scan as complete in status (controller will poll for this)
  status.state = STATE_IDLE;

  // NOTE: We don't send RESP_SCAN_COMPLETE to avoid wire collisions
  // Controller will poll status to check if scan is complete
}

void sendBufferedResults() {
  // Send all buffered results with small delays to prevent overwhelming the chain
  for (uint16_t i = 0; i < resultBufferCount; i++) {
    protocol.sendResponse(CONTROLLER_ADDRESS, RESP_SCAN_RESULT,
                         &resultBuffer[i], sizeof(WiFiScanResult));

    // Add delay every 10 results to let the chain clear
    if ((i + 1) % 10 == 0) {
      delay(20);
    } else {
      delay(5);
    }
  }

  // Send final ACK to indicate transmission complete
  protocol.sendAck(CONTROLLER_ADDRESS);
}
