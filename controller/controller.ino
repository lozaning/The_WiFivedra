/**
 * WiFivedra Controller Firmware
 *
 * Controls up to 48 ESP32-C5 subordinates for comprehensive WiFi scanning
 * across all 2.4GHz and 5GHz channels.
 *
 * Hardware: ESP32 (main controller)
 * Communication: Daisy chain - Direct UART connection to first subordinate
 *
 * Daisy Chain Topology:
 *   Controller -> Sub1 <-> Sub2 <-> Sub3 <-> ... <-> Sub48
 *
 * The controller only connects to the first subordinate.
 * Messages are automatically forwarded down the chain by each subordinate.
 */

#include "../common/protocol_defs.h"
#include "../common/serial_protocol.h"
#include <SD.h>
#include <SPI.h>

// Pin Configuration
#define SD_CS_PIN 5
#define LED_PIN 2

// Downstream UART Pin Configuration (connection to first subordinate)
#define DOWNSTREAM_TX_PIN 17  // ESP32 TX to Sub1's RX
#define DOWNSTREAM_RX_PIN 16  // ESP32 RX from Sub1's TX

// GPS UART Pin Configuration
#define GPS_TX_PIN 19  // ESP32 TX (not used for GPS input)
#define GPS_RX_PIN 18  // ESP32 RX from GPS module

// Serial ports
HardwareSerial downstreamSerial(1);  // Serial1 for daisy chain
HardwareSerial gpsSerial(2);          // Serial2 for GPS
SerialProtocol protocol(&downstreamSerial, CONTROLLER_ADDRESS);

// Subordinate tracking
struct SubordinateInfo {
  uint8_t address;
  bool online;
  StatusInfo status;
  unsigned long lastSeen;
  uint32_t totalResults;
};

SubordinateInfo subordinates[MAX_SUBORDINATES];
uint8_t numSubordinates = 0; // Auto-discovered during startup
uint8_t lastSubordinateAddress = 0;

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

// Result polling
uint8_t currentPollIndex = 0;  // Round-robin polling
uint8_t pendingResultsFrom = 0;  // Track which subordinate we're receiving results from
bool waitingForResults = false;  // True when waiting for a subordinate to finish sending results

// GPS tracking
GPSPosition currentGPS = {0};
unsigned long lastGPSBroadcast = 0;
#define GPS_BROADCAST_INTERVAL_MS 1000  // Broadcast GPS every 1 second
String nmeaBuffer = "";  // Buffer for NMEA sentence parsing

// GPS time tracking for absolute timestamps
struct GPSTime {
  uint8_t day;
  uint8_t month;
  uint16_t year;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint16_t millisecond;
  uint32_t referenceMillis;  // millis() when GPS time was captured
  bool valid;
  bool dateValid;  // Date is only available from RMC sentences
};
GPSTime gpsTime = {0, 0, 0, 0, 0, 0, 0, 0, false, false};

// Wardriving session tracking
uint16_t currentSessionNumber = 0;

// State machine
enum ControllerState {
  CTRL_INIT,
  CTRL_AUTO_DISCOVERING,  // Auto-assign addresses
  CTRL_DISCOVERING,        // Ping discovered devices
  CTRL_CONFIGURING,
  CTRL_SCANNING,
  CTRL_IDLE
};

ControllerState state = CTRL_INIT;

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  Serial.println("\n\n=== WiFivedra Controller ===");
  Serial.println("Daisy Chain Mode");
  Serial.println("Initializing...");

  // Initialize subordinate tracking
  for (uint8_t i = 0; i < MAX_SUBORDINATES; i++) {
    subordinates[i].address = i + 1;
    subordinates[i].online = false;
    subordinates[i].lastSeen = 0;
    subordinates[i].totalResults = 0;
  }

  // Configure downstream UART pins
  downstreamSerial.begin(SERIAL_BAUD_RATE, SERIAL_8N1, DOWNSTREAM_RX_PIN, DOWNSTREAM_TX_PIN);

  // Initialize serial protocol
  protocol.begin(SERIAL_BAUD_RATE);
  Serial.println("Serial protocol initialized (daisy chain)");

  // Initialize GPS
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS serial initialized (9600 baud)");

  // Initialize SD card
  if (SD.begin(SD_CS_PIN)) {
    sdCardAvailable = true;
    Serial.println("SD card initialized");
    createNewLogFile();
  } else {
    Serial.println("WARNING: SD card not found - data will not be saved!");
  }

  // Start auto-discovery (address assignment)
  state = CTRL_AUTO_DISCOVERING;
  autoDiscoverSubordinates();
}

