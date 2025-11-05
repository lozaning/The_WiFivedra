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
- Subordinates buffer only NEW networks (not previously seen)
- Each subordinate tracks last 500 unique networks by MAC address
- Deduplication: Only first sighting of a network is reported
- Controller polls subordinates continuously in round-robin
- Results transmitted only when requested (no delays between polls)
- Eliminates wire collisions in the daisy chain
- Subordinates ONLY transmit when polled (except during discovery)

## Building the Firmware

### Two Firmware Versions Available

WiFivedra firmware is available in **two versions**:

1. **Arduino Framework** (in `controller/` and `subordinate/` directories)
   - Easier to get started
   - Uses familiar Arduino APIs
   - ESP32-C3 used as proxy for ESP32-C5 in PlatformIO

2. **ESP-IDF (Espressif SDK)** (in `controller_espidf/` and `subordinate_espidf/` directories)
   - **Native ESP32-C5 support** ✅
   - Better performance and lower resource usage
   - Production-ready
   - More control over hardware

**For ESP32-C5 hardware, use the ESP-IDF version.**

### Building with Arduino/PlatformIO

#### Prerequisites
- [PlatformIO](https://platformio.org/) installed (`pip install platformio`)

#### Build Commands

Use the provided build script for easy compilation:

```bash
# Build both controller and subordinate firmware
./build.sh all

# Build only controller firmware
./build.sh controller

# Build only subordinate firmware
./build.sh subordinate

# Clean build files
./build.sh clean
```

**Build outputs:**
- Controller firmware: `.pio/build/controller/firmware.bin`
- Subordinate firmware: `.pio/build/subordinate/firmware.bin`

**Note:** The subordinate firmware is configured for ESP32-C3 in PlatformIO as ESP32-C5 support is not yet available. The firmware will work on ESP32-C5 hardware with minor adjustments.

### Alternative: Arduino IDE

Both firmwares can also be compiled using Arduino IDE:
1. Open `controller/controller.ino` or `subordinate/subordinate.ino`
2. Select the appropriate board (ESP32 Dev Module for controller, ESP32-C5 for subordinate)
3. Click Upload

### Building with ESP-IDF (Recommended for ESP32-C5)

For **native ESP32-C5 support** and better performance, use the ESP-IDF versions:

```bash
# Controller (ESP32)
cd controller_espidf
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor

# Subordinate (ESP32-C5)
cd subordinate_espidf
idf.py set-target esp32c5
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**See [ESP-IDF_BUILD.md](ESP-IDF_BUILD.md) for detailed ESP-IDF build instructions.**

## Quick Start

1. **Build Firmware**: Use `./build.sh all` to compile both firmwares
2. **Hardware Setup**: Wire subordinates in daisy chain (see [docs/SETUP.md](docs/SETUP.md))
3. **Flash Firmware**: Upload **identical** subordinate firmware to all ESP32-C5s
4. **Flash Controller**: Upload controller firmware to ESP32
5. **Power On**: System auto-discovers all subordinates on startup
6. **Start Scanning**: Type `start` in controller serial terminal (115200 baud)

## Project Structure

```
The_WiFivedra/
├── common/                           # Arduino framework common files
│   ├── protocol_defs.h               # Protocol definitions and data structures
│   └── serial_protocol.h             # Serial communication handler
├── controller/                       # Arduino framework controller
│   └── controller.ino                # Controller firmware (ESP32)
├── subordinate/                      # Arduino framework subordinate
│   └── subordinate.ino               # Subordinate firmware (ESP32-C5)
├── controller_espidf/                # ESP-IDF controller (native ESP32)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/
│       ├── controller_main.c         # Controller firmware (ESP-IDF)
│       └── protocol_defs.h
├── subordinate_espidf/               # ESP-IDF subordinate (native ESP32-C5)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/
│       ├── subordinate_main.c        # Subordinate firmware (ESP-IDF)
│       └── protocol_defs.h
├── docs/
│   ├── PROTOCOL.md                   # Serial protocol specification
│   └── SETUP.md                      # Hardware and software setup guide
├── platformio.ini                    # PlatformIO build configuration
├── build.sh                          # Build script for Arduino version
└── ESP-IDF_BUILD.md                  # ESP-IDF build instructions
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
2. **Subordinates** begin continuous scanning:
   - Each scan result is checked against the "seen networks" list (500 entries)
   - If network was seen before: Update timestamp and seen count, DON'T buffer
   - If network is NEW: Add to seen list AND buffer for reporting (up to 100 new networks)
3. **Controller** polls subordinates continuously in round-robin (no delays):
   - Sends `CMD_GET_SCAN_RESULTS` to one subordinate
   - Subordinate transmits only NEW networks (not previously seen)
   - Controller logs NEW networks to SD card
   - Controller sends `CMD_CLEAR_RESULTS` to free buffer
   - Immediately polls next subordinate
4. **Cycle time**: Each subordinate polled every ~6 seconds (52 subs × ~120ms per poll)

### Benefits of This Design:
- ✅ **No wire collisions** - One subordinate transmits at a time
- ✅ **Massive bandwidth savings** - Only NEW networks reported
- ✅ **Instant polling** - Controller continuously requests data
- ✅ **Deduplication** - Each subordinate tracks 500 unique networks
- ✅ **Efficient** - No delays, no repeated transmissions

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
- **Deduplication**: 500 unique networks tracked per subordinate (by MAC address)
- **New Network Buffer**: 100 new networks per subordinate
- **Poll Rate**: Each subordinate polled every ~6 seconds (52 subs × ~120ms)
- **Data Rate**: Only NEW networks transmitted (massive bandwidth savings)
- **Transmission**: Continuous polling with zero delays between subordinates
- **Wire Collisions**: Zero (one subordinate transmits at a time)
- **Storage**: ~500 bytes per NEW network detection
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
