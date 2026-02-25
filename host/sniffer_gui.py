#!/usr/bin/env python3
"""
5DRA WiFi Packet Sniffer — Multi-Device GUI

Manages multiple ESP32-C5 sniffers simultaneously with a futuristic dark-themed
interface, live packet display, device controls, and real-time visualizations.

Usage:
    python sniffer_gui.py
"""

import glob
import math
import queue
import struct
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog
from collections import namedtuple

import serial

# =============================================================================
# Protocol layer (from sniffer.py)
# =============================================================================

SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

MSG_TYPE_PACKET = 0x01
MSG_TYPE_RESPONSE = 0x02
MSG_TYPE_LOG = 0x03

PCAP_MAGIC = 0xA1B2C3D4
PCAP_VERSION = (2, 4)
PCAP_SNAPLEN = 65535
PCAP_LINKTYPE = 105

FRAME_TYPES = {
    (0, 0): "AssocReq", (0, 1): "AssocResp", (0, 2): "ReassocReq",
    (0, 3): "ReassocResp", (0, 4): "ProbeReq", (0, 5): "ProbeResp",
    (0, 8): "Beacon", (0, 9): "ATIM", (0, 10): "Disassoc",
    (0, 11): "Auth", (0, 12): "Deauth", (0, 13): "Action",
    (1, 8): "BlockAckReq", (1, 9): "BlockAck", (1, 10): "PS-Poll",
    (1, 11): "RTS", (1, 12): "CTS", (1, 13): "ACK",
    (2, 0): "Data", (2, 4): "Null", (2, 8): "QoS Data", (2, 12): "QoS Null",
}


def format_mac(raw):
    return ":".join(f"{b:02x}" for b in raw)


def classify_frame(payload):
    if len(payload) < 2:
        return "TooShort", None, None, -1
    fc = payload[0]
    frame_type = (fc >> 2) & 0x03
    frame_subtype = (fc >> 4) & 0x0F
    name = FRAME_TYPES.get((frame_type, frame_subtype),
                           f"Type{frame_type}/Sub{frame_subtype}")
    da = format_mac(payload[4:10]) if len(payload) >= 10 else None
    sa = format_mac(payload[10:16]) if len(payload) >= 16 else None
    return name, da, sa, frame_type


class SLIPDecoder:
    def __init__(self):
        self._buf = bytearray()
        self._escape = False

    def feed(self, data):
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
                    self._buf.append(b)
                self._escape = False
            else:
                self._buf.append(b)
        return frames


class PCAPWriter:
    def __init__(self, path):
        self._f = open(path, "wb")
        header = struct.pack("<IHHiIII",
                             PCAP_MAGIC, PCAP_VERSION[0], PCAP_VERSION[1],
                             0, 0, PCAP_SNAPLEN, PCAP_LINKTYPE)
        self._f.write(header)
        self._f.flush()
        self._lock = threading.Lock()

    def write_packet(self, payload):
        now = time.time()
        ts_sec = int(now)
        ts_usec = int((now - ts_sec) * 1_000_000)
        pkt_len = len(payload)
        record = struct.pack("<IIII", ts_sec, ts_usec, pkt_len, pkt_len)
        with self._lock:
            self._f.write(record)
            self._f.write(payload)
            self._f.flush()

    def close(self):
        self._f.close()


def parse_frame(frame):
    if len(frame) < 1:
        return None
    msg_type = frame[0]
    if msg_type == MSG_TYPE_PACKET:
        if len(frame) < 10:
            return None
        hdr = struct.unpack_from("<BBbBHI", frame, 0)
        payload = frame[10:]
        return msg_type, {
            "channel": hdr[1], "rssi": hdr[2], "flags": hdr[3],
            "sig_len": hdr[4], "timestamp": hdr[5],
        }, payload
    if msg_type == MSG_TYPE_RESPONSE:
        text = frame[1:].decode("utf-8", errors="replace")
        return msg_type, text, None
    if msg_type == MSG_TYPE_LOG:
        text = frame[1:].decode("utf-8", errors="replace")
        return msg_type, text, None
    return None


# =============================================================================
# Theme and constants
# =============================================================================

COLORS = {
    'bg': '#0a0a0f',
    'bg_secondary': '#0d1117',
    'bg_tertiary': '#161b22',
    'border': '#1a1f2e',
    'text': '#c9d1d9',
    'text_dim': '#6e7681',
    'text_bright': '#f0f6fc',
    'accent_cyan': '#00d4ff',
    'accent_green': '#00ff88',
    'accent_magenta': '#ff2daa',
    'accent_orange': '#ff8800',
    'accent_red': '#ff4444',
    'treeview_sel': '#1c2333',
}

DEVICE_COLORS = [
    '#00d4ff', '#ff2daa', '#00ff88', '#ff8800',
    '#b388ff', '#ffdd00', '#ff4444', '#44ffdd',
]

CHANNELS_24G = list(range(1, 15))
CHANNELS_5G = [36, 40, 44, 48, 52, 56, 60, 64,
               100, 104, 108, 112, 116, 120, 124, 128,
               132, 136, 140, 144, 149, 153, 157, 161, 165]
ALL_CHANNELS = CHANNELS_24G + CHANNELS_5G

FONT_MONO = ("Menlo", 11)
FONT_MONO_SM = ("Menlo", 10)
FONT_MONO_XS = ("Menlo", 9)
FONT_TITLE = ("Menlo", 14, "bold")
FONT_HEADING = ("Menlo", 11, "bold")

SIDEBAR_WIDTH = 290
CHART_HEIGHT = 200
MAX_TABLE_ROWS = 2000
PRUNE_AMOUNT = 500

PacketRecord = namedtuple("PacketRecord", [
    "seq", "device_idx", "port_short", "channel", "rssi",
    "length", "frame_type", "type_code", "da", "sa", "timestamp", "payload"
])


# =============================================================================
# Data structures
# =============================================================================

class RingBuffer:
    def __init__(self, capacity=60):
        self.capacity = capacity
        self._data = [0] * capacity
        self._idx = 0
        self._count = 0

    def append(self, value):
        self._data[self._idx] = value
        self._idx = (self._idx + 1) % self.capacity
        if self._count < self.capacity:
            self._count += 1

    def values(self):
        if self._count < self.capacity:
            return self._data[:self._count]
        return self._data[self._idx:] + self._data[:self._idx]


