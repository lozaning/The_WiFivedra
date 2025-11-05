# WiFivedra Configuration Examples

This document provides example configurations for different use cases.

## Example 1: Full 5GHz Coverage (Default)

Perfect for urban wardriving where 5GHz networks are prevalent.

### Configuration

```cpp
// In controller.ino setup():
numSubordinates = 48;

// Scan parameters
globalScanParams = {
  .band = BAND_5GHZ,
  .channel = 0,  // Auto-assigned per subordinate
  .scanMode = SCAN_MODE_ACTIVE,
  .scanTimeMs = 120,
  .intervalMs = 1000,
  .hidden = 1,
  .showHidden = 1
};
```

### Channel Distribution

- Subordinates 1-25: 5GHz channels (36-165)
- Subordinates 26-48: Duplicate coverage of high-density channels

### Best For
- Urban areas with many 5GHz networks
- Enterprise WiFi surveys
- Dense apartment complexes

---

## Example 2: Mixed Band Coverage

Balanced coverage of both 2.4GHz and 5GHz.

### Configuration

Modify `configureSubordinates()` in controller.ino:

```cpp
void configureSubordinates() {
  Serial.println("\n--- Configuring Subordinates ---");
  scanStartTime = millis();

  for (uint8_t i = 0; i < numSubordinates; i++) {
    if (subordinates[i].online) {
      ScanParams params = globalScanParams;

      // First 13 subordinates: 2.4GHz
      if (i < 13) {
        params.channel = i + 1;  // Channels 1-13
        params.band = BAND_2_4GHZ;
      }
      // Remaining subordinates: 5GHz
      else {
        params.channel = get5GHzChannel(i - 13);
        params.band = BAND_5GHZ;
      }

      Serial.printf("Sub %d: Band %s, Channel %d\n",
                    subordinates[i].address,
                    params.band == BAND_2_4GHZ ? "2.4GHz" : "5GHz",
                    params.channel);

      protocol.sendCommand(subordinates[i].address, CMD_SET_SCAN_PARAMS,
                          &params, sizeof(params));
      delay(20);
    }
  }
}
```

### Channel Distribution

- Subordinates 1-13: 2.4GHz channels 1-13
- Subordinates 14-48: 5GHz channels

### Best For
- Suburban areas
- General purpose wardriving
- Mixed device environments

---

## Example 3: High-Speed Scanning

Optimized for maximum scan rate while moving.

### Configuration

```cpp
globalScanParams = {
  .band = BAND_5GHZ,
  .channel = 0,
  .scanMode = SCAN_MODE_ACTIVE,
  .scanTimeMs = 80,      // Reduced scan time
  .intervalMs = 500,     // Faster interval
  .hidden = 0,           // Skip hidden networks
  .showHidden = 0
};
```

### Best For
- Highway wardriving
- Quick area surveys
- Time-sensitive data collection

---

## Example 4: Battery Optimized

Extended battery life for portable operation.

### Configuration

```cpp
globalScanParams = {
  .band = BAND_5GHZ,
  .channel = 0,
  .scanMode = SCAN_MODE_PASSIVE,  // Uses less power
  .scanTimeMs = 200,
  .intervalMs = 5000,              // 5 second interval
  .hidden = 0,
  .showHidden = 0
};

// Also reduce number of active subordinates
numSubordinates = 24;  // Only use half the devices
```

### Additional Power Savings

Add to subordinate.ino loop():

```cpp
// Put WiFi to sleep between scans
if (!scanningActive) {
  WiFi.setSleep(WIFI_PS_MAX_MODEM);
}
```

### Best For
- Battery-powered operation
- All-day monitoring
- Remote installations

---

## Example 5: Dense Coverage Mode

Maximum detail in high-density areas.

### Configuration

```cpp
globalScanParams = {
  .band = BAND_5GHZ,
  .channel = 0,
  .scanMode = SCAN_MODE_ACTIVE,
  .scanTimeMs = 300,     // Extended scan time
  .intervalMs = 2000,
  .hidden = 1,
  .showHidden = 1
};
```

