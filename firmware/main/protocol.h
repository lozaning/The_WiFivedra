#pragma once

#include <stdint.h>

/* --- Message types --- */
#define MSG_TYPE_PACKET   0x01
#define MSG_TYPE_RESPONSE 0x02
#define MSG_TYPE_LOG      0x03

/* --- Packet flags (in pkt_header_t.flags) --- */
#define PKT_FLAG_COMPRESSED  0x01

/* --- SLIP framing (RFC 1055) --- */
#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

/* --- 802.11 constants --- */
#define IEEE80211_FCS_LEN    4
#define MAX_80211_FRAME_LEN  2500

/* --- Buffer sizes --- */
#define SLIP_BUF_SIZE  5120

/* --- Default snaplen (0 = no truncation) --- */
#define DEFAULT_SNAPLEN  0

/* --- Packet metadata header (wire format, little-endian) --- */
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;    /* MSG_TYPE_PACKET */
    uint8_t  channel;     /* WiFi channel number */
    int8_t   rssi;        /* Signal strength in dBm */
    uint8_t  flags;       /* PKT_FLAG_* bits */
    uint16_t sig_len;     /* Original payload length (before compression) */
    uint32_t timestamp;   /* Microsecond timestamp from rx_ctrl */
} pkt_header_t;

_Static_assert(sizeof(pkt_header_t) == 10, "pkt_header_t must be 10 bytes");