class DeviceState:
    def __init__(self, port, device_idx, color):
        self.port = port
        self.port_short = port.split("usbmodem")[-1] if "usbmodem" in port else port.split("/")[-1]
        self.device_idx = device_idx
        self.color = color
        self.ser = None
        self.thread = None
        self.stop_event = threading.Event()
        self.packet_queue = queue.Queue(maxsize=5000)
        self.response_queue = queue.Queue(maxsize=100)
        self.decoder = SLIPDecoder()
        self.write_lock = threading.Lock()

        # Settings
        self.channel = 1
        self.band = "2.4G"
        self.filter_mgmt = True
        self.filter_data = True
        self.filter_ctrl = True
        self.snaplen = 0
        self.channel_pending = False  # True after user changes channel, until STATUS confirms

        # Stats
        self.pkt_count = 0
        self.drop_count = 0
        self.pps_counter = 0
        self.pps_ring = RingBuffer(60)
        self.total_ring = RingBuffer(60)
        self.last_pps_time = time.time()

    def send_command(self, cmd):
        if self.ser and self.ser.is_open:
            with self.write_lock:
                try:
                    self.ser.write((cmd + "\n").encode())
                except serial.SerialException:
                    pass


# =============================================================================
# Thread workers
# =============================================================================

def device_reader_loop(device, pcap_writer_ref):
    """Read serial data, decode SLIP, parse packets, enqueue for GUI."""
    while not device.stop_event.is_set():
        try:
            data = device.ser.read(4096)
        except (serial.SerialException, OSError):
            break
        if not data:
            continue

        for frame in device.decoder.feed(data):
            parsed = parse_frame(frame)
            if parsed is None:
                continue

            msg_type = parsed[0]

            if msg_type == MSG_TYPE_RESPONSE:
                try:
                    device.response_queue.put_nowait(parsed[1])
                except queue.Full:
                    pass
                continue

            if msg_type == MSG_TYPE_LOG:
                continue

            if msg_type == MSG_TYPE_PACKET:
                hdr, payload = parsed[1], parsed[2]
                device.pkt_count += 1
                device.pps_counter += 1

                name, da, sa, type_code = classify_frame(payload)

                rec = PacketRecord(
                    seq=device.pkt_count,
                    device_idx=device.device_idx,
                    port_short=device.port_short,
                    channel=hdr["channel"],
                    rssi=hdr["rssi"],
                    length=len(payload),
                    frame_type=name,
                    type_code=type_code,
                    da=da or "?",
                    sa=sa or "?",
                    timestamp=time.time(),
                    payload=payload,
                )

                try:
                    device.packet_queue.put_nowait(rec)
                except queue.Full:
                    device.drop_count += 1

                writer = pcap_writer_ref()
                if writer:
                    writer.write_packet(payload)


def scanner_loop(scanner_queue, stop_event):
    """Poll for USB modem devices every 2s."""
    known = set()
    while not stop_event.is_set():
        current = set(glob.glob("/dev/cu.usbmodem*"))
        for port in current - known:
            scanner_queue.put(("add", port))
        for port in known - current:
            scanner_queue.put(("remove", port))
        known = current
        stop_event.wait(2.0)


# =============================================================================
# GUI components
# =============================================================================

class DeviceCard(tk.Frame):
    def __init__(self, parent, device, gui, **kwargs):
        super().__init__(parent, bg=COLORS['bg_secondary'],
                         highlightbackground=device.color,
                         highlightthickness=1, **kwargs)
        self.device = device
        self.gui = gui

        # Header row
        hdr = tk.Frame(self, bg=COLORS['bg_secondary'])
        hdr.pack(fill=tk.X, padx=8, pady=(6, 2))

        self.indicator = tk.Canvas(hdr, width=10, height=10,
                                   bg=COLORS['bg_secondary'], highlightthickness=0)
        self.indicator.pack(side=tk.LEFT, padx=(0, 6))
        self.indicator.create_oval(1, 1, 9, 9, fill=device.color, outline="")

        tk.Label(hdr, text=device.port_short, font=FONT_HEADING,
                 bg=COLORS['bg_secondary'], fg=device.color).pack(side=tk.LEFT)

        self.band_label = tk.Label(hdr, text=device.band, font=FONT_MONO_XS,
                                   bg=COLORS['bg_secondary'], fg=COLORS['text_dim'])
        self.band_label.pack(side=tk.RIGHT)

        # Channel row
        ch_frame = tk.Frame(self, bg=COLORS['bg_secondary'])
        ch_frame.pack(fill=tk.X, padx=8, pady=2)

        tk.Label(ch_frame, text="CH:", font=FONT_MONO_SM,
                 bg=COLORS['bg_secondary'], fg=COLORS['text_dim']).pack(side=tk.LEFT)

        self.ch_var = tk.StringVar(value=str(device.channel))
        self.ch_combo = ttk.Combobox(ch_frame, textvariable=self.ch_var,
                                     values=[str(c) for c in ALL_CHANNELS],
                                     width=5, state="readonly")
        self.ch_combo.pack(side=tk.LEFT, padx=4)
        self.ch_combo.bind("<<ComboboxSelected>>", self._on_channel_change)

        # Filter row
        filt_frame = tk.Frame(self, bg=COLORS['bg_secondary'])
        filt_frame.pack(fill=tk.X, padx=8, pady=2)

        self.mgmt_var = tk.BooleanVar(value=True)
        self.data_var = tk.BooleanVar(value=True)
        self.ctrl_var = tk.BooleanVar(value=True)

        for text, var in [("MGMT", self.mgmt_var), ("DATA", self.data_var),
                          ("CTRL", self.ctrl_var)]:
            cb = tk.Checkbutton(filt_frame, text=text, variable=var,
                                font=FONT_MONO_XS,
                                bg=COLORS['bg_secondary'], fg=COLORS['text'],
                                selectcolor=COLORS['bg_tertiary'],
                                activebackground=COLORS['bg_secondary'],
                                activeforeground=COLORS['text'],
                                command=self._on_filter_change)
            cb.pack(side=tk.LEFT, padx=(0, 4))

        # Snaplen row
        snap_frame = tk.Frame(self, bg=COLORS['bg_secondary'])
        snap_frame.pack(fill=tk.X, padx=8, pady=2)

        tk.Label(snap_frame, text="SNAP:", font=FONT_MONO_XS,
                 bg=COLORS['bg_secondary'], fg=COLORS['text_dim']).pack(side=tk.LEFT)

        self.snap_var = tk.StringVar(value="0")
        self._snap_last_sent = "0"
        snap_entry = tk.Entry(snap_frame, textvariable=self.snap_var, width=6,
                              font=FONT_MONO_XS, bg=COLORS['bg_tertiary'],
                              fg=COLORS['text'], insertbackground=COLORS['text'],
                              relief=tk.FLAT)
        snap_entry.pack(side=tk.LEFT, padx=4)
        snap_entry.bind("<Return>", lambda e: self._on_snaplen_apply())
        snap_entry.bind("<FocusOut>", lambda e: self._on_snaplen_apply())

        tk.Label(snap_frame, text="(0=full)", font=("Menlo", 8),
                 bg=COLORS['bg_secondary'], fg=COLORS['text_dim']).pack(side=tk.LEFT)

        # Stats row
        self.stats_label = tk.Label(self, text="CAP:0  DROP:0  PPS:0",
                                    font=FONT_MONO_XS,
                                    bg=COLORS['bg_secondary'],
                                    fg=COLORS['text_dim'])
        self.stats_label.pack(fill=tk.X, padx=8, pady=(2, 6))

    def _on_channel_change(self, event=None):
        ch = self.ch_var.get()
        self.device.send_command(f"CH {ch}")
        try:
            self.device.channel = int(ch)
        except ValueError:
            pass
        self.device.channel_pending = True

    def _on_filter_change(self):
        parts = []
        if self.mgmt_var.get():
            parts.append("mgmt")
        if self.data_var.get():
            parts.append("data")
        if self.ctrl_var.get():
            parts.append("ctrl")
        if not parts:
            parts = ["none"]
        self.device.send_command(f"FILTER {' '.join(parts)}")
        self.device.filter_mgmt = self.mgmt_var.get()
        self.device.filter_data = self.data_var.get()
        self.device.filter_ctrl = self.ctrl_var.get()

    def _on_snaplen_apply(self):
        val_str = self.snap_var.get().strip()
        if val_str == self._snap_last_sent:
            return
        try:
            val = int(val_str)
        except ValueError:
            return
        self.device.send_command(f"SNAPLEN {val}")
        self.device.snaplen = val
        self._snap_last_sent = val_str

    def update_stats(self):
        pps = self.device.pps_ring.values()
        current_pps = pps[-1] if pps else 0
        self.stats_label.config(
            text=f"CAP:{self.device.pkt_count}  DROP:{self.device.drop_count}  PPS:{current_pps}")

    def update_from_status(self, text):
        # STATUS format: "CH 1 BAND 2.4G FILTER MGMT DATA CTRL SNAPLEN 0 ..."
        parts = text.split()
        for i, token in enumerate(parts):
            if token == "CH" and i + 1 < len(parts):
                try:
                    ch = int(parts[i + 1])
                    if self.device.channel_pending:
                        # Device confirmed our channel change
                        if ch == self.device.channel:
                            self.device.channel_pending = False
                        # Either way, don't overwrite the combobox while pending
                    else:
                        self.device.channel = ch
                        self.ch_var.set(str(ch))
                except (ValueError, IndexError):
                    pass
            elif token == "BAND" and i + 1 < len(parts):
                band = parts[i + 1]
                self.device.band = band
                self.band_label.config(text=band)


