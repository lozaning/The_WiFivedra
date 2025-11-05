/**
 * WiFivedra Controller Firmware
 *
 * Controls up to 48 ESP32-C5 subordinates for comprehensive WiFi scanning
 * across all 2.4GHz and 5GHz channels.
 *
 * Hardware: ESP32 (main controller)
 * Communication: UART daisy chain to subordinates
 *   - Serial1 TX -> Sub1 RX (start of chain)
 *   - Serial1 RX <- Sub48 TX (end of chain)
 */

#include "../common/protocol_defs.h"
#include "../common/serial_protocol.h"
#include <SD.h>
#include <SPI.h>

// Pin Configuration
#define SD_CS_PIN 5
#define LED_PIN 2

// Serial communication for daisy chain
// Serial1 TX/RX connects to first and last subordinates in chain
SerialProtocol protocol(&Serial1, CONTROLLER_ADDRESS);

// Subordinate tracking
struct SubordinateInfo {
  uint8_t address;
  bool online;
  StatusInfo status;
  unsigned long lastSeen;
  uint32_t totalResults;
};

SubordinateInfo subordinates[MAX_SUBORDINATES];
uint8_t numSubordinates = 48; // Configurable

// Scan configuration
ScanParams globalScanParams = {
  .band = BAND_5GHZ,
  .channel = 0,  // 0 = assigned channel
  .scanMode = SCAN_MODE_ACTIVE,
  .scanTimeMs = 120,
  .intervalMs = 1000,
  .hidden = 1,
  .showHidden = 1
};

// Data logging
File logFile;
bool sdCardAvailable = false;
unsigned long scanStartTime = 0;
uint32_t totalScansReceived = 0;

// State machine
enum ControllerState {
  CTRL_INIT,
  CTRL_DISCOVERING,
  CTRL_CONFIGURING,
  CTRL_SCANNING,
  CTRL_IDLE
};

ControllerState state = CTRL_INIT;

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  Serial.println("\n\n=== WiFivedra Controller ===");
  Serial.println("Initializing...");

  // Initialize subordinate tracking
  for (uint8_t i = 0; i < MAX_SUBORDINATES; i++) {
    subordinates[i].address = i + 1;
    subordinates[i].online = false;
    subordinates[i].lastSeen = 0;
    subordinates[i].totalResults = 0;
  }

  // Initialize serial protocol
  protocol.begin(SERIAL_BAUD_RATE);
  Serial.println("Serial protocol initialized");

  // Initialize SD card
  if (SD.begin(SD_CS_PIN)) {
    sdCardAvailable = true;
    Serial.println("SD card initialized");
    createNewLogFile();
  } else {
    Serial.println("WARNING: SD card not found - data will not be saved!");
  }

  // Start discovery
  state = CTRL_DISCOVERING;
  discoverSubordinates();
}

void loop() {
  // Handle incoming packets
  Packet packet;
  if (protocol.receivePacket(packet)) {
    handlePacket(packet);
  }

  // State machine
  switch (state) {
    case CTRL_DISCOVERING:
      // Discovery is async, wait for responses
      if (millis() - scanStartTime > 5000) {
        printDiscoveryResults();
        state = CTRL_CONFIGURING;
        configureSubordinates();
      }
      break;

    case CTRL_CONFIGURING:
      // Configuration is async, wait for completion
      if (millis() - scanStartTime > 5000) {
        state = CTRL_SCANNING;
        startScanning();
      }
      break;

    case CTRL_SCANNING:
      // Periodic status check
      if (millis() - scanStartTime > 30000) {
        printStatistics();
        scanStartTime = millis();
      }
      break;

    case CTRL_IDLE:
      // Do nothing
      break;
  }

  // LED heartbeat
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 500) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    lastBlink = millis();
  }

  // Serial command interface
  if (Serial.available()) {
    handleSerialCommand();
  }

  delay(1);
}

void discoverSubordinates() {
  Serial.println("\n--- Discovering Subordinates ---");
  scanStartTime = millis();

  // Send ping to all subordinates
  for (uint8_t i = 0; i < numSubordinates; i++) {
    protocol.sendCommand(subordinates[i].address, CMD_PING);
    delay(10); // Small delay between commands
  }
}

void configureSubordinates() {
  Serial.println("\n--- Configuring Subordinates ---");
  scanStartTime = millis();

  // Configure each subordinate with its channel assignment
  for (uint8_t i = 0; i < numSubordinates; i++) {
    if (subordinates[i].online) {
      ScanParams params = globalScanParams;

      // Assign specific 5GHz channel
      params.channel = get5GHzChannel(i);
      params.band = BAND_5GHZ;

      Serial.printf("Sub %d: Channel %d\n", subordinates[i].address, params.channel);

      protocol.sendCommand(subordinates[i].address, CMD_SET_SCAN_PARAMS,
                          &params, sizeof(params));
      delay(20);
    }
  }
}

void startScanning() {
  Serial.println("\n--- Starting WiFi Scanning ---");
  Serial.println("Wardriving mode active!");
  scanStartTime = millis();

  // Send start scan command to all online subordinates
  for (uint8_t i = 0; i < numSubordinates; i++) {
    if (subordinates[i].online) {
      protocol.sendCommand(subordinates[i].address, CMD_START_SCAN);
      delay(10);
    }
  }
}

void stopScanning() {
  Serial.println("\n--- Stopping WiFi Scanning ---");

  // Send stop scan command to all online subordinates
  for (uint8_t i = 0; i < numSubordinates; i++) {
    if (subordinates[i].online) {
      protocol.sendCommand(subordinates[i].address, CMD_STOP_SCAN);
      delay(10);
    }
  }

  state = CTRL_IDLE;
}