### Multiple Passes

Modify configureSubordinates() to assign multiple subordinates per channel:

```cpp
// 2 subordinates per channel for redundancy
uint8_t channelIndex = i / 2;
params.channel = get5GHzChannel(channelIndex);
```

### Best For
- Security audits
- Detailed site surveys
- Competition detection
- Signal mapping

---

## Example 6: Stationary Monitoring

Fixed installation for continuous monitoring.

### Configuration

```cpp
globalScanParams = {
  .band = BAND_BOTH,     // Monitor both bands
  .channel = 0,
  .scanMode = SCAN_MODE_PASSIVE,  // Non-intrusive
  .scanTimeMs = 200,
  .intervalMs = 10000,   // Every 10 seconds
  .hidden = 1,
  .showHidden = 1
};
```

### Additional Features

Add timestamp and rotating log files:

```cpp
void createNewLogFile() {
  char filename[32];
  // Create new file every hour
  snprintf(filename, sizeof(filename), "/monitor_%02d%02d.csv",
           hour(), minute());
  logFile = SD.open(filename, FILE_WRITE);
  // ...
}
```

### Best For
- Security monitoring
- Network change detection
- Long-term studies
- Event monitoring

---

## Example 7: 2.4GHz Focus

Detailed 2.4GHz coverage for IoT device discovery.

### Configuration

```cpp
numSubordinates = 13;  // One per 2.4GHz channel

void configureSubordinates() {
  for (uint8_t i = 0; i < numSubordinates; i++) {
    if (subordinates[i].online) {
      ScanParams params = {
        .band = BAND_2_4GHZ,
        .channel = i + 1,  // Channels 1-13
        .scanMode = SCAN_MODE_ACTIVE,
        .scanTimeMs = 150,
        .intervalMs = 1000,
        .hidden = 1,
        .showHidden = 1
      };

      protocol.sendCommand(subordinates[i].address, CMD_SET_SCAN_PARAMS,
                          &params, sizeof(params));
    }
  }
}
```

### Best For
- IoT device surveys
- 2.4GHz interference analysis
- Legacy device detection
- Zigbee coexistence studies

---

## Customization Tips

### Adjusting Scan Timing

```cpp
// Very fast (for highways)
.scanTimeMs = 50,
.intervalMs = 250,

// Normal (city driving)
.scanTimeMs = 120,
.intervalMs = 1000,

// Thorough (walking/stationary)
.scanTimeMs = 300,
.intervalMs = 2000,

// Deep scan (site survey)
.scanTimeMs = 500,
.intervalMs = 5000,
```

### Filtering Results

Add to handleScanResult() in controller.ino:

```cpp
// Only log strong signals
if (result.rssi < -80) {
  return;  // Skip weak signals
}

// Only log specific SSID patterns
if (!strstr(result.ssid, "Target")) {
  return;  // Skip non-matching SSIDs
}

// Deduplication (within time window)
if (isDuplicate(result)) {
  return;  // Skip duplicates
}
```

### Channel Priorities

Focus on common channels:

```cpp
// Priority 5GHz channels (most common in US)
const uint8_t priorityChannels[] = {36, 40, 44, 48, 149, 153, 157, 161};

// Assign multiple subordinates to priority channels
uint8_t channelIndex = i % sizeof(priorityChannels);
params.channel = priorityChannels[channelIndex];
```

### Region-Specific Configuration

```cpp
// US/Canada
const uint8_t channels5GHz[] = {36, 40, 44, 48, 52, 56, 60, 64,
                                100, 104, 108, 112, 116, 120, 124, 128,
                                132, 136, 140, 144, 149, 153, 157, 161, 165};

// Europe
const uint8_t channels5GHz[] = {36, 40, 44, 48, 52, 56, 60, 64,
                                100, 104, 108, 112, 116, 120, 124, 128,
                                132, 136, 140};

// Japan (includes channel 14 for 2.4GHz)
const uint8_t channels24GHz[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
```

---

## Performance Tuning

### Memory Optimization