class PacketRateChart(tk.Canvas):
    def __init__(self, parent, **kwargs):
        super().__init__(parent, bg=COLORS['bg_secondary'],
                         highlightthickness=0, **kwargs)
        self._lines = {}  # device_idx -> list of canvas line IDs
        self._title_id = None
        self.bind("<Configure>", self._on_resize)

    def _on_resize(self, event=None):
        self._redraw_title()

    def _redraw_title(self):
        w = self.winfo_width()
        if self._title_id:
            self.delete(self._title_id)
        self._title_id = self.create_text(
            w // 2, 12, text="PACKETS/SEC", font=FONT_MONO_XS,
            fill=COLORS['text_dim'])

    def update_chart(self, devices):
        w = self.winfo_width()
        h = self.winfo_height()
        if w < 20 or h < 30:
            return

        margin_top = 24
        margin_bottom = 4
        margin_left = 4
        margin_right = 4
        plot_w = w - margin_left - margin_right
        plot_h = h - margin_top - margin_bottom

        # Find max PPS across all devices
        max_pps = 1
        for dev in devices:
            vals = dev.pps_ring.values()
            if vals:
                m = max(vals)
                if m > max_pps:
                    max_pps = m

        # Remove lines for devices no longer present
        active_idxs = {d.device_idx for d in devices}
        for idx in list(self._lines.keys()):
            if idx not in active_idxs:
                for lid in self._lines[idx]:
                    self.delete(lid)
                del self._lines[idx]

        for dev in devices:
            vals = dev.pps_ring.values()
            if len(vals) < 2:
                if dev.device_idx in self._lines:
                    for lid in self._lines[dev.device_idx]:
                        self.delete(lid)
                    self._lines[dev.device_idx] = []
                continue

            n = len(vals)
            points = []
            for i, v in enumerate(vals):
                x = margin_left + (i / max(n - 1, 1)) * plot_w
                y = margin_top + plot_h - (v / max_pps) * plot_h
                points.append((x, y))

            # Reuse or create line segments
            existing = self._lines.get(dev.device_idx, [])
            needed = len(points) - 1
            while len(existing) < needed:
                lid = self.create_line(0, 0, 0, 0, fill=dev.color, width=2)
                existing.append(lid)
            while len(existing) > needed:
                self.delete(existing.pop())

            for i in range(needed):
                self.coords(existing[i],
                            points[i][0], points[i][1],
                            points[i + 1][0], points[i + 1][1])

            self._lines[dev.device_idx] = existing

        self._redraw_title()


class ChannelActivityChart(tk.Canvas):
    """Bar chart showing packet counts per channel.

    Uses exponential decay so bars fade smoothly instead of resetting to zero.
    Always shows all known WiFi channels on a fixed x-axis.
    """

    # Fixed channel positions: 2.4G left, gap, 5G right
    DISPLAY_CHANNELS = CHANNELS_24G + CHANNELS_5G

    def __init__(self, parent, **kwargs):
        super().__init__(parent, bg=COLORS['bg_secondary'],
                         highlightthickness=0, **kwargs)
        self._title_id = None
        # Smoothed values per channel (exponential moving average)
        self._smooth = {ch: 0.0 for ch in self.DISPLAY_CHANNELS}
        self._decay = 0.7  # retain 70% of previous value each update
        self.bind("<Configure>", lambda e: self._draw_title())

    def _draw_title(self):
        w = self.winfo_width()
        if self._title_id:
            self.delete(self._title_id)
        self._title_id = self.create_text(
            w // 2, 12, text="CHANNEL ACTIVITY", font=FONT_MONO_XS,
            fill=COLORS['text_dim'])

    def update_chart(self, channel_counts):
        self.delete("bars")
        self.delete("labels")
        w = self.winfo_width()
        h = self.winfo_height()
        if w < 20 or h < 30:
            self._draw_title()
            return

        # Update smoothed values: blend new counts with decayed old values
        for ch in self.DISPLAY_CHANNELS:
            new_val = channel_counts.get(ch, 0)
            self._smooth[ch] = self._smooth[ch] * self._decay + new_val * (1 - self._decay)

        margin_top = 26
        margin_bottom = 18
        margin_left = 4
        margin_right = 4
        plot_w = w - margin_left - margin_right
        plot_h = h - margin_top - margin_bottom

        n = len(self.DISPLAY_CHANNELS)
        max_val = max(self._smooth.values()) if self._smooth else 1
        if max_val < 0.1:
            max_val = 1
        gap = max(1, plot_w * 0.005)
        bar_w = max(2, (plot_w - gap * n) / n)

        for i, ch in enumerate(self.DISPLAY_CHANNELS):
            val = self._smooth[ch]
            bar_h = (val / max_val) * plot_h
            x = margin_left + i * (bar_w + gap)
            y = margin_top + plot_h - bar_h

            # Color: cyan for 2.4G, magenta for 5G
            color = COLORS['accent_cyan'] if ch <= 14 else COLORS['accent_magenta']

            self.create_rectangle(
                x, y, x + bar_w, margin_top + plot_h,
                fill=color, outline="", tags="bars")

            # Label select channels
            if ch in (1, 6, 11, 14, 36, 52, 100, 140, 165):
                self.create_text(
                    x + bar_w / 2, margin_top + plot_h + 8,
                    text=str(ch), font=("Menlo", 7),
                    fill=COLORS['text_dim'], tags="labels")

        self._draw_title()


