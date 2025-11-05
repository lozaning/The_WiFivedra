# WiFivedra Serial Protocol Specification

## Overview

The WiFivedra serial protocol enables communication between a controller ESP32 and up to 52 subordinate ESP32-C5 devices for comprehensive WiFi scanning across all 2.4GHz and 5GHz channels. The protocol includes automatic address assignment via daisy chain discovery.

## Protocol Version

Current Version: **1**

## Physical Layer

- **Interface**: UART Serial (Daisy Chain)
- **Baud Rate**: 115200 bps
- **Data Bits**: 8
- **Parity**: None
- **Stop Bits**: 1
- **Flow Control**: None
- **Topology**: Direct ESP32-to-ESP32 daisy chain (no RS-485 transceivers)

### Network Topology

The system uses a **daisy chain topology** where each device is connected to its immediate neighbors:

```
Controller <-> Sub1 <-> Sub2 <-> ... <-> Sub52
```

**Message Routing:**
- **Commands (Controller → Subordinates)**: Flow downstream (toward higher addresses)
  - Each subordinate receives all messages
  - If addressed to it (or broadcast), processes the message
  - If addressed to a higher numbered subordinate, forwards it downstream
  - If addressed to the controller or lower numbered device, does NOT forward

- **Responses (Subordinates → Controller)**: Flow upstream (toward lower addresses)
  - Subordinate sends response upstream
  - Each device forwards responses not addressed to it toward the controller
  - Response stops when it reaches the destination

**Key behaviors:**
- Messages automatically stop at their destination (no unnecessary forwarding)
- Each subordinate acts as both an endpoint and a router
- Broadcast messages (0xFF) are processed by all devices but not re-forwarded
- Message forwarding is transparent - applications don't need to handle routing

## Packet Structure

All communication uses a structured packet format:

```
+----------------+----------------+----------------+
| Packet Header  |    Payload     | Packet Footer  |
| (8 bytes)      | (0-502 bytes)  | (2 bytes)      |
+----------------+----------------+----------------+
```

### Packet Header (8 bytes)

| Offset | Field       | Size | Description                              |
|--------|-------------|------|------------------------------------------|
| 0      | Start Mark  | 1    | Start marker (0xAA)                      |
| 1      | Version     | 1    | Protocol version (0x01)                  |
| 2      | Dest Addr   | 1    | Destination address (0x00-0x30, 0xFF)    |
| 3      | Src Addr    | 1    | Source address (0x00-0x30)               |
| 4      | Type        | 1    | Command or Response type                 |
| 5-6    | Length      | 2    | Payload length (big-endian)              |
| 7      | Seq Num     | 1    | Sequence number                          |

### Payload (0-502 bytes)

Variable length data specific to the command or response.

### Packet Footer (2 bytes)

| Offset | Field      | Size | Description                              |
|--------|------------|------|------------------------------------------|
| 0      | Checksum   | 1    | XOR checksum of header and payload       |
| 1      | End Mark   | 1    | End marker (0x55)                        |

### Addressing

- **0x00**: Controller (master)
- **0x01-0x34**: Subordinates 1-52 (auto-assigned during discovery)
- **0xFE**: Unassigned device (used during auto-discovery phase)
- **0xFF**: Broadcast to all devices

## Command Types (Controller → Subordinate)

| Code | Command              | Description                          | Payload                    |
|------|---------------------|--------------------------------------|----------------------------|
| 0x01 | CMD_PING            | Test connectivity                    | None                       |
| 0x02 | CMD_ASSIGN_ADDRESS  | Auto-discovery address assignment    | AddressAssignment (2 bytes)|
| 0x10 | CMD_SET_SCAN_PARAMS | Configure scan parameters            | ScanParams (9 bytes)       |
| 0x11 | CMD_START_SCAN      | Start WiFi scanning                  | None                       |
| 0x12 | CMD_STOP_SCAN       | Stop WiFi scanning                   | None                       |
| 0x13 | CMD_GET_STATUS      | Request status information           | None                       |
| 0x14 | CMD_SET_CHANNEL     | Set specific channel                 | uint8_t channel            |
| 0x15 | CMD_GET_SCAN_RESULTS| Request buffered scan results        | None                       |
| 0x16 | CMD_CLEAR_RESULTS   | Clear result buffer                  | None                       |
| 0x17 | CMD_SET_SCAN_MODE   | Set scan mode (active/passive)       | uint8_t mode               |
| 0x18 | CMD_SET_SCAN_INTERVAL| Set scan interval                   | uint16_t interval_ms       |
| 0xFF | CMD_RESET           | Reset subordinate                    | None                       |

