#!/usr/bin/env python3
"""
5dra WiFi Packet Sniffer — Host CLI

Reads SLIP-encoded 802.11 frames from ESP32-C5 over USB CDC,
displays live packet info, and writes PCAP files.

Usage:
    python sniffer.py /dev/cu.usbmodem* -c 6 -w capture.pcap
    python sniffer.py /dev/cu.usbmodem* --filter mgmt --snaplen 128
"""

import argparse
import struct
import sys
import threading
import time
import serial

# --- SLIP constants ---
SLIP_END     = 0xC0
SLIP_ESC     = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

# --- Message types ---
MSG_TYPE_PACKET   = 0x01
MSG_TYPE_RESPONSE = 0x02
MSG_TYPE_LOG      = 0x03

# --- Packet flags ---
PKT_FLAG_COMPRESSED = 0x01

# --- PCAP constants ---
PCAP_MAGIC     = 0xA1B2C3D4
PCAP_VERSION   = (2, 4)
PCAP_SNAPLEN   = 65535
PCAP_LINKTYPE  = 105  # LINKTYPE_IEEE802_11

# --- 802.11 frame type/subtype names ---
FRAME_TYPES = {
    (0, 0): "AssocReq",
    (0, 1): "AssocResp",
    (0, 2): "ReassocReq",
    (0, 3): "ReassocResp",
    (0, 4): "ProbeReq",
    (0, 5): "ProbeResp",
    (0, 8): "Beacon",
    (0, 9): "ATIM",
    (0, 10): "Disassoc",
    (0, 11): "Auth",
    (0, 12): "Deauth",
    (0, 13): "Action",
    (1, 8): "BlockAckReq",
    (1, 9): "BlockAck",
    (1, 10): "PS-Poll",
    (1, 11): "RTS",
    (1, 12): "CTS",
    (1, 13): "ACK",
    (2, 0): "Data",
    (2, 4): "Null",
    (2, 8): "QoS Data",
    (2, 12): "QoS Null",
}


def format_mac(raw):
    """Format 6 bytes as a MAC address string."""
    return ":".join(f"{b:02x}" for b in raw)


def classify_frame(payload):
    """Decode 802.11 Frame Control to get type/subtype name, DA, SA."""
    if len(payload) < 2:
        return "TooShort", None, None

    fc = payload[0]
    frame_type = (fc >> 2) & 0x03
    frame_subtype = (fc >> 4) & 0x0F

    name = FRAME_TYPES.get((frame_type, frame_subtype),
                           f"Type{frame_type}/Sub{frame_subtype}")

    da = format_mac(payload[4:10]) if len(payload) >= 10 else None
    sa = format_mac(payload[10:16]) if len(payload) >= 16 else None

    return name, da, sa


class SLIPDecoder:
    """Stateful SLIP byte-stream decoder."""

    def __init__(self):
        self._buf = bytearray()
        self._escape = False

    def feed(self, data):
        """Feed raw bytes, yield complete SLIP frames."""
        frames = []
        for b in data:
            if b == SLIP_END:
                if len(self._buf) > 0:
                    frames.append(bytes(self._buf))
                    self._buf = bytearray()
                self._escape = False
            elif b == SLIP_ESC:
                self._escape = True
            elif self._escape:
                if b == SLIP_ESC_END:
                    self._buf.append(SLIP_END)
                elif b == SLIP_ESC_ESC:
                    self._buf.append(SLIP_ESC)
                else:
                    self._buf.append(b)  # malformed, pass through
                self._escape = False
            else:
                self._buf.append(b)
        return frames


class PCAPWriter:
    """Write PCAP files with LINKTYPE_IEEE802_11."""

    def __init__(self, path):
        self._f = open(path, "wb")
        # Global header
        header = struct.pack("<IHHiIII",
                             PCAP_MAGIC,
                             PCAP_VERSION[0], PCAP_VERSION[1],
                             0,              # thiszone
                             0,              # sigfigs
                             PCAP_SNAPLEN,
                             PCAP_LINKTYPE)
        self._f.write(header)
        self._f.flush()

    def write_packet(self, payload):
        """Write a single packet record with current wall-clock timestamp."""
        now = time.time()
        ts_sec = int(now)
        ts_usec = int((now - ts_sec) * 1_000_000)
        pkt_len = len(payload)
        record = struct.pack("<IIII", ts_sec, ts_usec, pkt_len, pkt_len)
        self._f.write(record)
        self._f.write(payload)
        self._f.flush()

    def close(self):
        self._f.close()