class FrameTypeChart(tk.Canvas):
    def __init__(self, parent, **kwargs):
        super().__init__(parent, bg=COLORS['bg_secondary'],
                         highlightthickness=0, **kwargs)
        self._title_id = None
        self.bind("<Configure>", lambda e: self._draw_title())

    def _draw_title(self):
        w = self.winfo_width()
        if self._title_id:
            self.delete(self._title_id)
        self._title_id = self.create_text(
            w // 2, 12, text="FRAME TYPES", font=FONT_MONO_XS,
            fill=COLORS['text_dim'])

    def update_chart(self, type_counts):
        self.delete("bars")
        self.delete("labels")
        w = self.winfo_width()
        h = self.winfo_height()
        if w < 20 or h < 30 or not type_counts:
            self._draw_title()
            return

        margin_top = 26
        margin_bottom = 4
        margin_left = 60
        margin_right = 8
        plot_w = w - margin_left - margin_right
        plot_h = h - margin_top - margin_bottom

        sorted_types = sorted(type_counts.items(), key=lambda x: -x[1])[:8]
        if not sorted_types:
            return
        max_count = sorted_types[0][1] if sorted_types else 1
        if max_count == 0:
            max_count = 1

        n = len(sorted_types)
        bar_h = max(3, plot_h / max(n, 1) - 3)
        type_colors = ['#00d4ff', '#ff2daa', '#00ff88', '#ff8800',
                       '#b388ff', '#ffdd00', '#ff4444', '#44ffdd']

        for i, (name, count) in enumerate(sorted_types):
            bar_w = (count / max_count) * plot_w
            y = margin_top + i * (plot_h / n)
            self.create_rectangle(
                margin_left, y, margin_left + bar_w, y + bar_h,
                fill=type_colors[i % len(type_colors)], outline="", tags="bars")
            self.create_text(
                margin_left - 4, y + bar_h / 2,
                text=name[:8], font=("Menlo", 8), anchor=tk.E,
                fill=COLORS['text'], tags="labels")

        self._draw_title()


class RSSIChart(tk.Canvas):
    def __init__(self, parent, **kwargs):
        super().__init__(parent, bg=COLORS['bg_secondary'],
                         highlightthickness=0, **kwargs)
        self._title_id = None
        self.bind("<Configure>", lambda e: self._draw_title())

    def _draw_title(self):
        w = self.winfo_width()
        if self._title_id:
            self.delete(self._title_id)
        self._title_id = self.create_text(
            w // 2, 12, text="RSSI DISTRIBUTION", font=FONT_MONO_XS,
            fill=COLORS['text_dim'])

    def update_chart(self, rssi_bins):
        self.delete("bars")
        self.delete("labels")
        w = self.winfo_width()
        h = self.winfo_height()
        if w < 20 or h < 30:
            self._draw_title()
            return

        margin_top = 26
        margin_bottom = 18
        margin_left = 4
        margin_right = 4
        plot_w = w - margin_left - margin_right
        plot_h = h - margin_top - margin_bottom

        n = len(rssi_bins)
        max_count = max(rssi_bins) if rssi_bins else 1
        if max_count == 0:
            max_count = 1
        bar_w = max(2, plot_w / n - 1)

        for i, count in enumerate(rssi_bins):
            bar_h = (count / max_count) * plot_h
            x = margin_left + i * (plot_w / n)
            y = margin_top + plot_h - bar_h

            # Red to green gradient
            ratio = i / max(n - 1, 1)
            r = int(255 * (1 - ratio))
            g = int(255 * ratio)
            color = f"#{r:02x}{g:02x}44"

            self.create_rectangle(
                x, y, x + bar_w, margin_top + plot_h,
                fill=color, outline="", tags="bars")

        # Label endpoints
        self.create_text(
            margin_left + 4, margin_top + plot_h + 8,
            text="-100", font=("Menlo", 7), anchor=tk.W,
            fill=COLORS['text_dim'], tags="labels")
        self.create_text(
            w - margin_right - 4, margin_top + plot_h + 8,
            text="-10", font=("Menlo", 7), anchor=tk.E,
            fill=COLORS['text_dim'], tags="labels")

        self._draw_title()


