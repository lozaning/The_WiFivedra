# WiFivedra Daisy Chain Protocol Specification

## Overview

The WiFivedra Daisy Chain Protocol is a simple, robust UART-based communication protocol designed for ESP32 devices connected in a daisy chain configuration. It supports addressed messaging, broadcasting, and automatic message forwarding.

## Protocol Version

Current Version: 1.0

## Physical Layer

- **Transport**: UART (Serial)
- **Baud Rate**: 115200 (configurable, must match across all devices)
- **Data Bits**: 8
- **Parity**: None
- **Stop Bits**: 1
- **Flow Control**: None
- **Voltage Level**: 3.3V (ESP32 standard)

## Frame Format

Every message follows this structure:

```
┌───────────┬─────────┬────────┬─────────┬──────────┬──────────┐
│START_BYTE │ ADDRESS │ LENGTH │ COMMAND │   DATA   │ CHECKSUM │
│  (0xAA)   │ (1 byte)│(1 byte)│ (1 byte)│ (N bytes)│ (1 byte) │
└───────────┴─────────┴────────┴─────────┴──────────┴──────────┘
     Fixed     Target    Payload   First    Optional   Error
               Device     Size     Payload    Payload   Check
```

### Field Descriptions

#### 1. START_BYTE (1 byte)
- **Value**: `0xAA` (fixed)
- **Purpose**: Frame synchronization
- **Notes**: Not included in checksum calculation

#### 2. ADDRESS (1 byte)
- **Range**: 0-255
  - `0x00`: Broadcast (all devices process)
  - `0x01-0xFE`: Individual device address
  - `0xFF`: Reserved for future use
- **Purpose**: Identify target device
- **Notes**: Included in checksum

#### 3. LENGTH (1 byte)
- **Range**: 1-255
- **Value**: Number of bytes in COMMAND + DATA fields
- **Purpose**: Define payload size
- **Notes**: Included in checksum

#### 4. COMMAND (1 byte)
- **Range**: 0-255
- **Purpose**: Specify action to be performed
- **Notes**: First byte of payload, included in checksum

#### 5. DATA (0-254 bytes)
- **Range**: Variable length
- **Purpose**: Command parameters or payload
- **Notes**: All bytes included in checksum

#### 6. CHECKSUM (1 byte)
- **Algorithm**: XOR of ADDRESS, LENGTH, COMMAND, and all DATA bytes
- **Purpose**: Error detection
- **Calculation**: `CHECKSUM = ADDRESS ⊕ LENGTH ⊕ COMMAND ⊕ DATA[0] ⊕ DATA[1] ⊕ ... ⊕ DATA[N-1]`

## Checksum Algorithm

```cpp
uint8_t calculateChecksum(uint8_t address, uint8_t length, uint8_t* payload) {
  uint8_t checksum = address ^ length;
  for (uint8_t i = 0; i < length; i++) {
    checksum ^= payload[i];
  }
  return checksum;
}
```

## Standard Commands

### Command Set

| Command | Value | Description | Data Bytes | Response |
|---------|-------|-------------|------------|----------|
| CMD_PING | 0x01 | Ping device | 0 | ACK |
| CMD_SET_LED | 0x02 | Control LED | 1 (0=off, 1=on) | None |
| CMD_GET_STATUS | 0x03 | Request status | 0 | Status data |
| CMD_CUSTOM | 0x10-0xFF | User-defined | Variable | Variable |

### Command Details

#### CMD_PING (0x01)
- **Purpose**: Test device presence
- **Data**: None
- **Response**: ACK frame
- **Behavior**: Device blinks LED briefly

**Frame Example**:
```
[0xAA][0x03][0x01][0x01][0x03]
       addr  len   cmd   cksum
```

#### CMD_SET_LED (0x02)
- **Purpose**: Control device LED
- **Data**: 1 byte (0=off, 1=on)
- **Response**: None
- **Behavior**: Sets LED state

**Frame Example** (turn on LED on device 5):
```
[0xAA][0x05][0x02][0x02][0x01][0x04]
       addr  len   cmd   data  cksum
```

#### CMD_GET_STATUS (0x03)
- **Purpose**: Request device status
- **Data**: None
- **Response**: Status frame with device-specific data
- **Behavior**: Device sends status information

## Response Format

Responses follow the same frame format, with ADDRESS field containing the responding device's address.

