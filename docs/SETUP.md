# WiFivedra Setup Guide

## Hardware Requirements

### Controller
- 1x ESP32 development board (any variant)
- MicroSD card module (for data logging)
- MicroSD card (8GB or larger recommended)
- USB cable for programming and power

### Subordinates
- 48x ESP32-C5 development boards
- Serial bus wiring (see connection diagram)
- Power distribution system

## Connection Diagram

### UART Daisy Chain Topology

The system uses a UART daisy chain architecture where subordinates are connected in series:

```
┌──────────────┐
│  Controller  │
│    ESP32     │
└──┬────────┬──┘
   │ TX     │ RX
   │        │
   │        └──────────────────────────────────────────────────┐
   │                                                            │
   └────────────┐                                              │
                │                                              │
           ┌────▼────┐                                         │
           │  Sub 1  │                                         │
           │ ESP32-C5│                                         │
           └──┬────┬─┘                                         │
              │ TX │ RX                                        │
              │    │                                           │
              │    └─────────────────────────────┐             │
              └────────────┐                     │             │
                           │                     │             │
                      ┌────▼────┐                │             │
                      │  Sub 2  │                │             │
                      │ ESP32-C5│                │             │
                      └──┬────┬─┘                │             │
                         │ TX │ RX               │             │
                         │    │                  │             │
                        ...  ...                 │             │
                         │    │                  │             │
                      ┌──▼────┴──┐               │             │
                      │  Sub 48  │               │             │
                      │ ESP32-C5 ├───────────────┘             │
                      └──────────┘ TX ───────────────────────┘
                           (RX unused)
```

**Wiring:**
- Controller TX -> Sub1 RX
- Sub1 TX -> Sub2 RX
- Sub2 TX -> Sub3 RX
- ...
- Sub48 TX -> Controller RX
- Common GND between all devices

**Key Points:**
- Each subordinate receives on RX and forwards on TX
- Last subordinate (Sub48) TX connects back to Controller RX
- Simple 3-wire connection: TX, RX, GND
- No additional components needed

## Pin Assignments

### Controller (ESP32)

```cpp
// Serial Communication
Serial1 TX  -> GPIO 17 (to Sub1 RX)
Serial1 RX  -> GPIO 16 (from Sub48 TX)

// SD Card (SPI)
MOSI -> GPIO 23
MISO -> GPIO 19
SCK  -> GPIO 18
CS   -> GPIO 5

// Indicators
LED  -> GPIO 2
```

### Subordinate (ESP32-C5)

```cpp
// Serial Communication - Daisy Chain
Serial RX  -> GPIO 20 (from previous device TX)
Serial1 TX -> GPIO 21 (to next device RX)

// Indicators
LED  -> GPIO 2
```

Note: Subordinates use Serial (default UART) for receiving and Serial1 for forwarding to maintain independent buffers.

## Software Setup

### 1. Install Arduino IDE

Download and install Arduino IDE from https://www.arduino.cc/

### 2. Install ESP32 Board Support

1. Open Arduino IDE
2. Go to File → Preferences
3. Add to "Additional Board Manager URLs":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. Go to Tools → Board → Board Manager
5. Search for "esp32" and install "esp32 by Espressif Systems"

### 3. Install Required Libraries

No external libraries required! The project uses only built-in ESP32 libraries:
- WiFi.h (built-in)
- SD.h (built-in)
- SPI.h (built-in)

### 4. Configure Subordinate Addresses

**IMPORTANT:** Each subordinate must have a unique address (1-48)

Edit `subordinate/subordinate.ino` line 12:
```cpp
#define MY_ADDRESS 1  // Change this for each device!
```

You'll need to:
1. Set `MY_ADDRESS` to 1
2. Upload to first subordinate
3. Set `MY_ADDRESS` to 2
4. Upload to second subordinate
5. Repeat for all 48 devices

**TIP:** Label each subordinate physically with its address!

### 5. Upload Controller Firmware

1. Open `controller/controller.ino` in Arduino IDE
2. Select Tools → Board → "ESP32 Dev Module" (or your specific board)
3. Select Tools → Port → (your COM port)
4. Click Upload

### 6. Upload Subordinate Firmware