def parse_frame(frame):
    """Unpack a SLIP frame into (msg_type, header_dict, payload) or None."""
    if len(frame) < 1:
        return None

    msg_type = frame[0]

    if msg_type == MSG_TYPE_PACKET:
        if len(frame) < 10:
            return None
        hdr = struct.unpack_from("<BBbBHI", frame, 0)
        payload = frame[10:]

        return msg_type, {
            "channel":   hdr[1],
            "rssi":      hdr[2],
            "flags":     hdr[3],
            "sig_len":   hdr[4],
            "timestamp": hdr[5],
        }, payload

    if msg_type == MSG_TYPE_RESPONSE:
        text = frame[1:].decode("utf-8", errors="replace")
        return msg_type, text, None

    if msg_type == MSG_TYPE_LOG:
        text = frame[1:].decode("utf-8", errors="replace")
        return msg_type, text, None

    return None


def reader_thread(ser, decoder, pcap_writer, stop_event):
    """Read from serial, decode SLIP, display and write packets."""
    pkt_count = 0
    while not stop_event.is_set():
        try:
            data = ser.read(4096)
        except serial.SerialException:
            break
        if not data:
            continue

        for frame in decoder.feed(data):
            parsed = parse_frame(frame)
            if parsed is None:
                continue

            msg_type = parsed[0]

            if msg_type == MSG_TYPE_RESPONSE:
                print(f"\n<< {parsed[1]}")
                continue

            if msg_type == MSG_TYPE_LOG:
                print(f"\n[LOG] {parsed[1]}")
                continue

            if msg_type == MSG_TYPE_PACKET:
                hdr, payload = parsed[1], parsed[2]
                pkt_count += 1

                name, da, sa = classify_frame(payload)
                line = (f"#{pkt_count:<6d} "
                        f"ch={hdr['channel']:<3d} "
                        f"rssi={hdr['rssi']:<4d} "
                        f"len={len(payload):<5d} "
                        f"{name:<14s} "
                        f"DA={da or '?':<17s} "
                        f"SA={sa or '?'}")
                print(line)

                if pcap_writer:
                    pcap_writer.write_packet(payload)


def main():
    parser = argparse.ArgumentParser(
        description="5dra WiFi Packet Sniffer — Host CLI")
    parser.add_argument("port", help="Serial port (e.g. /dev/cu.usbmodem*)")
    parser.add_argument("-b", "--baud", type=int, default=921600,
                        help="Baud rate (default: 921600)")
    parser.add_argument("-c", "--channel", type=int, default=None,
                        help="Initial WiFi channel")
    parser.add_argument("-w", "--write", metavar="FILE", default=None,
                        help="Write PCAP file")
    parser.add_argument("-s", "--snaplen", type=int, default=None,
                        help="Truncate frames to N bytes (0=full)")
    parser.add_argument("-f", "--filter", type=str, default=None,
                        help="Frame filter: mgmt, data, ctrl, all "
                             "(combine with +, e.g. mgmt+data)")
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    print(f"Connected to {args.port} @ {args.baud} baud")

    pcap_writer = None
    if args.write:
        pcap_writer = PCAPWriter(args.write)
        print(f"Writing PCAP to {args.write}")

    # Send initial configuration commands
    if args.filter:
        filt = args.filter.replace("+", " ").replace(",", " ")
        ser.write(f"FILTER {filt}\n".encode())
        print(f"Requested filter: {filt}")

    if args.snaplen is not None:
        ser.write(f"SNAPLEN {args.snaplen}\n".encode())
        print(f"Requested snaplen: {args.snaplen}")

    if args.channel:
        ser.write(f"CH {args.channel}\n".encode())
        print(f"Requested channel {args.channel}")

    decoder = SLIPDecoder()
    stop_event = threading.Event()

    reader = threading.Thread(target=reader_thread,
                              args=(ser, decoder, pcap_writer, stop_event),
                              daemon=True)
    reader.start()

    # Command input loop
    print("\nCommands: ch <N>, filter <types>, snaplen <N>, status, quit")
    print("Listening for packets...\n")
    try:
        while True:
            try:
                line = input()
            except EOFError:
                break

            line = line.strip()
            if not line:
                continue

            if line.lower() == "quit":
                break

            # Normalize filter shorthand
            if line.lower().startswith("filter "):
                line = line.replace("+", " ").replace(",", " ")

            ser.write((line + "\n").encode())

    except KeyboardInterrupt:
        pass
    finally:
        print("\nShutting down...")
        stop_event.set()
        reader.join(timeout=2)
        ser.close()
        if pcap_writer:
            pcap_writer.close()
            print("PCAP file saved.")


if __name__ == "__main__":
    main()
