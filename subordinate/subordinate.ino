/**
 * WiFivedra Subordinate Firmware
 *
 * Performs WiFi scanning on assigned channels and reports results to controller.
 *
 * Hardware: ESP32-C5
 * Communication: Serial to controller
 */

#include "../common/protocol_defs.h"
#include "../common/serial_protocol.h"
#include <WiFi.h>

// Configuration - SET THIS FOR EACH DEVICE
#define MY_ADDRESS 1  // Change this for each subordinate (1-48)

// Pin Configuration
#define LED_PIN 2

// Serial communication
SerialProtocol protocol(&Serial, MY_ADDRESS);

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

// Result buffer
#define MAX_RESULT_BUFFER 50
WiFiScanResult resultBuffer[MAX_RESULT_BUFFER];
uint16_t resultBufferCount = 0;

void setup() {
  pinMode(LED_PIN, OUTPUT);
  bootTime = millis();

  // Initialize serial protocol
  protocol.begin(SERIAL_BAUD_RATE);

  // Set WiFi to station mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Initialize status
  status.channel = scanParams.channel;
  status.band = scanParams.band;

  delay(100);
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
  if (scanningActive) {
    // Fast blink when scanning
    digitalWrite(LED_PIN, (millis() / 100) % 2);
  } else {
    // Slow blink when idle
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

    // Send result immediately to controller
    protocol.sendResponse(CONTROLLER_ADDRESS, RESP_SCAN_RESULT, &result, sizeof(result));

    // Also buffer if there's space
    if (resultBufferCount < MAX_RESULT_BUFFER) {
      resultBuffer[resultBufferCount++] = result;
      status.resultCount = resultBufferCount;
    }

    // Small delay between results
    delay(5);
  }

  // Clean up
  WiFi.scanDelete();

  // Send scan complete notification
  protocol.sendResponse(CONTROLLER_ADDRESS, RESP_SCAN_COMPLETE, nullptr, 0);

  status.state = STATE_IDLE;
}

void sendBufferedResults() {
  for (uint16_t i = 0; i < resultBufferCount; i++) {
    protocol.sendResponse(CONTROLLER_ADDRESS, RESP_SCAN_RESULT,
                         &resultBuffer[i], sizeof(WiFiScanResult));
    delay(5);
  }
  protocol.sendAck(CONTROLLER_ADDRESS);
}
