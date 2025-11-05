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

### Daisy Chain Topology

The system uses a **direct UART daisy chain** architecture. Each ESP32 is connected directly to its immediate neighbors (left and right), forming a chain from the controller to the last subordinate.

**Benefits of this design:**
- **Lower cost**: No RS-485 transceivers needed
- **Simple wiring**: Just TX/RX connections between neighbors
- **Bidirectional**: Messages flow down the chain, responses flow back up
- **Automatic forwarding**: Each subordinate automatically forwards messages not addressed to it

```
┌────────────┐       ┌─────────┐       ┌─────────┐             ┌─────────┐
│ Controller │◄─────►│  Sub 1  │◄─────►│  Sub 2  │◄───...───►│  Sub 48 │
│   ESP32    │       │ ESP32-C5│       │ ESP32-C5│             │ ESP32-C5│
└────────────┘       └─────────┘       └─────────┘             └─────────┘
  Downstream          Up | Down         Up | Down                Upstream
  connection          |     |           |     |                  connection
  only                |     |           |     |                  only
```

**How it works:**
1. **Controller → Subordinates**: Commands flow downstream (left to right)
   - Controller sends to Sub1
   - Sub1 processes if addressed to it, or forwards downstream to Sub2
   - This continues until message reaches destination

2. **Subordinates → Controller**: Responses flow upstream (right to left)
   - Subordinate sends response upstream toward controller
   - Each device forwards responses not addressed to it
   - Response eventually reaches controller

3. **No unnecessary forwarding**: Once a message reaches its destination, it stops there

### Wiring Diagram

**Controller to Sub1:**
```
Controller ESP32          Subordinate 1 ESP32-C5
┌────────────┐           ┌──────────────┐
│ GPIO 17 TX │──────────►│ GPIO 20 RX   │
│ GPIO 16 RX │◄──────────│ GPIO 21 TX   │
│ GND        │◄─────────►│ GND          │
└────────────┘           └──────────────┘
```

**Sub N to Sub N+1 (all middle subordinates):**
```
Subordinate N ESP32-C5    Subordinate N+1 ESP32-C5
┌──────────────┐         ┌──────────────┐
│ GPIO 17 TX   │────────►│ GPIO 20 RX   │  (Downstream)
│ GPIO 16 RX   │◄────────│ GPIO 21 TX   │  (Downstream)
│ GND          │◄───────►│ GND          │
└──────────────┘         └──────────────┘
```

**Important wiring notes:**
- Each subordinate has TWO UART connections (except the last one):
  - **Upstream** (GPIO 21 TX, GPIO 20 RX): Connects to previous device
  - **Downstream** (GPIO 17 TX, GPIO 16 RX): Connects to next device
- The **last subordinate** only has an upstream connection
- TX on one device connects to RX on the other
- **All devices must share a common ground**

## Pin Assignments

### Controller (ESP32)

```cpp
// Downstream Serial (to Sub1)
Serial1 TX  -> GPIO 17 (to Sub1's RX pin 20)
Serial1 RX  -> GPIO 16 (from Sub1's TX pin 21)

// SD Card (SPI)
MOSI -> GPIO 23
MISO -> GPIO 19
SCK  -> GPIO 18
CS   -> GPIO 5

// Indicators
LED  -> GPIO 2

// Debug/USB
Serial (USB) -> For debug output and commands
```

### Subordinate (ESP32-C5)

```cpp
// Upstream Serial (to previous device / toward controller)
Serial1 TX  -> GPIO 21 (to previous device's RX)
Serial1 RX  -> GPIO 20 (from previous device's TX)

// Downstream Serial (to next device / away from controller)
Serial2 TX  -> GPIO 17 (to next device's RX pin 20)
Serial2 RX  -> GPIO 16 (from next device's TX pin 21)

// Indicators
LED  -> GPIO 2

// Notes:
// - The LAST subordinate (#48) does not use downstream pins
// - Set IS_LAST_NODE = true for the last device
```

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

### 4. Flash Subordinate Firmware

**GREAT NEWS:** All subordinates can use identical firmware! The system supports up to 52 subordinates.

The subordinate firmware includes **auto-discovery**:
- All devices boot with an unassigned address
- The controller automatically assigns addresses (1, 2, 3, ...)  via the daisy chain
- The last device in the chain automatically detects it's the last node
- No manual configuration needed!

**Steps:**
1. Open `subordinate/subordinate.ino` in Arduino IDE
2. Select Tools → Board → "ESP32-C5 Dev Module"
3. Select Tools → Port → (your COM port)
4. Click Upload
5. **Repeat for ALL subordinates** - the firmware is identical!

**LED Indicators during startup:**
- **Very fast blink (100ms)**: Waiting for address assignment
- **Slow blink (1s)**: Address assigned, idle
- **Medium blink (200ms)**: Scanning active