class DeviceTotalChart(tk.Canvas):
    """Rolling line chart of total packets captured per device.

    Each device gets its own colored line. Lines that fall behind
    the others are easy to spot — indicates a struggling device.
    Uses a 60-sample ring buffer (one sample per second).
    """

    def __init__(self, parent, **kwargs):
        super().__init__(parent, bg=COLORS['bg_secondary'],
                         highlightthickness=0, **kwargs)
        self._lines = {}      # device_idx -> list of canvas line IDs
        self._legend_ids = [] # canvas IDs for legend text
        self._title_id = None
        self.bind("<Configure>", lambda e: self._redraw_title())

    def _redraw_title(self):
        w = self.winfo_width()
        if self._title_id:
            self.delete(self._title_id)
        self._title_id = self.create_text(
            w // 2, 12, text="TOTAL PKTS / DEVICE", font=FONT_MONO_XS,
            fill=COLORS['text_dim'])

    def update_chart(self, devices):
        w = self.winfo_width()
        h = self.winfo_height()
        if w < 20 or h < 30:
            return

        margin_top = 24
        margin_bottom = 16
        margin_left = 4
        margin_right = 4
        plot_w = w - margin_left - margin_right
        plot_h = h - margin_top - margin_bottom

        # Collect all total-pkt ring buffer values; find global max
        all_series = {}
        global_max = 1
        for dev in devices:
            vals = dev.total_ring.values()
            all_series[dev.device_idx] = (vals, dev.color, dev.port_short)
            if vals:
                m = max(vals)
                if m > global_max:
                    global_max = m

        # Clean up lines for removed devices
        active_idxs = {d.device_idx for d in devices}
        for idx in list(self._lines.keys()):
            if idx not in active_idxs:
                for lid in self._lines[idx]:
                    self.delete(lid)
                del self._lines[idx]

        # Clear old legend
        for lid in self._legend_ids:
            self.delete(lid)
        self._legend_ids.clear()

        legend_y = h - 6
        legend_x = margin_left + 4

        for dev_idx, (vals, color, name) in all_series.items():
            if len(vals) < 2:
                if dev_idx in self._lines:
                    for lid in self._lines[dev_idx]:
                        self.delete(lid)
                    self._lines[dev_idx] = []
                continue

            n = len(vals)
            points = []
            for i, v in enumerate(vals):
                x = margin_left + (i / max(n - 1, 1)) * plot_w
                y = margin_top + plot_h - (v / global_max) * plot_h
                points.append((x, y))

            existing = self._lines.get(dev_idx, [])
            needed = len(points) - 1
            while len(existing) < needed:
                lid = self.create_line(0, 0, 0, 0, fill=color, width=2)
                existing.append(lid)
            while len(existing) > needed:
                self.delete(existing.pop())

            for i in range(needed):
                self.coords(existing[i],
                            points[i][0], points[i][1],
                            points[i + 1][0], points[i + 1][1])

            self._lines[dev_idx] = existing

            # Legend entry
            lid = self.create_text(
                legend_x, legend_y, text=name, font=("Menlo", 7),
                fill=color, anchor=tk.W)
            self._legend_ids.append(lid)
            legend_x += len(name) * 6 + 10

        self._redraw_title()


# =============================================================================
# Main application
# =============================================================================

