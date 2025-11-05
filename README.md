# The WiFivedra

A collaboration between [Lozaning](https://x.com/lozaning) and [Rudi](https://x.com/eMbeddedHome)

The WiFivedra is the second generation WiFi monitoring device featuring up to 52 ESP32-C5 microcontrollers for comprehensive 2.4GHz and 5GHz WiFi channel coverage. Designed for wardriving and WiFi network discovery, this system provides real-time scanning across all WiFi channels simultaneously.

## Features

- **52 ESP32-C5 Subordinates**: Massively parallel scanning across all WiFi channels with double coverage
- **Auto-Discovery**: Flash identical firmware to all subordinates - no manual configuration!
- **Daisy Chain Architecture**: Direct ESP32-to-ESP32 connections, no RS-485 transceivers needed
- **Full Band Coverage**: 2.4GHz (channels 1-13) and 5GHz (all 25 channels, double coverage)
- **Efficient Protocol**: Poll-based communication eliminates wire collisions
- **Real-time Data**: Network detection with buffered result transmission
- **SD Card Storage**: CSV format logging of all discovered networks
- **Configurable Scanning**: Adjustable scan parameters per subordinate
- **Active/Passive Modes**: Support for both scan types
- **Wardriving Ready**: Designed for mobile WiFi discovery

## Architecture

**Daisy Chain Topology** - Direct ESP32-to-ESP32 UART connections:

```
┌────────────┐       ┌─────────┐       ┌─────────┐             ┌─────────┐
│ Controller │◄─────►│  Sub 1  │◄─────►│  Sub 2  │◄───...───►│  Sub 52 │
│   ESP32    │       │ ESP32-C5│       │ ESP32-C5│             │ ESP32-C5│
│            │       │  Ch 36  │       │  Ch 40  │             │  Ch 40* │
│ SD Logging │       │  (Addr1)│       │  (Addr2)│             │ (Addr52)│
└────────────┘       └─────────┘       └─────────┘             └─────────┘
  Commands →            ← Responses (on request)
```

*Channels cycle: 52 subs provide double coverage of 25 5GHz channels

### Key Design Decisions

**Auto-Discovery**:
- All subordinates boot with unassigned address
- Controller initiates daisy chain discovery on startup
- Each subordinate auto-assigns itself and detects if it's last in chain
- No manual address configuration needed!

**Poll-Based Communication**:
- Subordinates buffer scan results locally (100 results)
- Controller polls subordinates in round-robin (every 5 seconds)
- Results transmitted only when requested
- Eliminates wire collisions in the daisy chain
- Subordinates ONLY transmit when polled (except during discovery)

## Quick Start

1. **Hardware Setup**: Wire subordinates in daisy chain (see [docs/SETUP.md](docs/SETUP.md))
2. **Flash Firmware**: Upload **identical** subordinate firmware to all ESP32-C5s
3. **Flash Controller**: Upload controller firmware to ESP32
4. **Power On**: System auto-discovers all subordinates on startup
5. **Start Scanning**: Type `start` in controller serial terminal (115200 baud)

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

The system uses a custom serial protocol at 115200 baud with structured packets:

- **Packet Format**: Header (8 bytes) + Payload (0-502 bytes) + Footer (2 bytes)
- **Commands**: 11 command types (ping, scan control, configuration, etc.)
- **Responses**: Status updates, scan results, acknowledgments
- **Error Handling**: Checksums, timeouts, retry logic

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for complete protocol specification.

## Controller Commands

Connect to controller serial terminal (115200 baud):

- `status` or `s` - Show scanning statistics
- `start` - Start WiFi scanning on all subordinates
- `stop` - Stop scanning
- `autodiscover` - Re-run auto-discovery to find subordinates
- `discover` - Ping all discovered subordinates
- `config` - Configure subordinates with channel assignments
- `help` - Show command list

## Scanning Workflow

1. **Controller** sends `CMD_START_SCAN` to all subordinates
2. **Subordinates** begin continuous scanning and buffer results (up to 100 networks)
3. **Controller** polls subordinates in round-robin every 5 seconds:
   - Sends `CMD_GET_SCAN_RESULTS` to one subordinate
   - Subordinate transmits all buffered results
   - Controller logs results to SD card
   - Controller sends `CMD_CLEAR_RESULTS` to free buffer
4. **Repeat** - Each subordinate is polled every ~4 minutes (52 subs × 5 sec)

This design prevents wire collisions and ensures smooth data flow in the daisy chain.

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
- Up to 52x ESP32-C5 development boards
- UART connections (TX/RX wiring between neighbors)
- No RS-485 transceivers needed!
- Power distribution system (~35W peak for 52 subordinates)

## Software Requirements

- Arduino IDE 2.0+
- ESP32 board support (Espressif)
- No external libraries required (uses built-in WiFi, SD, SPI libraries)

## Performance

- **Scan Rate**: ~1 scan per second per subordinate (configurable)
- **Coverage**: All WiFi channels with double coverage (52 subs / 25 channels)
- **Buffer Capacity**: 100 networks per subordinate
- **Poll Rate**: Each subordinate polled every ~4 minutes (5 sec × 52 subs)
- **Data Rate**: Efficient batched transmission, no wire collisions
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
