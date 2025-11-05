# The WiFivedra

A collaboration between [Lozaning](https://x.com/lozaning) and [Rudi](https://x.com/eMbeddedHome)

The WiFivedra is the second generation WiFi monitoring device featuring 48 ESP32-C5 microcontrollers for comprehensive 2.4GHz and 5GHz WiFi channel coverage. Designed for wardriving and WiFi network discovery, this system provides real-time scanning across all WiFi channels simultaneously.

## Features

- **48 ESP32-C5 Subordinates**: Parallel scanning across all WiFi channels
- **Full Band Coverage**: 2.4GHz (channels 1-13) and 5GHz (25+ channels)
- **Serial Protocol**: Robust communication between controller and subordinates
- **Real-time Data**: Instant network detection and logging
- **SD Card Storage**: CSV format logging of all discovered networks
- **Configurable Scanning**: Adjustable scan parameters per subordinate
- **Active/Passive Modes**: Support for both scan types
- **Wardriving Ready**: Designed for mobile WiFi discovery

## Architecture

```
┌─────────────────────────────────────────────────────┐
│           Controller ESP32                          │
│  - Coordinates all subordinates                     │
│  - Aggregates scan results                          │
│  - Logs data to SD card                             │
└──────────────────┬──────────────────────────────────┘
                   │ UART Daisy Chain
                   │ TX ──────────────────────────┐
                   │                              │
                   │ RX ◄─────────────────────────┼─┐
                   ▼                              │ │
              ┌─────────┐ TX                      │ │
              │  Sub 1  ├──────────────┐          │ │
              │ESP32-C5 │              │          │ │
              │ Ch 36   │ RX ◄─────────┼──────────┘ │
              └─────────┘              │            │
                                       ▼            │
                                  ┌─────────┐ TX    │
                                  │  Sub 2  ├───────┤
                                  │ESP32-C5 │       │
                                  │ Ch 40   │ RX ◄──┤
                                  └─────────┘       │
                                       ...          │
                                  ┌─────────┐ TX    │
                                  │ Sub 48  ├───────┘
                                  │ESP32-C5 │
                                  │ Ch 165  │ RX (unused)
                                  └─────────┘
```

## Quick Start

1. **Hardware Setup**: Connect controller and subordinates via serial bus (see [docs/SETUP.md](docs/SETUP.md))
2. **Configure Addresses**: Set unique address (1-48) on each subordinate
3. **Upload Firmware**: Flash controller and subordinate firmware
4. **Start Scanning**: Connect to controller serial terminal and type `start`

## Project Structure

```
The_WiFivedra/
├── common/
│   ├── protocol_defs.h       # Protocol definitions and data structures
│   └── serial_protocol.h     # Serial communication handler
├── controller/
│   └── controller.ino        # Controller firmware (ESP32)
├── subordinate/
│   └── subordinate.ino       # Subordinate firmware (ESP32-C5)
└── docs/
    ├── PROTOCOL.md           # Serial protocol specification
    └── SETUP.md              # Hardware and software setup guide
```

## Serial Protocol

The system uses a UART daisy chain topology with custom packet-based protocol at 115200 baud:

- **Topology**: Controller TX -> Sub1 -> Sub2 -> ... -> Sub48 -> Controller RX
- **Packet Format**: Header (8 bytes) + Payload (0-502 bytes) + Footer (2 bytes)
- **Commands**: 11 command types (ping, scan control, configuration, etc.)
- **Responses**: Status updates, scan results, acknowledgments
- **Forwarding**: Each subordinate forwards packets not addressed to it
- **Error Handling**: Checksums, timeouts, retry logic

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for complete protocol specification.

## Controller Commands

Connect to controller serial terminal (115200 baud):

- `status` or `s` - Show scanning statistics
- `start` - Start WiFi scanning on all subordinates
- `stop` - Stop scanning
- `discover` - Discover connected subordinates
- `config` - Configure subordinates with channel assignments
- `subs <n>` - Set number of subordinates (1-48)
- `help` - Show command list

## Data Format

Scan results are logged to SD card in CSV format:

```csv
timestamp,sub_addr,bssid,ssid,rssi,channel,band,auth
1234567,1,AA:BB:CC:DD:EE:FF,"MyNetwork",-45,36,2,3
```

## Channel Assignment

Each subordinate is automatically assigned a specific channel:

### 5GHz Channels (25 channels)
36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165

### 2.4GHz Channels (13 channels)
1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13

## Hardware Requirements

### Controller
- 1x ESP32 development board
- 1x MicroSD card module
- 1x MicroSD card (8GB+)

### Subordinates
- 48x ESP32-C5 development boards
- UART wiring (TX/RX/GND between devices)
- Power distribution system (~32W peak)

## Software Requirements

- Arduino IDE 2.0+
- ESP32 board support (Espressif)
- No external libraries required (uses built-in WiFi, SD, SPI libraries)

## Performance

- **Scan Rate**: ~1 scan per second per subordinate (configurable)
- **Coverage**: All WiFi channels scanned continuously
- **Data Rate**: Up to ~2400 networks per minute (50 per subordinate)
- **Storage**: ~500 bytes per network detection
- **Battery Life**: ~1-2 hours on 5000mAh LiPo (scanning mode)

## Use Cases

- **Wardriving**: Mobile WiFi network discovery and mapping
- **WiFi Planning**: Site surveys for network deployment
- **Security Research**: WiFi network analysis and monitoring
- **Spectrum Analysis**: 2.4GHz and 5GHz channel utilization
- **Educational**: Learning WiFi protocols and ESP32 programming

## Documentation

- [Protocol Specification](docs/PROTOCOL.md) - Complete serial protocol documentation
- [Setup Guide](docs/SETUP.md) - Hardware assembly and software installation

## Future Enhancements

- GPS integration for location tagging
- Real-time web interface
- Data visualization dashboard
- OTA firmware updates
- Bluetooth/BLE scanning
- WPA handshake capture
- Signal strength mapping

## Legal Notice

This device is designed for authorized WiFi network discovery and monitoring. Users are responsible for complying with local laws and regulations regarding WiFi monitoring and spectrum analysis. Do not use this device to:

- Access networks without authorization
- Interfere with wireless communications
- Violate privacy laws
- Circumvent security measures

Always obtain proper authorization before conducting WiFi surveys or monitoring.

## License

MIT License - See LICENSE file for details

## Contributing

Contributions welcome! Please open issues or pull requests for:
- Bug fixes
- Feature enhancements
- Documentation improvements
- Hardware integration guides

## Credits

Created by:
- [Lozaning](https://x.com/lozaning)
- [Rudi (eMbedded Home)](https://x.com/eMbeddedHome)

Based on the original Wifydra project. 