class SnifferGUI:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("The WiFIVEdra")
        self.root.geometry("1400x850")
        self.root.minsize(900, 600)
        self.root.configure(bg=COLORS['bg'])

        # State
        self.devices = {}       # port -> DeviceState
        self.device_cards = {}  # port -> DeviceCard
        self.next_device_idx = 0
        self.global_seq = 0
        self.table_row_count = 0
        self.scanner_queue = queue.Queue()
        self.scanner_stop = threading.Event()
        self.pcap_writer = None
        self.recording = False

        # Stats for charts
        self.channel_counts = {}       # counts since last chart update
        self.frame_type_counts = {}
        self.rssi_bins = [0] * 18

        # UI toggles
        self.privacy_mode = False
        self.auto_scroll = True
        self.channel_hopping = False
        self.hop_interval_ms = 500
        self.hop_timer_id = None
        self.hop_positions = {}  # device port -> index into ALL_CHANNELS

        # Configure ttk styles
        self._setup_styles()

        # Build UI
        self._build_toolbar()
        self._build_main_area()

        # Start scanner thread
        self.scanner_thread = threading.Thread(
            target=scanner_loop,
            args=(self.scanner_queue, self.scanner_stop),
            daemon=True)
        self.scanner_thread.start()

        # Start periodic callbacks
        self.root.after(75, self._update_packets)
        self.root.after(1000, self._update_charts)
        self.root.after(2000, self._update_devices)

        # Keyboard shortcuts
        self.root.bind("<Command-k>", lambda e: self._clear_table())
        self.root.bind("<Control-k>", lambda e: self._clear_table())

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _setup_styles(self):
        style = ttk.Style()
        style.theme_use("clam")

        style.configure(".", background=COLORS['bg'],
                        foreground=COLORS['text'], fieldbackground=COLORS['bg_tertiary'])

        style.configure("Treeview",
                        background=COLORS['bg_secondary'],
                        foreground=COLORS['text'],
                        fieldbackground=COLORS['bg_secondary'],
                        borderwidth=0,
                        font=FONT_MONO_XS,
                        rowheight=20)
        style.configure("Treeview.Heading",
                        background=COLORS['bg_tertiary'],
                        foreground=COLORS['accent_cyan'],
                        font=FONT_MONO_XS,
                        borderwidth=0)
        style.map("Treeview",
                  background=[("selected", COLORS['treeview_sel'])],
                  foreground=[("selected", COLORS['text_bright'])])
        style.map("Treeview.Heading",
                  background=[("active", COLORS['border'])])

        style.configure("TCombobox",
                        background=COLORS['bg_tertiary'],
                        foreground=COLORS['text_bright'],
                        fieldbackground=COLORS['bg_tertiary'],
                        arrowcolor=COLORS['accent_cyan'],
                        selectbackground=COLORS['bg_tertiary'],
                        selectforeground=COLORS['text_bright'])
        style.map("TCombobox",
                  fieldbackground=[("readonly", COLORS['bg_tertiary'])],
                  foreground=[("readonly", COLORS['text_bright'])],
                  selectbackground=[("readonly", COLORS['bg_tertiary'])],
                  selectforeground=[("readonly", COLORS['text_bright'])],
                  background=[("readonly", COLORS['bg_tertiary'])])

        style.configure("Vertical.TScrollbar",
                        background=COLORS['bg_tertiary'],
                        troughcolor=COLORS['bg_secondary'],
                        borderwidth=0,
                        arrowcolor=COLORS['text_dim'])

        # Style the combobox dropdown listbox (not controlled by ttk)
        self.root.option_add("*TCombobox*Listbox.background", COLORS['bg_tertiary'])
        self.root.option_add("*TCombobox*Listbox.foreground", COLORS['text_bright'])
        self.root.option_add("*TCombobox*Listbox.selectBackground", COLORS['accent_cyan'])
        self.root.option_add("*TCombobox*Listbox.selectForeground", COLORS['bg'])

    def _build_toolbar(self):
        toolbar = tk.Frame(self.root, bg=COLORS['bg_tertiary'], height=44)
        toolbar.pack(fill=tk.X, side=tk.TOP)
        toolbar.pack_propagate(False)

        # Title
        tk.Label(toolbar, text="The WiFIVEdra", font=FONT_TITLE,
                 bg=COLORS['bg_tertiary'], fg=COLORS['accent_cyan']).pack(
            side=tk.LEFT, padx=16)

        # Packet count label
        self.pkt_count_label = tk.Label(
            toolbar, text="0 packets", font=FONT_MONO_SM,
            bg=COLORS['bg_tertiary'], fg=COLORS['text_dim'])
        self.pkt_count_label.pack(side=tk.LEFT, padx=16)

        # Right side buttons
        self.clear_btn = tk.Button(
            toolbar, text="CLEAR", font=FONT_MONO_SM,
            bg=COLORS['bg_secondary'], fg=COLORS['text'],
            activebackground=COLORS['border'],
            activeforeground=COLORS['text_bright'],
            relief=tk.FLAT, padx=12, pady=4,
            command=self._clear_table)
        self.clear_btn.pack(side=tk.RIGHT, padx=8)

        self.rec_btn = tk.Button(
            toolbar, text="\u25cf REC", font=FONT_MONO_SM,
            bg=COLORS['bg_secondary'], fg=COLORS['text'],
            activebackground=COLORS['border'],
            activeforeground=COLORS['accent_red'],
            relief=tk.FLAT, padx=12, pady=4,
            command=self._toggle_recording)
        self.rec_btn.pack(side=tk.RIGHT, padx=4)

    def _build_main_area(self):
        # Main container: sidebar | content
        main = tk.Frame(self.root, bg=COLORS['bg'])
        main.pack(fill=tk.BOTH, expand=True)

        # Sidebar
        sidebar = tk.Frame(main, bg=COLORS['bg'], width=SIDEBAR_WIDTH)
        sidebar.pack(side=tk.LEFT, fill=tk.Y)
        sidebar.pack_propagate(False)

        sidebar_header = tk.Frame(sidebar, bg=COLORS['bg'])
        sidebar_header.pack(fill=tk.X, padx=8, pady=(8, 4))
        tk.Label(sidebar_header, text="DEVICES", font=FONT_HEADING,
                 bg=COLORS['bg'], fg=COLORS['text_dim']).pack(side=tk.LEFT)
        self.device_count_label = tk.Label(
            sidebar_header, text="(0)", font=FONT_MONO_XS,
            bg=COLORS['bg'], fg=COLORS['text_dim'])
        self.device_count_label.pack(side=tk.LEFT, padx=4)

        # Scrollable device list
        self.sidebar_canvas = tk.Canvas(sidebar, bg=COLORS['bg'],
                                        highlightthickness=0)
        self.sidebar_canvas.pack(fill=tk.BOTH, expand=True, padx=4)
        self.device_frame = tk.Frame(self.sidebar_canvas, bg=COLORS['bg'])
        self.sidebar_canvas.create_window(
            (0, 0), window=self.device_frame, anchor=tk.NW,
            tags="device_frame")
        self.device_frame.bind("<Configure>", self._on_sidebar_configure)
        self.sidebar_canvas.bind("<Configure>", self._on_sidebar_canvas_configure)

        # Separator
        tk.Frame(main, bg=COLORS['border'], width=1).pack(
            side=tk.LEFT, fill=tk.Y)

        # Right content area
        content = tk.Frame(main, bg=COLORS['bg'])
        content.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        # --- Pack bottom-up: charts, separator, button bar, then table fills rest ---

        # Charts (very bottom) — use grid for controllable column weights
        self.chart_area = tk.Frame(content, bg=COLORS['bg'], height=CHART_HEIGHT)
        self.chart_area.pack(fill=tk.X, side=tk.BOTTOM)
        self.chart_area.pack_propagate(False)
        self.chart_area.grid_rowconfigure(0, weight=1)

        self.pps_chart = PacketRateChart(self.chart_area)
        self.pps_chart.grid(row=0, column=0, sticky="nsew", padx=(1, 0))

        tk.Frame(self.chart_area, bg=COLORS['border'], width=1).grid(
            row=0, column=1, sticky="ns")

        self.channel_chart = ChannelActivityChart(self.chart_area)
        self.channel_chart.grid(row=0, column=2, sticky="nsew", padx=(0, 0))

        tk.Frame(self.chart_area, bg=COLORS['border'], width=1).grid(
            row=0, column=3, sticky="ns")

        self.frame_chart = FrameTypeChart(self.chart_area)
        self.frame_chart.grid(row=0, column=4, sticky="nsew", padx=(0, 0))

        tk.Frame(self.chart_area, bg=COLORS['border'], width=1).grid(
            row=0, column=5, sticky="ns")

        self.rssi_chart = RSSIChart(self.chart_area)
        self.rssi_chart.grid(row=0, column=6, sticky="nsew", padx=(0, 0))

        tk.Frame(self.chart_area, bg=COLORS['border'], width=1).grid(
            row=0, column=7, sticky="ns")

        self.device_total_chart = DeviceTotalChart(self.chart_area)
        self.device_total_chart.grid(row=0, column=8, sticky="nsew", padx=(0, 1))

        # Default: equal weight for all chart columns
        self._set_chart_weights(normal=True)

        # Chart separator
        tk.Frame(content, bg=COLORS['border'], height=1).pack(
            fill=tk.X, side=tk.BOTTOM)

        # Button bar (above charts)
        btn_bar = tk.Frame(content, bg=COLORS['bg_tertiary'], height=32)
        btn_bar.pack(fill=tk.X, side=tk.BOTTOM)
        btn_bar.pack_propagate(False)

        btn_style = dict(font=FONT_MONO_XS, bg='#ffffff', fg='#000000',
                         activebackground='#dddddd', activeforeground='#000000',
                         relief=tk.FLAT, padx=10, pady=2)

        self.privacy_btn = tk.Button(
            btn_bar, text="PRIVACY", command=self._toggle_privacy, **btn_style)
        self.privacy_btn.pack(side=tk.LEFT, padx=(8, 4), pady=3)

        self.scroll_btn = tk.Button(
            btn_bar, text="AUTO SCROLL: ON", command=self._toggle_auto_scroll,
            **btn_style)
        self.scroll_btn.pack(side=tk.LEFT, padx=4, pady=3)

        self.hop_btn = tk.Button(
            btn_bar, text="CHANNEL HOP", command=self._toggle_channel_hopping,
            **btn_style)
        self.hop_btn.pack(side=tk.LEFT, padx=4, pady=3)

        tk.Label(btn_bar, text="ms:", font=FONT_MONO_XS,
                 bg=COLORS['bg_tertiary'], fg=COLORS['text_dim']).pack(
            side=tk.LEFT, padx=(4, 0))
        self.hop_interval_var = tk.StringVar(value="500")
        self.hop_interval_entry = tk.Entry(
            btn_bar, textvariable=self.hop_interval_var, width=5,
            font=FONT_MONO_XS, bg=COLORS['bg_tertiary'],
            fg=COLORS['text'], insertbackground=COLORS['text'],
            relief=tk.FLAT)
        self.hop_interval_entry.pack(side=tk.LEFT, padx=(2, 8), pady=3)

        # Packet table (fills remaining space)
        table_frame = tk.Frame(content, bg=COLORS['bg'])
        table_frame.pack(fill=tk.BOTH, expand=True)

        columns = ("seq", "dev", "ch", "rssi", "len", "type", "da", "sa")
        self.tree = ttk.Treeview(table_frame, columns=columns,
                                 show="headings", selectmode="browse")

        col_widths = {"seq": 60, "dev": 80, "ch": 40, "rssi": 50,
                      "len": 50, "type": 100, "da": 140, "sa": 140}
        col_labels = {"seq": "#", "dev": "Device", "ch": "Ch", "rssi": "RSSI",
                      "len": "Len", "type": "Type", "da": "DA", "sa": "SA"}

        for col in columns:
            self.tree.heading(col, text=col_labels[col])
            self.tree.column(col, width=col_widths[col], minwidth=30)

        # Configure per-device color tags
        for i, color in enumerate(DEVICE_COLORS):
            self.tree.tag_configure(f"dev{i}", foreground=color)

        scrollbar = ttk.Scrollbar(table_frame, orient=tk.VERTICAL,
                                  command=self.tree.yview)
        self.tree.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.tree.pack(fill=tk.BOTH, expand=True)

    def _on_sidebar_configure(self, event=None):
        self.sidebar_canvas.configure(
            scrollregion=self.sidebar_canvas.bbox("all"))

    def _on_sidebar_canvas_configure(self, event=None):
        self.sidebar_canvas.itemconfig(
            "device_frame", width=event.width)

    # --- Periodic callbacks ---

    def _update_packets(self):
        """Drain packet queues from all devices, insert into table."""
        batch = []
        for dev in list(self.devices.values()):
            drained = 0
            while drained < 200:
                try:
                    rec = dev.packet_queue.get_nowait()
                    batch.append(rec)
                    drained += 1
                except queue.Empty:
                    break

        if batch:
            privacy = self.privacy_mode
            privacy_mac = "[redacted]"
            privacy_str = "[privacy]"

            for rec in batch:
                self.global_seq += 1
                tag = f"dev{rec.device_idx % len(DEVICE_COLORS)}"
                da = privacy_mac if privacy else rec.da
                sa = privacy_mac if privacy else rec.sa
                self.tree.insert("", tk.END, values=(
                    self.global_seq, rec.port_short, rec.channel,
                    rec.rssi, rec.length, rec.frame_type,
                    da, sa
                ), tags=(tag,))
                self.table_row_count += 1

                # Update chart stats
                ch = rec.channel
                self.channel_counts[ch] = self.channel_counts.get(ch, 0) + 1
                self.frame_type_counts[rec.frame_type] = \
                    self.frame_type_counts.get(rec.frame_type, 0) + 1
                rssi_idx = max(0, min(17, (rec.rssi + 100) // 5))
                self.rssi_bins[rssi_idx] += 1

            # Prune if needed
            if self.table_row_count > MAX_TABLE_ROWS:
                children = self.tree.get_children()
                for iid in children[:PRUNE_AMOUNT]:
                    self.tree.delete(iid)
                self.table_row_count -= PRUNE_AMOUNT

            if self.auto_scroll:
                self.tree.yview_moveto(1.0)

            self.pkt_count_label.config(text=f"{self.global_seq} packets")

        self.root.after(75, self._update_packets)

    def _update_charts(self):
        """Update PPS ring buffers and redraw charts."""
        devices = list(self.devices.values())

        for dev in devices:
            dev.pps_ring.append(dev.pps_counter)
            dev.total_ring.append(dev.pkt_count)
            dev.pps_counter = 0

        # Update device card stats
        for port, card in list(self.device_cards.items()):
            if port in self.devices:
                card.update_stats()

        # Redraw charts — channel chart uses smoothing internally,
        # pass current window counts then reset for next window
        self.pps_chart.update_chart(devices)
        self.device_total_chart.update_chart(devices)
        self.channel_chart.update_chart(self.channel_counts)
        self.channel_counts = {}
        self.frame_chart.update_chart(self.frame_type_counts)
        self.rssi_chart.update_chart(self.rssi_bins)

        self.root.after(1000, self._update_charts)

    def _update_devices(self):
        """Process scanner events, poll device STATUS."""
        # Process scanner queue
        while True:
            try:
                action, port = self.scanner_queue.get_nowait()
            except queue.Empty:
                break

            if action == "add" and port not in self.devices:
                self._probe_and_add_device(port)
            elif action == "remove" and port in self.devices:
                self._remove_device(port)

        # Poll STATUS for each device
        for dev in list(self.devices.values()):
            dev.send_command("STATUS")
            # Drain response queue for status updates
            while True:
                try:
                    resp = dev.response_queue.get_nowait()
                    if dev.port in self.device_cards:
                        self.device_cards[dev.port].update_from_status(resp)
                except queue.Empty:
                    break

        self.device_count_label.config(text=f"({len(self.devices)})")
        self.root.after(2000, self._update_devices)

    # --- Device management ---

    def _probe_and_add_device(self, port):
        """Try to open a serial port and confirm it's a 5dra device."""
        try:
            ser = serial.Serial(port, 921600, timeout=0.5)
        except (serial.SerialException, OSError):
            return

        try:
            # Flush any stale data
            ser.reset_input_buffer()
            ser.write(b"STATUS\n")
            time.sleep(0.3)

            confirmed = False
            status_text = ""
            decoder = SLIPDecoder()

            # Read multiple chunks — STATUS response may be buried behind
            # a flood of packet data on busy devices
            for attempt in range(3):
                raw = ser.read(4096)
                if not raw:
                    if attempt == 0:
                        # No data at all on first read — not a device
                        ser.close()
                        return
                    continue

                frames = decoder.feed(raw)
                for frame in frames:
                    parsed = parse_frame(frame)
                    if parsed is None:
                        continue
                    if parsed[0] == MSG_TYPE_RESPONSE:
                        text = parsed[1]
                        if "CH" in text.upper():
                            status_text = text
                            confirmed = True
                    elif parsed[0] == MSG_TYPE_PACKET:
                        # Valid SLIP-decoded packet confirms this is a 5dra device
                        confirmed = True

                if status_text:
                    break

                # If we got packets but no status yet, send STATUS again
                if confirmed and not status_text and attempt < 2:
                    ser.write(b"STATUS\n")
                    time.sleep(0.3)

            if not confirmed:
                ser.close()
                return

        except (serial.SerialException, OSError):
            try:
                ser.close()
            except Exception:
                pass
            return

        # Confirmed — create device
        idx = self.next_device_idx
        self.next_device_idx += 1
        color = DEVICE_COLORS[idx % len(DEVICE_COLORS)]

        dev = DeviceState(port, idx, color)
        dev.ser = ser

        # Parse initial status: "CH 1 BAND 2.4G FILTER ..."
        parts = status_text.split()
        for i, token in enumerate(parts):
            if token == "CH" and i + 1 < len(parts):
                try:
                    dev.channel = int(parts[i + 1])
                except (ValueError, IndexError):
                    pass
            elif token == "BAND" and i + 1 < len(parts):
                dev.band = parts[i + 1]

        # Start reader thread
        dev.thread = threading.Thread(
            target=device_reader_loop,
            args=(dev, lambda: self.pcap_writer),
            daemon=True)
        dev.thread.start()

        self.devices[port] = dev

        # Create UI card
        card = DeviceCard(self.device_frame, dev, self)
        card.pack(fill=tk.X, padx=4, pady=4)
        card.ch_var.set(str(dev.channel))
        if status_text:
            card.update_from_status(status_text)
        self.device_cards[port] = card

    def _remove_device(self, port):
        """Clean up a disconnected device."""
        if port not in self.devices:
            return

        dev = self.devices.pop(port)
        dev.stop_event.set()

        try:
            dev.ser.close()
        except Exception:
            pass

        if port in self.device_cards:
            self.device_cards[port].destroy()
            del self.device_cards[port]

    # --- Actions ---

    def _clear_table(self):
        for item in self.tree.get_children():
            self.tree.delete(item)
        self.table_row_count = 0
        self.global_seq = 0
        self.channel_counts = {}
        self.frame_type_counts = {}
        self.rssi_bins = [0] * 18
        self.channel_chart._smooth = {ch: 0.0 for ch in ChannelActivityChart.DISPLAY_CHANNELS}
        self.pkt_count_label.config(text="0 packets")

    def _toggle_privacy(self):
        self.privacy_mode = not self.privacy_mode
        if self.privacy_mode:
            self.privacy_btn.config(bg=COLORS['accent_magenta'], fg='#ffffff')
            # Redact all existing rows
            for iid in self.tree.get_children():
                vals = list(self.tree.item(iid, "values"))
                vals[6] = "[redacted]"  # DA
                vals[7] = "[redacted]"  # SA
                self.tree.item(iid, values=vals)
        else:
            self.privacy_btn.config(bg='#ffffff', fg='#000000')
            # Note: can't un-redact already displayed rows (data not stored),
            # but new rows will show real addresses

    def _toggle_auto_scroll(self):
        self.auto_scroll = not self.auto_scroll
        if self.auto_scroll:
            self.scroll_btn.config(text="AUTO SCROLL: ON")
            self.tree.yview_moveto(1.0)
        else:
            self.scroll_btn.config(text="AUTO SCROLL: OFF")

    def _set_chart_weights(self, normal=True):
        """Set grid column weights for chart area. When hopping, channel chart is wider."""
        if normal:
            for col in (0, 2, 4, 6, 8):
                self.chart_area.grid_columnconfigure(col, weight=1)
        else:
            # Channel chart (col 2) gets 3x weight
            self.chart_area.grid_columnconfigure(0, weight=1)
            self.chart_area.grid_columnconfigure(2, weight=3)
            self.chart_area.grid_columnconfigure(4, weight=1)
            self.chart_area.grid_columnconfigure(6, weight=1)
            self.chart_area.grid_columnconfigure(8, weight=1)

    def _toggle_channel_hopping(self):
        if self.channel_hopping:
            self._stop_hopping()
        else:
            self._start_hopping()

    def _start_hopping(self):
        devices = list(self.devices.values())
        if not devices:
            return

        # Parse interval
        try:
            interval = int(self.hop_interval_var.get())
            if interval < 50:
                interval = 50
        except ValueError:
            interval = 500
        self.hop_interval_ms = interval

        self.channel_hopping = True
        self.hop_btn.config(bg=COLORS['accent_green'], fg='#000000')

        # Distribute devices evenly across all channels
        n_dev = len(devices)
        n_ch = len(ALL_CHANNELS)
        spacing = n_ch / n_dev
        for i, dev in enumerate(devices):
            start_idx = int(i * spacing) % n_ch
            self.hop_positions[dev.port] = start_idx
            ch = ALL_CHANNELS[start_idx]
            dev.send_command(f"CH {ch}")
            dev.channel = ch
            dev.channel_pending = True
            if dev.port in self.device_cards:
                self.device_cards[dev.port].ch_var.set(str(ch))

        # Enlarge channel chart
        self._set_chart_weights(normal=False)

        # Start hop timer
        self._hop_tick()

    def _hop_tick(self):
        if not self.channel_hopping:
            return
        n_ch = len(ALL_CHANNELS)
        for dev in list(self.devices.values()):
            if dev.port not in self.hop_positions:
                # New device joined during hopping — slot it in
                self.hop_positions[dev.port] = 0
            idx = (self.hop_positions[dev.port] + 1) % n_ch
            self.hop_positions[dev.port] = idx
            ch = ALL_CHANNELS[idx]
            dev.send_command(f"CH {ch}")
            dev.channel = ch
            dev.channel_pending = True
            if dev.port in self.device_cards:
                self.device_cards[dev.port].ch_var.set(str(ch))

        self.hop_timer_id = self.root.after(self.hop_interval_ms, self._hop_tick)

    def _stop_hopping(self):
        self.channel_hopping = False
        self.hop_btn.config(bg='#ffffff', fg='#000000')
        if self.hop_timer_id:
            self.root.after_cancel(self.hop_timer_id)
            self.hop_timer_id = None
        self.hop_positions.clear()
        self._set_chart_weights(normal=True)

    def _toggle_recording(self):
        if self.recording:
            # Stop recording
            self.recording = False
            self.rec_btn.config(fg=COLORS['text'], bg=COLORS['bg_secondary'])
            if self.pcap_writer:
                self.pcap_writer.close()
                self.pcap_writer = None
        else:
            # Start recording
            path = filedialog.asksaveasfilename(
                defaultextension=".pcap",
                filetypes=[("PCAP files", "*.pcap"), ("All files", "*.*")],
                title="Save PCAP capture")
            if not path:
                return
            self.pcap_writer = PCAPWriter(path)
            self.recording = True
            self.rec_btn.config(fg=COLORS['accent_red'], bg='#2a0a0a')

    def _on_close(self):
        """Clean shutdown."""
        if self.channel_hopping:
            self._stop_hopping()
        self.scanner_stop.set()
        for dev in list(self.devices.values()):
            dev.stop_event.set()
            try:
                dev.ser.close()
            except Exception:
                pass
        if self.pcap_writer:
            self.pcap_writer.close()
        self.root.destroy()

    def run(self):
        self.root.mainloop()


# =============================================================================
# Entry point
# =============================================================================

if __name__ == "__main__":
    app = SnifferGUI()
    app.run()
