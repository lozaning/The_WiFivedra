# The WiFIVEdra

Dual-band (2.4GHz + 5GHz) WiFi packet sniffer using multiple ESP32-C5 devices.

## Hardware

- Seeed Studio XIAO ESP32-C5 (one or more)

## Firmware

ESP-IDF project in `firmware/`. Captures raw 802.11 frames in promiscuous mode, SLIP-encodes them with a 10-byte header, and streams over USB CDC.

Requires ESP-IDF v6.0. Flash with `idf.py flash`. You must unplug and replug USB after every flash -- the XIAO ESP32-C5 does not auto-reset.

## Host Tools

In `host/`. Requires Python 3 and pyserial (`pip install pyserial`).

**CLI** -- single device, terminal output:

```
python host/sniffer.py /dev/cu.usbmodem* -c 6 -w capture.pcap
```

**GUI** -- multi-device, auto-detection, live charts:

```
python host/sniffer_gui.py
```

The GUI auto-detects all connected ESP32-C5 sniffers, displays packets in a color-coded table, and provides per-device channel/filter/snaplen controls. Includes channel hopping mode for spectrum-wide scanning, PCAP recording, and privacy mode for screenshots.

## Commands

Sent over USB as ASCII text. The firmware supports:

- `CH <n>` -- set WiFi channel
- `FILTER <mgmt|data|ctrl>` -- set frame type filter
- `SNAPLEN <n>` -- truncate frames (0 = full)
- `STATUS` -- query current config