1. Open `subordinate/subordinate.ino` in Arduino IDE
2. **Set the MY_ADDRESS define to the correct value**
3. Select Tools → Board → "ESP32-C5 Dev Module"
4. Select Tools → Port → (your COM port)
5. Click Upload
6. Repeat for each subordinate with different addresses

## Power Considerations

### Power Budget

Each ESP32-C5 consumes approximately:
- Idle: 80mA @ 3.3V
- WiFi scanning: 200mA @ 3.3V (peak)

Total system power (all 48 subordinates scanning):
- Peak: 48 × 200mA = 9.6A @ 3.3V = ~32W
- Typical: 48 × 100mA = 4.8A @ 3.3V = ~16W

### Power Supply Options

1. **Multiple 5V Power Supplies**
   - Use several USB power supplies
   - Group subordinates into banks of 8-10 devices
   - Each bank gets its own supply

2. **Single High-Current Supply**
   - Use a 5V 15A+ power supply
   - Distribute to all devices via power bus
   - Add bulk capacitors near each device

3. **Battery Operation**
   - LiPo battery: 3S (11.1V) 5000mAh minimum
   - Step-down converters to 5V
   - Runtime: ~1-2 hours under full scan

**Important:** All devices must share a common ground!

## Initial Testing

### 1. Test Controller

1. Connect controller to USB
2. Open Serial Monitor (115200 baud)
3. You should see:
   ```
   === WiFivedra Controller ===
   Initializing...
   Serial protocol initialized
   SD card initialized
   ```
4. Type `help` to see available commands

### 2. Test Single Subordinate

1. Connect one subordinate to the serial bus
2. Set its address to 1
3. On controller serial monitor, type: `discover`
4. You should see:
   ```
   --- Discovering Subordinates ---
   Sub 1: ONLINE
   Total: 1/48 online
   ```

### 3. Test Scanning

1. Type: `config` to configure the subordinate
2. Type: `start` to begin scanning
3. You should see WiFi networks being detected:
   ```
   [Sub01 Ch036] AA:BB:CC:DD:EE:FF | MyNetwork | RSSI: -45 dBm
   ```

### 4. Add More Subordinates

1. Connect additional subordinates one at a time
2. Verify each comes online with `discover`
3. Once all are connected, start scanning with `start`

## Channel Assignment Strategy

The system automatically assigns channels based on subordinate ID:

### For 5GHz Coverage (48 subordinates)

- Subs 1-25: Assigned to individual 5GHz channels
- Subs 26-48: Can be configured for additional coverage or 2.4GHz

### For Mixed Band Coverage

Modify `controller.ino` to split subordinates:
- Subs 1-13: 2.4GHz channels 1-13
- Subs 14-48: 5GHz channels

## Troubleshooting

### No Subordinates Found

1. Check serial bus wiring
2. Verify all devices share common ground
3. Check baud rate matches (115200)
4. Ensure subordinate addresses are set correctly
5. Try testing one subordinate at a time

### Garbled Serial Data

1. Check for loose connections
2. Verify TX/RX are not swapped
3. Reduce baud rate to 57600 if using long cables
4. Check for common ground between all devices
5. Keep wire runs short and away from power cables

### WiFi Scan Failures

1. Ensure antenna is connected to ESP32-C5
2. Check WiFi region settings
3. Verify ESP32-C5 supports 5GHz (most do)
4. Some channels may be restricted by region

### SD Card Errors

1. Format SD card as FAT32
2. Check SPI wiring
3. Try different SD card
4. Ensure SD card is not write-protected

### High Memory Usage

1. Reduce MAX_RESULT_BUFFER in subordinate.ino
2. Increase scan interval
3. Send results more frequently

## Performance Optimization

### For Maximum Coverage

- Set scan interval to 500ms
- Use active scanning
- Enable hidden network detection

### For Battery Life

- Set scan interval to 5000ms
- Use passive scanning
- Reduce scan time per channel

### For Accuracy

- Increase scan time per channel to 300ms
- Scan each channel multiple times
- Average RSSI values

## Next Steps

After basic setup:

1. Mount devices in enclosure
2. Add GPS module for location tagging (future enhancement)
3. Configure for mobile wardriving setup
4. Add external antennas for better range
5. Implement data visualization tools

## Safety Notes

- Do not operate while driving - have a passenger control it
- Follow local regulations regarding WiFi monitoring
- Ensure proper ventilation for enclosed setups
- Monitor temperature during extended operation
- Keep firmware updated
