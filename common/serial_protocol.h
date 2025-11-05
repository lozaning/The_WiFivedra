#ifndef SERIAL_PROTOCOL_H
#define SERIAL_PROTOCOL_H

#include "protocol_defs.h"
#include <Arduino.h>

class SerialProtocol {
private:
  HardwareSerial* upstreamSerial;    // Connection to previous device (toward controller)
  HardwareSerial* downstreamSerial;  // Connection to next device (away from controller)
  uint8_t myAddress;
  uint8_t seqNum;
  bool isEndNode;  // True if this is the last device in the chain

  // RX Buffers (separate for upstream and downstream)
  uint8_t upstreamRxBuffer[MAX_PACKET_SIZE];
  uint16_t upstreamRxIndex;
  bool upstreamReceiving;
  unsigned long upstreamLastByteTime;

  uint8_t downstreamRxBuffer[MAX_PACKET_SIZE];
  uint16_t downstreamRxIndex;
  bool downstreamReceiving;
  unsigned long downstreamLastByteTime;

  // TX Buffer
  Packet txPacket;

public:
  // Constructor for controller (only downstream connection)
  SerialProtocol(HardwareSerial* downstream, uint8_t address)
    : upstreamSerial(nullptr), downstreamSerial(downstream), myAddress(address),
      seqNum(0), isEndNode(false),
      upstreamRxIndex(0), upstreamReceiving(false),
      downstreamRxIndex(0), downstreamReceiving(false) {}

  // Constructor for subordinates (upstream and downstream connections)
  SerialProtocol(HardwareSerial* upstream, HardwareSerial* downstream, uint8_t address, bool isLast = false)
    : upstreamSerial(upstream), downstreamSerial(downstream), myAddress(address),
      seqNum(0), isEndNode(isLast),
      upstreamRxIndex(0), upstreamReceiving(false),
      downstreamRxIndex(0), downstreamReceiving(false) {}

  void begin(unsigned long baudRate = SERIAL_BAUD_RATE) {
    if (upstreamSerial != nullptr) {
      upstreamSerial->begin(baudRate);
      upstreamSerial->setTimeout(100);
    }
    if (downstreamSerial != nullptr) {
      downstreamSerial->begin(baudRate);
      downstreamSerial->setTimeout(100);
    }
  }

  void setEndNode(bool isLast) {
    isEndNode = isLast;
  }

  // Send a packet (determines direction automatically)
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

    // Determine direction: responses/messages to controller go upstream, commands go downstream
    HardwareSerial* targetSerial = nullptr;
    if (destAddr < myAddress) {
      // Going toward controller (upstream)
      targetSerial = upstreamSerial;
    } else {
      // Going away from controller (downstream)
      targetSerial = downstreamSerial;
    }

    if (targetSerial == nullptr) {
      return false;  // No connection in that direction
    }

    // Send packet
    targetSerial->write((uint8_t*)&txPacket.header, sizeof(PacketHeader));
    if (length > 0) {
      targetSerial->write(txPacket.payload, length);
    }
    targetSerial->write((uint8_t*)&txPacket.footer, sizeof(PacketFooter));
    targetSerial->flush();

    return true;
  }

  // Forward a packet (used when packet is not for this device)
  bool forwardPacket(const Packet& packet) {
    // Determine direction based on destination address
    HardwareSerial* targetSerial = nullptr;
    if (packet.header.destAddr < myAddress) {
      // Forward upstream (toward controller)
      targetSerial = upstreamSerial;
    } else {
      // Forward downstream (away from controller)
      targetSerial = downstreamSerial;
    }

    if (targetSerial == nullptr) {
      return false;  // No connection in that direction
    }

    // Forward the packet as-is (don't modify anything)
    targetSerial->write((uint8_t*)&packet.header, sizeof(PacketHeader));
    if (packet.header.length > 0) {
      targetSerial->write(packet.payload, packet.header.length);
    }
    targetSerial->write((uint8_t*)&packet.footer, sizeof(PacketFooter));
    targetSerial->flush();

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

private:
  // Helper function to process received bytes from a specific serial port
  bool processSerialInput(HardwareSerial* serial, uint8_t* rxBuffer, uint16_t& rxIndex,
                          bool& receiving, unsigned long& lastByteTime, Packet& packet) {
    if (serial == nullptr) {
      return false;
    }

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
                // Check if packet is for us or broadcast
                if (packet.header.destAddr == myAddress || packet.header.destAddr == 0xFF) {
                  return true;  // Packet is for this device
                } else {
                  // Packet is not for us - forward it
                  forwardPacket(packet);
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

public:
  // Receive packet (non-blocking) - checks both upstream and downstream
  bool receivePacket(Packet& packet) {
    // Check upstream serial first (messages from controller direction)
    if (processSerialInput(upstreamSerial, upstreamRxBuffer, upstreamRxIndex,
                           upstreamReceiving, upstreamLastByteTime, packet)) {
      return true;
    }

    // Check downstream serial (messages from subordinates direction)
    if (processSerialInput(downstreamSerial, downstreamRxBuffer, downstreamRxIndex,
                           downstreamReceiving, downstreamLastByteTime, packet)) {
      return true;
    }

    return false;
  }

  uint8_t getAddress() const { return myAddress; }
  void setAddress(uint8_t addr) { myAddress = addr; }
};

#endif // SERIAL_PROTOCOL_H