```cpp
// In subordinate.ino
#define MAX_RESULT_BUFFER 25  // Reduce from 50 if memory constrained

// Send results immediately instead of buffering
if (scanningActive) {
  // Send each result as it's found
  protocol.sendResponse(CONTROLLER_ADDRESS, RESP_SCAN_RESULT,
                       &result, sizeof(result));
}
```

### Serial Bus Optimization

```cpp
// Reduce delay between commands
delay(5);  // Instead of delay(20)

// Batch status requests
if (millis() - lastStatusCheck > 60000) {
  // Only check status every minute
}

// Use broadcast for common commands
protocol.sendCommand(0xFF, CMD_START_SCAN);  // Broadcast
```

### SD Card Performance

```cpp
// Batch writes
#define WRITE_BUFFER_SIZE 10
WiFiScanResult writeBuffer[WRITE_BUFFER_SIZE];
uint8_t bufferIndex = 0;

// Write when buffer is full
if (bufferIndex >= WRITE_BUFFER_SIZE) {
  for (uint8_t i = 0; i < bufferIndex; i++) {
    logScanResult(0, writeBuffer[i]);
  }
  logFile.flush();
  bufferIndex = 0;
}
```

---

## Testing Configurations

### Bench Test Setup

```cpp
// Minimal configuration for testing
numSubordinates = 3;  // Test with just 3 devices

globalScanParams = {
  .band = BAND_2_4GHZ,
  .channel = 6,  // Fixed channel for testing
  .scanMode = SCAN_MODE_ACTIVE,
  .scanTimeMs = 100,
  .intervalMs = 2000,
  .hidden = 0,
  .showHidden = 1
};
```

### Debug Mode

Add to controller.ino:

```cpp
#define DEBUG_MODE 1

#if DEBUG_MODE
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif
```

---

## Troubleshooting Common Issues

### No Networks Found

Try this configuration:

```cpp
globalScanParams = {
  .band = BAND_2_4GHZ,     // Start with 2.4GHz (more common)
  .channel = 0,            // Scan all channels
  .scanMode = SCAN_MODE_ACTIVE,
  .scanTimeMs = 300,       // Longer scan time
  .intervalMs = 1000,
  .hidden = 1,
  .showHidden = 1
};
```

### Too Many Duplicates

Add deduplication logic:

```cpp
struct NetworkEntry {
  uint8_t bssid[6];
  unsigned long lastSeen;
};

#define DUPLICATE_WINDOW_MS 10000  // 10 seconds
NetworkEntry seenNetworks[100];

bool isDuplicate(WiFiScanResult& result) {
  unsigned long now = millis();
  for (int i = 0; i < 100; i++) {
    if (memcmp(seenNetworks[i].bssid, result.bssid, 6) == 0) {
      if (now - seenNetworks[i].lastSeen < DUPLICATE_WINDOW_MS) {
        return true;  // Seen recently
      }
      seenNetworks[i].lastSeen = now;
      return false;
    }
  }
  return false;
}
```

---

## Advanced Examples

### GPS Integration (Future)

```cpp
// Add to scan result
struct WiFiScanResultGPS {
  WiFiScanResult scan;
  float latitude;
  float longitude;
  float altitude;
};

// Log with GPS coordinates
logFile.printf("%lu,%d,%02X:%02X:%02X:%02X:%02X:%02X,\"%s\",%d,%d,%d,%d,%.6f,%.6f,%.1f\n",
               result.timestamp, subAddr,
               result.bssid[0], result.bssid[1], result.bssid[2],
               result.bssid[3], result.bssid[4], result.bssid[5],
               result.ssid, result.rssi, result.channel, result.band,
               result.authMode, gps.latitude, gps.longitude, gps.altitude);
```

### Web Interface (Future)

```cpp
// Add WiFi AP mode to controller
WiFi.softAP("WiFivedra", "password");
WebServer server(80);

server.on("/status", []() {
  String json = "{";
  json += "\"online\":" + String(onlineCount) + ",";
  json += "\"total\":" + String(totalScansReceived) + ",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";
  server.send(200, "application/json", json);
});
```