## Response Types (Subordinate → Controller)

| Code | Response            | Description                          | Payload                    |
|------|---------------------|--------------------------------------|----------------------------|
| 0x01 | RESP_ACK            | Acknowledgment                       | None                       |
| 0x02 | RESP_NACK           | Negative acknowledgment              | ErrorCode (1 byte)         |
| 0x03 | RESP_ADDRESS_ASSIGNED| Address assignment confirmation     | AddressAssignment (2 bytes)|
| 0x10 | RESP_STATUS         | Status information                   | StatusInfo (16 bytes)      |
| 0x20 | RESP_SCAN_RESULT    | WiFi scan result                     | WiFiScanResult (47 bytes)  |
| 0x21 | RESP_SCAN_COMPLETE  | Scan cycle complete                  | None                       |
| 0xFE | RESP_ERROR          | Error response                       | ErrorCode (1 byte)         |

## Data Structures

### AddressAssignment (2 bytes)

Used for auto-discovery address assignment.

| Offset | Field           | Size | Description                              |
|--------|-----------------|------|------------------------------------------|
| 0      | assignedAddress | 1    | Address being assigned to the subordinate|
| 1      | isLastNode      | 1    | 1 if last node in chain, 0 otherwise     |

**Auto-Discovery Process:**
1. Controller sends CMD_ASSIGN_ADDRESS(address=1) downstream
2. First unassigned subordinate receives it, adopts address 1
3. Subordinate tries to forward CMD_ASSIGN_ADDRESS(address=2) downstream with timeout
4. If timeout (no response), subordinate sets isLastNode=1
5. Subordinate sends RESP_ADDRESS_ASSIGNED upstream to controller
6. Process repeats for each subordinate in the chain
7. Controller counts responses to determine total number of subordinates

### ScanParams (9 bytes)

| Offset | Field       | Size | Description                              |
|--------|-------------|------|------------------------------------------|
| 0      | band        | 1    | WiFi band (0x01=2.4GHz, 0x02=5GHz, 0x03=Both) |
| 1      | channel     | 1    | Specific channel (0=all for band)        |
| 2      | scanMode    | 1    | Scan mode (0x01=active, 0x02=passive)    |
| 3-4    | scanTimeMs  | 2    | Time per channel in ms                   |
| 5-6    | intervalMs  | 2    | Interval between scans in ms             |
| 7      | hidden      | 1    | Scan for hidden networks (0=no, 1=yes)   |
| 8      | showHidden  | 1    | Show hidden in results (0=no, 1=yes)     |

### WiFiScanResult (47 bytes)

| Offset | Field       | Size | Description                              |
|--------|-------------|------|------------------------------------------|
| 0-5    | bssid       | 6    | MAC address of AP                        |
| 6-38   | ssid        | 33   | Network name (null-terminated)           |
| 39     | rssi        | 1    | Signal strength (signed)                 |
| 40     | channel     | 1    | Channel number                           |
| 41     | band        | 1    | WiFi band                                |
| 42     | authMode    | 1    | Authentication mode                      |
| 43-46  | timestamp   | 4    | Timestamp (ms since boot)                |

### StatusInfo (16 bytes)

| Offset | Field       | Size | Description                              |
|--------|-------------|------|------------------------------------------|
| 0      | state       | 1    | Current state (idle/scanning/processing/error) |
| 1      | channel     | 1    | Current channel                          |
| 2      | band        | 1    | Current band                             |
| 3-4    | scanCount   | 2    | Number of scans performed                |
| 5-6    | resultCount | 2    | Number of results in buffer              |
| 7-10   | uptime      | 4    | Uptime in seconds                        |
| 11     | lastError   | 1    | Last error code                          |
| 12     | freeHeap    | 1    | Free heap percentage                     |

## Error Codes