**Response Example** (ACK from device 3):
```
[0xAA][0x03][0x01][0xAC][0xAE]
       addr  len   ack   cksum
```

## Message Routing

### Controller to Sub
1. Controller sends message to Serial1 TX
2. First sub receives on Serial1 RX
3. Sub checks ADDRESS field:
   - If match or broadcast → process command
   - If match (not broadcast) → send response, stop forwarding
   - If no match → forward to Serial2 TX
4. Next sub receives on Serial1 RX
5. Process repeats down the chain

### Sub to Controller (Response)
1. Sub sends response to Serial1 TX (back upstream)
2. Previous device receives on Serial1 RX
3. Device forwards response upstream on Serial1 TX
4. Response travels back to controller

### Broadcast Behavior
- All devices process the command
- No responses sent (avoid bus collision)
- Message forwarded to next device in chain

## State Machine

Each device implements a receive state machine:

```
State 0: WAIT_START
  ├─ Receive START_BYTE (0xAA) → State 1
  └─ Receive other → State 0

State 1: READ_ADDRESS
  ├─ Store ADDRESS
  └─ → State 2

State 2: READ_LENGTH
  ├─ Store LENGTH
  └─ → State 3

State 3: READ_COMMAND
  ├─ Store COMMAND
  ├─ If LENGTH == 1 → State 5
  └─ Else → State 4

State 4: READ_DATA
  ├─ Store DATA bytes
  ├─ If all bytes read → State 5
  └─ Else → Continue State 4

State 5: READ_CHECKSUM
  ├─ Calculate expected checksum
  ├─ If match → Process message
  ├─ Else → Discard message
  └─ → State 0
```

## Error Handling

### Checksum Mismatch
- **Action**: Discard message
- **Logging**: Log error if debug enabled
- **Recovery**: Wait for next valid frame

### Timeout
- **Controller**: Implement response timeout (default 1000ms)
- **Action**: Consider message lost, optionally retry
- **Max Retries**: 3 recommended

### Buffer Overflow
- **Prevention**: Limit LENGTH to maximum buffer size
- **Action**: Reset state machine if overflow detected

## Timing Considerations

### Inter-Message Delay
- **Minimum**: 10ms recommended
- **Controller Loop**: 100ms between commands recommended

### Response Timeout
- **Default**: 1000ms
- **Per-hop Latency**: ~5-10ms
- **Formula**: `timeout = base_timeout + (num_hops * hop_latency)`

### UART Transmission Time
For 115200 baud:
- Bit time: ~8.68 μs
- Byte time: ~86.8 μs (10 bits/byte with start/stop)
- 10-byte message: ~868 μs

## Best Practices

### Addressing
1. Reserve address 0 for broadcasts
2. Start device addresses from 1
3. Use sequential addresses for easier management
4. Document address assignments

### Reliability
1. Implement retry logic for critical commands
2. Use shorter messages for better reliability
3. Add delays between consecutive messages
4. Monitor checksum errors for cable issues

### Performance
1. Keep chain length under 15 devices
2. Use appropriate baud rate for cable length
3. Consider bus arbitration for complex networks
4. Implement message queuing on controller

### Security
1. Implement authentication if needed
2. Encrypt sensitive DATA payloads
3. Add sequence numbers to detect replay attacks
4. Validate command ranges

## Extensions

### Custom Commands
- Use command values 0x10-0xFF for custom functionality
- Document custom commands in application code
- Maintain backward compatibility when updating

### Protocol Versioning
- Include version in CMD_GET_STATUS response
- Implement graceful degradation for version mismatches

### Multi-Master Support
- Requires bus arbitration mechanism
- Consider CSMA/CD-like collision detection
- Add device priority levels

## Limitations

1. **Single Master**: Current design assumes one controller
2. **No Acknowledgment**: Commands other than PING don't require ACK
3. **Limited Error Recovery**: No automatic retransmission
4. **No Flow Control**: Sender must pace messages
5. **Collision Possible**: Multiple simultaneous responses can collide

## Future Enhancements

- [ ] Add CRC-16 for better error detection
- [ ] Implement sequence numbers
- [ ] Add message timestamps
- [ ] Support variable baud rates
- [ ] Add bus arbitration for multi-master
- [ ] Implement priority levels
- [ ] Add compression for large payloads
