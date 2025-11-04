# The_WiFivedra

New repository for the Collab between Lozaning (https://x.com/lozaning) and Rudi (https://x.com/eMbeddedHome)

The WiFivedra is the second generation of Wifydra's and includes (38) ESP32-C5 for full 38 channel 2.4Ghz and 5Ghz wifi monitoring.

---

## 🔗 ESP32 Daisy Chain Communication Bus

A robust daisy-chained UART communication bus system for ESP32 devices. This allows a single controller ESP32 to communicate with multiple sub-devices connected in a chain, where messages are automatically forwarded down the line until they reach their destination.

### 🎯 Features

- **Addressed Messaging**: Send messages to specific devices (1-254) or broadcast to all
- **Automatic Forwarding**: Sub-devices automatically forward messages not meant for them
- **Checksum Validation**: Built-in error detection with XOR checksums
- **Flexible Protocol**: Easy to extend with custom commands
- **Bidirectional**: Sub-devices can send responses back to the controller
- **Scalable**: Support for up to 254 devices in a single chain

### 📋 Protocol Specification

#### Message Format

```
[START_BYTE][ADDRESS][LENGTH][COMMAND][DATA...][CHECKSUM]
```

| Field | Size | Description |
|-------|------|-------------|
| START_BYTE | 1 byte | Always `0xAA` - marks message start |
| ADDRESS | 1 byte | Target device (0=broadcast, 1-254=device, 255=reserved) |
| LENGTH | 1 byte | Number of bytes in COMMAND + DATA |
| COMMAND | 1 byte | Command identifier |
| DATA | 0-252 bytes | Optional command data |
| CHECKSUM | 1 byte | XOR of ADDRESS, LENGTH, COMMAND, and DATA |

#### Built-in Commands

| Command | Value | Description |
|---------|-------|-------------|
| CMD_PING | 0x01 | Ping a device (visual LED feedback) |
| CMD_SET_LED | 0x02 | Set LED state (data: 0=off, 1=on) |
| CMD_GET_STATUS | 0x03 | Request device status |
| CMD_CUSTOM | 0x10+ | Custom commands (extend as needed) |

### 🔌 Hardware Setup

#### Wiring Diagram

```
Controller ESP32          Sub ESP32 #1           Sub ESP32 #2           Sub ESP32 #3
┌─────────────┐          ┌─────────────┐        ┌─────────────┐        ┌─────────────┐
│             │          │             │        │             │        │             │
│ TX1 (17)────┼─────────→│ RX1 (16)    │        │             │        │             │
│             │          │             │        │             │        │             │
│ RX1 (16)←───┼──────────│ TX1 (17)    │        │             │        │             │
│             │          │             │        │             │        │             │
│             │          │ TX2 (19)────┼───────→│ RX1 (16)    │        │             │
│             │          │             │        │             │        │             │
│             │          │ RX2 (18)←───┼────────│ TX1 (17)    │        │             │
│             │          │             │        │             │        │             │
│             │          │             │        │ TX2 (19)────┼───────→│ RX1 (16)    │
│             │          │             │        │             │        │             │
│             │          │             │        │ RX2 (18)←───┼────────│ TX1 (17)    │
│             │          │             │        │             │        │             │
└─────────────┘          └─────────────┘        └─────────────┘        └─────────────┘
```

#### Pin Configuration

**Controller ESP32:**
- TX: GPIO 17 (Serial1 TX)
- RX: GPIO 16 (Serial1 RX)

**First Sub ESP32:**
- RX_IN: GPIO 16 (Serial1 RX) - from Controller
- TX_IN: GPIO 17 (Serial1 TX) - to Controller (for responses)
- TX_OUT: GPIO 19 (Serial2 TX) - to next Sub
- RX_OUT: GPIO 18 (Serial2 RX) - from next Sub

**Subsequent Sub ESP32s:**
- Same pin configuration as first sub
- Each sub connects to the next in the chain

**Common Connections:**
- All devices must share a common GND
- Power each ESP32 appropriately (3.3V or 5V depending on your board)

### 🚀 Quick Start

#### 1. Upload Controller Code

1. Open `controller_esp32/controller_esp32.ino` in Arduino IDE
2. Select your ESP32 board
3. Upload to your controller ESP32
4. Open Serial Monitor at 115200 baud

#### 2. Upload Sub Code

1. Open `sub_esp32/sub_esp32.ino` in Arduino IDE
2. **IMPORTANT**: Change `MY_ADDRESS` to a unique value for each sub:
   ```cpp
   #define MY_ADDRESS 1  // Change to 2, 3, 4, etc. for each sub
   ```
3. Verify pin assignments match your wiring
4. Upload to each sub ESP32
5. (Optional) Open Serial Monitor to see message processing

#### 3. Test the System

The controller will automatically ping devices 1-5 every few seconds. Watch the Serial Monitor to see:
- Controller: Messages being sent and responses received
- Subs: Messages being received, processed, or forwarded

### 💡 Usage Examples

#### Example 1: Ping a Specific Device

```cpp
// Ping device at address 3
sendCommand(3, CMD_PING, NULL, 0);

// Wait for response
if (waitForResponse(3, 500)) {
  Serial.println("Device 3 is alive!");
}
```

#### Example 2: Control an LED

```cpp
// Turn on LED on device 2
uint8_t data = 1;
sendCommand(2, CMD_SET_LED, &data, 1);

// Turn off LED on device 2
data = 0;
sendCommand(2, CMD_SET_LED, &data, 1);
```

#### Example 3: Broadcast to All Devices

```cpp
// Turn on all LEDs
uint8_t data = 1;
sendBroadcast(CMD_SET_LED, &data, 1);
```

#### Example 4: Send Custom Data

```cpp
// Send custom command with data
uint8_t customData[] = {0x12, 0x34, 0x56, 0x78};
sendCommand(5, CMD_CUSTOM, customData, 4);
```

### 🔧 Configuration

#### Controller Configuration

In `controller_esp32.ino`:

```cpp
// Change UART pins
#define TX_PIN 17
#define RX_PIN 16

// Adjust response timeout
#define RESPONSE_TIMEOUT 1000  // ms
```

#### Sub Configuration

In `sub_esp32.ino`:

```cpp
// Set unique address for each device
#define MY_ADDRESS 1  // 1-254

// Change UART pins
#define RX_IN_PIN 16
#define TX_IN_PIN 17
#define RX_OUT_PIN 18
#define TX_OUT_PIN 19

// Change LED pin
#define LED_PIN 2
```

### 📊 How It Works

#### Message Flow Example

Let's say the controller sends a message to device #3:

1. **Controller** sends: `[0xAA][0x03][0x01][0x01][0x03]`
   - Address: 3
   - Command: PING (0x01)

2. **Sub #1** (address 1):
   - Receives message
   - Checks address: not for me (3 ≠ 1)
   - Forwards to Serial2 → Sub #2

3. **Sub #2** (address 2):
   - Receives message
   - Checks address: not for me (3 ≠ 2)
   - Forwards to Serial2 → Sub #3

4. **Sub #3** (address 3):
   - Receives message
   - Checks address: **FOR ME!** (3 = 3)
   - Processes command (blinks LED)
   - Sends response back upstream

5. **Response travels back**:
   - Sub #3 → Sub #2 → Sub #1 → Controller

#### State Machine

Each sub device uses a state machine to parse incoming bytes:

```
State 0: Wait for START_BYTE (0xAA)
State 1: Read ADDRESS
State 2: Read LENGTH
State 3: Read COMMAND
State 4: Read DATA (if length > 1)
State 5: Read and verify CHECKSUM
```

### 🛠️ Troubleshooting

#### No Response from Subs

1. **Check wiring**: Ensure TX connects to RX and vice versa
2. **Common ground**: All devices must share GND
3. **Baud rate**: Verify all devices use same baud rate (115200)
4. **Power**: Ensure all ESP32s are properly powered
5. **Address**: Verify each sub has a unique MY_ADDRESS

#### Messages Not Forwarding

1. **Check Serial2 initialization** on subs
2. **Verify pin assignments** for RX_OUT and TX_OUT
3. **Check Serial Monitor** on subs to see if messages are received

#### Checksum Errors

1. **Cable length**: Keep UART connections under 1 meter for reliability
2. **Noise**: Use shielded cables or twisted pairs
3. **Baud rate**: Lower baud rate for longer cables (9600 or 57600)

### 📝 Best Practices

1. **Keep chains short**: Recommended maximum 10-15 devices
2. **Use proper voltage levels**: ESP32 uses 3.3V logic
3. **Add delay between messages**: 100ms minimum
4. **Implement timeouts**: Don't wait forever for responses
5. **Use level shifters**: If mixing 3.3V and 5V devices
6. **Consider cable quality**: Use proper gauge wire for power
7. **Debug one device at a time**: Test each sub individually first

### 🎓 Advanced Topics

#### Adding Encryption

For secure communications, implement encryption in the message payload.

#### Implementing Error Recovery

Add message retry logic to handle unreliable connections.

#### Network Discovery

Implement auto-discovery of devices on the bus by pinging all addresses.

---

**Happy Daisy Chaining! 🌼** 