| Code | Name                | Description                          |
|------|---------------------|--------------------------------------|
| 0x00 | ERR_NONE            | No error                             |
| 0x01 | ERR_INVALID_COMMAND | Unknown command                      |
| 0x02 | ERR_INVALID_PARAMS  | Invalid parameters                   |
| 0x03 | ERR_TIMEOUT         | Operation timeout                    |
| 0x04 | ERR_BUSY            | Device is busy                       |
| 0x05 | ERR_NOT_READY       | Device not ready                     |
| 0x06 | ERR_SCAN_FAILED     | WiFi scan failed                     |
| 0x07 | ERR_BUFFER_FULL     | Result buffer full                   |
| 0x08 | ERR_CHECKSUM        | Checksum mismatch                    |
| 0xFF | ERR_UNKNOWN         | Unknown error                        |

## Communication Flow

### Device Discovery

```
Controller                          Subordinate
    |                                    |
    |--- CMD_PING (broadcast) --------->|
    |                                    |
    |<---------- RESP_ACK ---------------|
    |                                    |
```

### Configuration

```
Controller                          Subordinate
    |                                    |
    |--- CMD_SET_SCAN_PARAMS ----------->|
    |                                    |
    |<---------- RESP_ACK ---------------|
    |                                    |
```

### Scanning Operation

```
Controller                          Subordinate
    |                                    |
    |--- CMD_START_SCAN ---------------->|
    |                                    |
    |<---------- RESP_ACK ---------------|
    |                                    |
    |                                    | (performs scan)
    |                                    |
    |<----- RESP_SCAN_RESULT ------------|
    |<----- RESP_SCAN_RESULT ------------|
    |<----- RESP_SCAN_RESULT ------------|
    |                                    |
    |<----- RESP_SCAN_COMPLETE ----------|
    |                                    |
    |                                    | (wait interval)
    |                                    |
    |                                    | (performs scan)
    |                                    |
    |<----- RESP_SCAN_RESULT ------------|
    |     ...                            |
```

## Channel Assignment

### 5GHz Channels (25 channels)

Subordinates 0-24 are assigned the following 5GHz channels:
- 36, 40, 44, 48, 52, 56, 60, 64
- 100, 104, 108, 112, 116, 120, 124, 128
- 132, 136, 140, 144
- 149, 153, 157, 161, 165

### 2.4GHz Channels (13 channels)

Subordinates can be configured for channels 1-13 in the 2.4GHz band.

## Example Packet

**CMD_PING to subordinate 5:**

```
Hex: AA 01 05 00 01 00 00 00 05 55
```

Breakdown:
- `AA`: Start marker
- `01`: Protocol version 1
- `05`: Destination address (subordinate 5)
- `00`: Source address (controller)
- `01`: Command type (CMD_PING)
- `00 00`: Payload length (0)
- `00`: Sequence number (0)
- `05`: Checksum (0x01 ^ 0x05 ^ 0x00 ^ 0x01 = 0x05)
- `55`: End marker

## Timing Considerations

- **Command Timeout**: 5000 ms
- **Scan Timeout**: 10000 ms
- **Response Delay**: 10 ms minimum between responses
- **Typical Scan Interval**: 1000 ms

## Error Handling

1. **Checksum Failure**: Packet is discarded silently
2. **Invalid Command**: Subordinate responds with RESP_NACK + ERR_INVALID_COMMAND
3. **Timeout**: Controller should retry up to 3 times
4. **Buffer Overflow**: Subordinate responds with RESP_NACK + ERR_BUFFER_FULL

## Implementation Notes

- All multi-byte values are transmitted in big-endian format
- Sequence numbers wrap around at 255
- Subordinates should respond within 100ms of receiving a command
- Controller should implement retry logic with exponential backoff
- Maximum packet size is 512 bytes total

## Security Considerations

This protocol does not implement encryption or authentication. It is designed for use in a trusted environment with physical access control. For deployments requiring security:

1. Implement encryption at the transport layer
2. Add authentication tokens to command packets
3. Use secure boot on all devices
4. Implement packet signing

## Future Extensions

Possible future enhancements:

- Packet fragmentation for larger data transfers
- Compression for scan results
- Multi-controller support
- OTA firmware update commands
- GPS coordinate tagging
- Real-time clock synchronization