void handlePacket(Packet& packet) {
  uint8_t subIndex = packet.header.srcAddr - 1;

  if (subIndex >= MAX_SUBORDINATES) {
    return; // Invalid address
  }

  // Update subordinate tracking
  subordinates[subIndex].online = true;
  subordinates[subIndex].lastSeen = millis();

  // Handle different response types
  switch (packet.header.type) {
    case RESP_ACK:
      // Acknowledgment received
      break;

    case RESP_STATUS:
      if (packet.header.length == sizeof(StatusInfo)) {
        memcpy(&subordinates[subIndex].status, packet.payload, sizeof(StatusInfo));
      }
      break;

    case RESP_SCAN_RESULT:
      if (packet.header.length == sizeof(WiFiScanResult)) {
        WiFiScanResult result;
        memcpy(&result, packet.payload, sizeof(WiFiScanResult));
        handleScanResult(packet.header.srcAddr, result);
      }
      break;

    case RESP_SCAN_COMPLETE:
      Serial.printf("Sub %d: Scan complete\n", packet.header.srcAddr);
      break;

    case RESP_ERROR:
      if (packet.header.length >= 1) {
        ErrorCode error = (ErrorCode)packet.payload[0];
        Serial.printf("Sub %d: Error %d\n", packet.header.srcAddr, error);
      }
      break;
  }
}

void handleScanResult(uint8_t subAddr, WiFiScanResult& result) {
  uint8_t subIndex = subAddr - 1;
  if (subIndex < MAX_SUBORDINATES) {
    subordinates[subIndex].totalResults++;
  }
  totalScansReceived++;

  // Print to serial
  Serial.printf("[Sub%02d Ch%03d] %02X:%02X:%02X:%02X:%02X:%02X | %-32s | RSSI: %4d dBm\n",
                subAddr, result.channel,
                result.bssid[0], result.bssid[1], result.bssid[2],
                result.bssid[3], result.bssid[4], result.bssid[5],
                result.ssid, result.rssi);

  // Save to SD card
  if (sdCardAvailable) {
    logScanResult(subAddr, result);
  }
}

void logScanResult(uint8_t subAddr, WiFiScanResult& result) {
  if (!logFile) {
    createNewLogFile();
  }

  // CSV format: timestamp,sub_addr,bssid,ssid,rssi,channel,band,auth
  logFile.printf("%lu,%d,%02X:%02X:%02X:%02X:%02X:%02X,\"%s\",%d,%d,%d,%d\n",
                 result.timestamp, subAddr,
                 result.bssid[0], result.bssid[1], result.bssid[2],
                 result.bssid[3], result.bssid[4], result.bssid[5],
                 result.ssid, result.rssi, result.channel, result.band, result.authMode);

  // Periodic flush
  static uint32_t flushCounter = 0;
  if (++flushCounter % 50 == 0) {
    logFile.flush();
  }
}

void createNewLogFile() {
  char filename[32];
  snprintf(filename, sizeof(filename), "/wardrive_%lu.csv", millis());

  logFile = SD.open(filename, FILE_WRITE);
  if (logFile) {
    // Write CSV header
    logFile.println("timestamp,sub_addr,bssid,ssid,rssi,channel,band,auth");
    logFile.flush();
    Serial.printf("Created log file: %s\n", filename);
  }
}

void printDiscoveryResults() {
  uint8_t onlineCount = 0;
  Serial.println("\n--- Discovery Results ---");

  for (uint8_t i = 0; i < numSubordinates; i++) {
    if (subordinates[i].online) {
      onlineCount++;
      Serial.printf("Sub %d: ONLINE\n", subordinates[i].address);
    }
  }

  Serial.printf("Total: %d/%d online\n", onlineCount, numSubordinates);
}

void printStatistics() {
  Serial.println("\n--- Statistics ---");
  Serial.printf("Total networks detected: %lu\n", totalScansReceived);
  Serial.printf("Runtime: %lu seconds\n", millis() / 1000);

  uint8_t onlineCount = 0;
  for (uint8_t i = 0; i < numSubordinates; i++) {
    if (subordinates[i].online) {
      onlineCount++;
    }
  }
  Serial.printf("Active subordinates: %d/%d\n", onlineCount, numSubordinates);
}

void handleSerialCommand() {
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd == "status" || cmd == "s") {
    printStatistics();
  } else if (cmd == "start") {
    startScanning();
  } else if (cmd == "stop") {
    stopScanning();
  } else if (cmd == "discover") {
    state = CTRL_DISCOVERING;
    discoverSubordinates();
  } else if (cmd == "config") {
    configureSubordinates();
  } else if (cmd.startsWith("subs ")) {
    numSubordinates = cmd.substring(5).toInt();
    if (numSubordinates > MAX_SUBORDINATES) {
      numSubordinates = MAX_SUBORDINATES;
    }
    Serial.printf("Number of subordinates set to: %d\n", numSubordinates);
  } else if (cmd == "help") {
    Serial.println("\nCommands:");
    Serial.println("  status (s)  - Show statistics");
    Serial.println("  start       - Start scanning");
    Serial.println("  stop        - Stop scanning");
    Serial.println("  discover    - Discover subordinates");
    Serial.println("  config      - Configure subordinates");
    Serial.println("  subs <n>    - Set number of subordinates");
    Serial.println("  help        - Show this help");
  }
}