void loop() {
  // Parse GPS data
  readGPS();

  // Broadcast GPS to subordinates periodically
  if (millis() - lastGPSBroadcast >= GPS_BROADCAST_INTERVAL_MS) {
    broadcastGPS();
    lastGPSBroadcast = millis();
  }

  // Handle incoming packets
  Packet packet;
  if (protocol.receivePacket(packet)) {
    handlePacket(packet);
  }

  // State machine
  switch (state) {
    case CTRL_AUTO_DISCOVERING:
      // Auto-discovery is async, wait for all subordinates to respond
      // Timeout after 10 seconds (generous for long chains)
      if (millis() - scanStartTime > 10000) {
        printAutoDiscoveryResults();
        state = CTRL_DISCOVERING;
        discoverSubordinates();  // Ping all discovered devices
      }
      break;

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
      // Poll subordinates continuously when not waiting for results
      if (numSubordinates > 0 && !waitingForResults) {
        pollSubordinateForResults(currentPollIndex);
        currentPollIndex = (currentPollIndex + 1) % numSubordinates;
        waitingForResults = true;
      }

      // Periodic status printout
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

void autoDiscoverSubordinates() {
  Serial.println("\n--- Auto-Discovering Subordinates ---");
  Serial.println("Assigning addresses via daisy chain...");
  scanStartTime = millis();

  // Send CMD_ASSIGN_ADDRESS with address=1 to the first subordinate
  AddressAssignment assignment;
  assignment.assignedAddress = 1;
  assignment.isLastNode = 0;

  protocol.sendCommand(UNASSIGNED_ADDRESS, CMD_ASSIGN_ADDRESS,
                      &assignment, sizeof(assignment));

  Serial.println("Sent address assignment command downstream");
  Serial.println("Waiting for subordinates to report...");
}

void printAutoDiscoveryResults() {
  Serial.println("\n--- Auto-Discovery Complete ---");
  Serial.printf("Total subordinates discovered: %d\n", numSubordinates);

  if (lastSubordinateAddress > 0) {
    Serial.printf("Last subordinate in chain: #%d\n", lastSubordinateAddress);
  }

  for (uint8_t i = 0; i < numSubordinates; i++) {
    Serial.printf("  Sub %d: ", subordinates[i].address);
    if (subordinates[i].address == lastSubordinateAddress) {
      Serial.println("(LAST NODE)");
    } else {
      Serial.println("");
    }
  }
}

void discoverSubordinates() {
  Serial.println("\n--- Pinging Subordinates ---");
  scanStartTime = millis();

  // Send ping to all discovered subordinates
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

void pollSubordinateForResults(uint8_t index) {
  if (index >= numSubordinates || !subordinates[index].online) {
    return;
  }

  uint8_t subAddr = subordinates[index].address;

  // Request scan results from this subordinate
  protocol.sendCommand(subAddr, CMD_GET_SCAN_RESULTS);

  // Mark that we're expecting results from this subordinate
  pendingResultsFrom = subAddr;

  // Note: Results will come back asynchronously via handlePacket()
  // When we receive the final ACK, we'll send CMD_CLEAR_RESULTS to free the buffer
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
      // Check if this is the final ACK from result transmission
      if (pendingResultsFrom == packet.header.srcAddr) {
        // Result transmission complete, clear the subordinate's buffer
        protocol.sendCommand(packet.header.srcAddr, CMD_CLEAR_RESULTS);
        pendingResultsFrom = 0;  // Clear pending flag
        waitingForResults = false;  // Ready to poll next subordinate
      }
      break;

    case RESP_ADDRESS_ASSIGNED:
      if (packet.header.length == sizeof(AddressAssignment)) {
        AddressAssignment confirmation;
        memcpy(&confirmation, packet.payload, sizeof(AddressAssignment));

        Serial.printf("Subordinate #%d registered", confirmation.assignedAddress);
        if (confirmation.isLastNode) {
          Serial.print(" (LAST NODE)");
          lastSubordinateAddress = confirmation.assignedAddress;
        }
        Serial.println();

        numSubordinates++;
      }
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

  // WiGLE CSV format: MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type
  String timestamp = timestampToISO8601(result.timestamp);
  const char* authMode = authModeToWiGLE(result.authMode);
  float accuracy = getGPSAccuracy(result.gpsQuality);

  logFile.printf("%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%s,%d,%d,%.8f,%.8f,%.2f,%.1f,WIFI\n",
                 result.bssid[0], result.bssid[1], result.bssid[2],
                 result.bssid[3], result.bssid[4], result.bssid[5],
                 result.ssid,
                 authMode,
                 timestamp.c_str(),
                 result.channel,
                 result.rssi,
                 result.latitude,
                 result.longitude,
                 result.altitude,
                 accuracy);

  // Periodic flush
  static uint32_t flushCounter = 0;
  if (++flushCounter % 50 == 0) {
    logFile.flush();
  }
}

void createNewLogFile() {
  // Use session number if not already set
  if (currentSessionNumber == 0) {
    currentSessionNumber = findNextSessionNumber();
  }

  char filename[32];
  snprintf(filename, sizeof(filename), "/wigle_%04d.csv", currentSessionNumber);

  logFile = SD.open(filename, FILE_WRITE);
  if (logFile) {
    // Write WiGLE CSV header
    logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
    logFile.flush();
    Serial.printf("Created WiGLE log file: %s\n", filename);
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
  } else if (cmd == "autodiscover") {
    // Reset and re-run auto-discovery
    numSubordinates = 0;
    lastSubordinateAddress = 0;
    state = CTRL_AUTO_DISCOVERING;
    autoDiscoverSubordinates();
  } else if (cmd == "discover") {
    state = CTRL_DISCOVERING;
    discoverSubordinates();
  } else if (cmd == "config") {
    configureSubordinates();
  } else if (cmd == "help") {
    Serial.println("\nCommands:");
    Serial.println("  status (s)     - Show statistics");
    Serial.println("  start          - Start scanning");
    Serial.println("  stop           - Stop scanning");
    Serial.println("  autodiscover   - Auto-assign addresses to subordinates");
    Serial.println("  discover       - Ping all discovered subordinates");
    Serial.println("  config         - Configure subordinates");
    Serial.println("  help           - Show this help");
  }
}

// Read and parse GPS data from NMEA sentences
void readGPS() {
  while (gpsSerial.available()) {
    char c = gpsSerial.read();

    if (c == '\n') {
      // Complete NMEA sentence received
      parseNMEA(nmeaBuffer);
      nmeaBuffer = "";
    } else if (c != '\r') {
      nmeaBuffer += c;
    }
  }
}

// Parse NMEA sentence (focusing on GGA for position data)
void parseNMEA(String sentence) {
  if (sentence.startsWith("$GPGGA") || sentence.startsWith("$GNGGA")) {
    // Example: $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
    // Split by commas
    int idx[15];
    int count = 0;
    idx[0] = 0;

    for (int i = 0; i < sentence.length() && count < 14; i++) {
      if (sentence[i] == ',') {
        idx[++count] = i + 1;
      }
    }

    if (count >= 9) {
      // Parse time (HHMMSS.sss format)
      String timeStr = sentence.substring(idx[0], idx[1] - 1);

      // Parse latitude
      String latStr = sentence.substring(idx[1], idx[2] - 1);
      String latDir = sentence.substring(idx[2], idx[3] - 1);

      // Parse longitude
      String lonStr = sentence.substring(idx[3], idx[4] - 1);
      String lonDir = sentence.substring(idx[4], idx[5] - 1);

      // Parse fix quality
      String fixStr = sentence.substring(idx[5], idx[6] - 1);

      // Parse number of satellites
      String satStr = sentence.substring(idx[6], idx[7] - 1);

      // Parse altitude
      String altStr = sentence.substring(idx[8], idx[9] - 1);

      // Parse GPS time if available
      if (timeStr.length() >= 6) {
        gpsTime.hour = timeStr.substring(0, 2).toInt();
        gpsTime.minute = timeStr.substring(2, 4).toInt();
        gpsTime.second = timeStr.substring(4, 6).toInt();
        if (timeStr.length() > 7 && timeStr.indexOf('.') > 0) {
          gpsTime.millisecond = timeStr.substring(7).toInt();
        } else {
          gpsTime.millisecond = 0;
        }
        gpsTime.referenceMillis = millis();
        gpsTime.valid = true;
      }

      if (latStr.length() > 0 && lonStr.length() > 0) {
        // Convert NMEA format (DDMM.MMMM) to decimal degrees
        float lat = latStr.substring(0, 2).toFloat() + latStr.substring(2).toFloat() / 60.0;
        if (latDir == "S") lat = -lat;

        float lon = lonStr.substring(0, 3).toFloat() + lonStr.substring(3).toFloat() / 60.0;
        if (lonDir == "W") lon = -lon;

        currentGPS.latitude = lat;
        currentGPS.longitude = lon;
        currentGPS.altitude = altStr.toFloat();
        currentGPS.satellites = satStr.toInt();
        currentGPS.fixQuality = fixStr.toInt();
        currentGPS.timestamp = millis();
      }
    }
  }
  else if (sentence.startsWith("$GPRMC") || sentence.startsWith("$GNRMC")) {
    // Example: $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
    // Split by commas
    int idx[15];
    int count = 0;
    idx[0] = 0;

    for (int i = 0; i < sentence.length() && count < 14; i++) {
      if (sentence[i] == ',') {
        idx[++count] = i + 1;
      }
    }

    if (count >= 9) {
      // Parse time (HHMMSS.sss format)
      String timeStr = sentence.substring(idx[0], idx[1] - 1);

      // Parse status (A=active/valid, V=void/invalid)
      String statusStr = sentence.substring(idx[1], idx[2] - 1);

      // Only process if we have a valid fix
      if (statusStr == "A") {
        // Parse latitude
        String latStr = sentence.substring(idx[2], idx[3] - 1);
        String latDir = sentence.substring(idx[3], idx[4] - 1);

        // Parse longitude
        String lonStr = sentence.substring(idx[4], idx[5] - 1);
        String lonDir = sentence.substring(idx[5], idx[6] - 1);

        // Skip speed (idx[6]) and course (idx[7])

        // Parse date (DDMMYY format)
        String dateStr = sentence.substring(idx[8], idx[9] - 1);

        // Parse GPS time if available
        if (timeStr.length() >= 6) {
          gpsTime.hour = timeStr.substring(0, 2).toInt();
          gpsTime.minute = timeStr.substring(2, 4).toInt();
          gpsTime.second = timeStr.substring(4, 6).toInt();
          if (timeStr.length() > 7 && timeStr.indexOf('.') > 0) {
            gpsTime.millisecond = timeStr.substring(7).toInt();
          } else {
            gpsTime.millisecond = 0;
          }
          gpsTime.referenceMillis = millis();
          gpsTime.valid = true;
        }

        // Parse GPS date if available
        if (dateStr.length() >= 6) {
          gpsTime.day = dateStr.substring(0, 2).toInt();
          gpsTime.month = dateStr.substring(2, 4).toInt();
          uint8_t yearShort = dateStr.substring(4, 6).toInt();
          // Convert 2-digit year to 4-digit (assume 2000-2099)
          gpsTime.year = 2000 + yearShort;
          gpsTime.dateValid = true;
        }

        // Parse position data
        if (latStr.length() > 0 && lonStr.length() > 0) {
          // Convert NMEA format (DDMM.MMMM) to decimal degrees
          float lat = latStr.substring(0, 2).toFloat() + latStr.substring(2).toFloat() / 60.0;
          if (latDir == "S") lat = -lat;

          float lon = lonStr.substring(0, 3).toFloat() + lonStr.substring(3).toFloat() / 60.0;
          if (lonDir == "W") lon = -lon;

          currentGPS.latitude = lat;
          currentGPS.longitude = lon;
          // RMC doesn't provide altitude or satellites, keep existing values
          currentGPS.timestamp = millis();

          // Set fix quality to 1 (GPS fix) since we have valid RMC data
          if (currentGPS.fixQuality == 0) {
            currentGPS.fixQuality = 1;
          }
        }
      }
    }
  }
}

// Broadcast current GPS position to all subordinates
void broadcastGPS() {
  if (numSubordinates > 0 && state == CTRL_SCANNING) {
    // Broadcast to all subordinates (address 0xFF = broadcast)
    protocol.sendCommand(0xFF, CMD_GPS_UPDATE, &currentGPS, sizeof(GPSPosition));
  }
}

// Convert timestamp from millis() to ISO 8601 format using GPS time reference
// Returns format: YYYY-MM-DD HH:MM:SS
String timestampToISO8601(uint32_t timestamp) {
  if (!gpsTime.valid) {
    // No GPS time available, return placeholder
    return "0000-00-00 00:00:00";
  }

  // Calculate elapsed time since GPS time reference
  uint32_t elapsedMs = timestamp - gpsTime.referenceMillis;

  // Calculate total seconds from GPS time + elapsed time
  uint32_t totalSeconds = gpsTime.hour * 3600 + gpsTime.minute * 60 + gpsTime.second + (elapsedMs / 1000);

  // Calculate day overflow
  uint32_t daysElapsed = totalSeconds / 86400;  // 86400 seconds in a day
  totalSeconds = totalSeconds % 86400;

  // Calculate time components
  uint8_t hours = (totalSeconds / 3600) % 24;
  uint8_t minutes = (totalSeconds / 60) % 60;
  uint8_t seconds = totalSeconds % 60;

  // Use real date if available from RMC, otherwise placeholder
  char buffer[32];
  if (gpsTime.dateValid) {
    // Calculate date with day overflow
    uint16_t year = gpsTime.year;
    uint8_t month = gpsTime.month;
    uint8_t day = gpsTime.day + daysElapsed;

    // Simple day overflow handling (approximate, doesn't handle month/year boundaries perfectly)
    // Days per month (not accounting for leap years)
    const uint8_t daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    while (day > daysInMonth[month - 1]) {
      day -= daysInMonth[month - 1];
      month++;
      if (month > 12) {
        month = 1;
        year++;
      }
    }

    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hours, minutes, seconds);
  } else {
    // No date available, use placeholder
    snprintf(buffer, sizeof(buffer), "0000-00-00 %02d:%02d:%02d", hours, minutes, seconds);
  }

  return String(buffer);
}

// Convert ESP32 WiFi auth mode to WiGLE format string
const char* authModeToWiGLE(uint8_t authMode) {
  switch (authMode) {
    case 0:  // WIFI_AUTH_OPEN
      return "[Open]";
    case 1:  // WIFI_AUTH_WEP
      return "[WEP]";
    case 2:  // WIFI_AUTH_WPA_PSK
      return "[WPA]";
    case 3:  // WIFI_AUTH_WPA2_PSK
      return "[WPA2]";
    case 4:  // WIFI_AUTH_WPA_WPA2_PSK
      return "[WPA2]";
    case 5:  // WIFI_AUTH_WPA2_ENTERPRISE
      return "[WPA2-EAP]";
    case 6:  // WIFI_AUTH_WPA3_PSK
      return "[WPA3]";
    case 7:  // WIFI_AUTH_WPA2_WPA3_PSK
      return "[WPA3]";
    case 8:  // WIFI_AUTH_WAPI_PSK
      return "[WAPI]";
    default:
      return "[Unknown]";
  }
}

// Get GPS accuracy in meters based on fix quality
float getGPSAccuracy(uint8_t gpsQuality) {
  switch (gpsQuality) {
    case 0:  // No fix
      return 0.0;
    case 1:  // GPS fix
      return 15.0;
    case 2:  // DGPS fix
      return 3.0;
    default:
      return 0.0;
  }
}

// Find next available session number by scanning SD card
uint16_t findNextSessionNumber() {
  uint16_t maxSession = 0;
  File root = SD.open("/");

  if (!root) {
    return 1;
  }

  File file = root.openNextFile();
  while (file) {
    String filename = String(file.name());

    // Look for files matching pattern: wigle_NNNN.csv
    if (filename.startsWith("wigle_") && filename.endsWith(".csv")) {
      // Extract session number
      int startIdx = 6;  // Length of "wigle_"
      int endIdx = filename.indexOf(".csv");
      if (endIdx > startIdx) {
        String numberStr = filename.substring(startIdx, endIdx);
        uint16_t sessionNum = numberStr.toInt();
        if (sessionNum > maxSession) {
          maxSession = sessionNum;
        }
      }
    }

    file.close();
    file = root.openNextFile();
  }

  root.close();
  return maxSession + 1;
}
