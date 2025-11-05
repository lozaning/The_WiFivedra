#ifndef SERIAL_PROTOCOL_H
#define SERIAL_PROTOCOL_H

#include "protocol_defs.h"
#include <Arduino.h>

class SerialProtocol {
private:
  HardwareSerial* serial;
  uint8_t myAddress;
  uint8_t seqNum;

  // RX Buffer
  uint8_t rxBuffer[MAX_PACKET_SIZE];
  uint16_t rxIndex;
  bool receiving;
  unsigned long lastByteTime;

  // TX Buffer
  Packet txPacket;

public:
  SerialProtocol(HardwareSerial* serialPort, uint8_t address)
    : serial(serialPort), myAddress(address), seqNum(0), rxIndex(0), receiving(false) {}

  void begin(unsigned long baudRate = SERIAL_BAUD_RATE) {
    serial->begin(baudRate);
    serial->setTimeout(100);
  }

  // Send a packet
  bool sendPacket(uint8_t destAddr, uint8_t type, const uint8_t* payload, uint16_t length) {
    if (length > MAX_PAYLOAD_SIZE) {
      return false;
    }

    // Build header
    txPacket.header.startMarker = PACKET_START_MARKER;
    txPacket.header.version = PROTOCOL_VERSION;
    txPacket.header.destAddr = destAddr;
    txPacket.header.srcAddr = myAddress;
    txPacket.header.type = type;
    txPacket.header.length = length;
    txPacket.header.seqNum = seqNum++;

    // Copy payload
    if (payload != nullptr && length > 0) {
      memcpy(txPacket.payload, payload, length);
    }

    // Build footer
    txPacket.footer.checksum = txPacket.calculateChecksum();
    txPacket.footer.endMarker = PACKET_END_MARKER;

    // Send packet
    serial->write((uint8_t*)&txPacket.header, sizeof(PacketHeader));
    if (length > 0) {
      serial->write(txPacket.payload, length);
    }
    serial->write((uint8_t*)&txPacket.footer, sizeof(PacketFooter));
    serial->flush();

    return true;
  }

  // Send command with payload
  bool sendCommand(uint8_t destAddr, CommandType cmd, const void* params = nullptr, uint16_t size = 0) {
    return sendPacket(destAddr, cmd, (const uint8_t*)params, size);
  }

  // Send response with payload
  bool sendResponse(uint8_t destAddr, ResponseType resp, const void* data = nullptr, uint16_t size = 0) {
    return sendPacket(destAddr, resp, (const uint8_t*)data, size);
  }

  // Send ACK
  bool sendAck(uint8_t destAddr) {
    return sendResponse(destAddr, RESP_ACK, nullptr, 0);
  }

  // Send NACK
  bool sendNack(uint8_t destAddr, ErrorCode error) {
    return sendResponse(destAddr, RESP_NACK, &error, sizeof(error));
  }

  // Receive packet (non-blocking)
  bool receivePacket(Packet& packet) {
    unsigned long currentTime = millis();

    while (serial->available()) {
      uint8_t byte = serial->read();
      lastByteTime = currentTime;

      // Look for start marker
      if (!receiving) {
        if (byte == PACKET_START_MARKER) {
          rxBuffer[0] = byte;
          rxIndex = 1;
          receiving = true;
        }
        continue;
      }

      // Receive packet data
      if (rxIndex < MAX_PACKET_SIZE) {
        rxBuffer[rxIndex++] = byte;

        // Check if we have at least the header
        if (rxIndex >= sizeof(PacketHeader)) {
          PacketHeader* header = (PacketHeader*)rxBuffer;

          // Check if we have the complete packet
          uint16_t expectedSize = sizeof(PacketHeader) + header->length + sizeof(PacketFooter);

          if (rxIndex >= expectedSize) {
            // Check end marker
            if (rxBuffer[expectedSize - 1] == PACKET_END_MARKER) {
              // Copy to packet structure
              memcpy(&packet.header, rxBuffer, sizeof(PacketHeader));
              memcpy(packet.payload, rxBuffer + sizeof(PacketHeader), packet.header.length);
              memcpy(&packet.footer, rxBuffer + sizeof(PacketHeader) + packet.header.length, sizeof(PacketFooter));

              // Reset receive state
              receiving = false;
              rxIndex = 0;

              // Verify checksum
              if (packet.verifyChecksum()) {
                // Check if packet is for us (or broadcast)
                if (packet.header.destAddr == myAddress || packet.header.destAddr == 0xFF) {
                  return true;
                }
              }
            }

            // Invalid packet, reset
            receiving = false;
            rxIndex = 0;
          }
        }
      } else {
        // Buffer overflow, reset
        receiving = false;
        rxIndex = 0;
      }
    }

    // Timeout check
    if (receiving && (currentTime - lastByteTime > 100)) {
      receiving = false;
      rxIndex = 0;
    }

    return false;
  }

  uint8_t getAddress() const { return myAddress; }
  void setAddress(uint8_t addr) { myAddress = addr; }
};

#endif // SERIAL_PROTOCOL_H