**Physical arrangement:**
- The subordinates will be numbered in order from controller: Sub1, Sub2, Sub3, etc.
- The physical order in the daisy chain determines the address
- Label devices AFTER auto-discovery completes

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

### 1. Test Controller (Without Subordinates)

1. Connect controller to USB
2. Open Serial Monitor (115200 baud)
3. You should see:
   ```
   === WiFivedra Controller ===
   Daisy Chain Mode
   Initializing...
   Serial protocol initialized (daisy chain)
   SD card initialized

   --- Auto-Discovering Subordinates ---
   Assigning addresses via daisy chain...
   ```
4. After 10 seconds (no subordinates connected):
   ```
   --- Auto-Discovery Complete ---
   Total subordinates discovered: 0
   ```
5. Type `help` to see available commands

### 2. Test Single Subordinate

1. **Power off the controller**
2. Wire the first subordinate to the controller (Controller GPIO17 TX → Sub GPIO20 RX, Controller GPIO16 RX ← Sub GPIO21 TX, GND ← → GND)
3. **Power on both devices**
4. Watch the controller serial monitor:
   ```
   --- Auto-Discovering Subordinates ---
   Assigning addresses via daisy chain...
   Subordinate #1 registered (LAST NODE)

   --- Auto-Discovery Complete ---
   Total subordinates discovered: 1
   Last subordinate in chain: #1
   ```
5. The subordinate's LED should change from very fast blink to slow blink

### 3. Test Daisy Chain

1. **Power off all devices**
2. Wire the second subordinate between Sub1 and ground (Sub1 GPIO17 TX → Sub2 GPIO20 RX, Sub1 GPIO16 RX ← Sub2 GPIO21 TX, GND)
3. **Power on all devices**
4. Watch auto-discovery:
   ```
   Subordinate #1 registered
   Subordinate #2 registered (LAST NODE)

   --- Auto-Discovery Complete ---
   Total subordinates discovered: 2
   Last subordinate in chain: #2
   ```
5. Both subordinates should now have slow-blinking LEDs

### 4. Test Scanning

1. Type: `config` to configure subordinates
2. Type: `start` to begin scanning
3. You should see WiFi networks being detected:
   ```
   [Sub01 Ch036] AA:BB:CC:DD:EE:FF | MyNetwork | RSSI: -45 dBm
   [Sub02 Ch040] BB:CC:DD:EE:FF:00 | AnotherNet | RSSI: -52 dBm
   ```

### 5. Add More Subordinates

1. **Power off all devices**
2. Wire additional subordinates in the chain
3. **Power on and watch auto-discovery**
4. Each subordinate will be assigned the next sequential address
5. The last physical device in the chain will be detected as the last node

## Channel Assignment Strategy

The system automatically assigns channels based on subordinate ID:

### For 5GHz Coverage (52 subordinates)

There are 25 unique 5GHz channels available. With 52 subordinates:
- Subs 1-25: First pass through all 5GHz channels
- Subs 26-50: Second pass through all 5GHz channels (double coverage)
- Subs 51-52: Third pass starts (channels 36, 40)

This provides **double coverage** of all 5GHz channels for better detection accuracy.

### For Mixed Band Coverage

Modify `controller.ino` to split subordinates:
- Subs 1-13: 2.4GHz channels 1-13
- Subs 14-52: 5GHz channels (cycling through all 25 channels)

## Troubleshooting

### Auto-Discovery Issues

**No subordinates discovered:**
1. Check that all devices are powered on
2. Verify daisy chain wiring (TX to RX, RX to TX)
3. Ensure all devices share common ground
4. Check subordinate LEDs - should blink very fast (100ms) when waiting for address
5. Try with just one subordinate first
6. Power cycle all devices (auto-discovery runs on startup)

**Some subordinates not discovered:**
1. Check wiring of the last discovered device and the next one
2. Verify power to all devices in the chain
3. The problem is likely between the last discovered device and the next
4. Try running `autodiscover` command again from controller

**Wrong number of subordinates:**
1. One or more subordinates may not have firmware uploaded
2. Check that all subordinates are powered
3. Verify serial connections throughout the chain

### Garbled Serial Data

1. Check for loose connections
2. Verify TX/RX are not swapped
3. Ensure common ground connection
4. Check for voltage level issues (all should be 3.3V logic)
5. Reduce baud rate to 57600 if using long cables (>2m between devices)

### Only First Few Subordinates Respond

1. Check downstream connections from last responding device
2. Verify message forwarding is working (check serial output)
3. Check power to devices further down the chain
4. Ensure cable quality is good (signal degrades over long distances)
5. Consider adding signal buffers if chain is very long (>10 devices)

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
