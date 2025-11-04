/*
 * WiFivedra Daisy Chain Bus - Controller ESP32
 *
 * This controller sends messages down a daisy-chained UART bus to sub ESP32 devices.
 * Each message is addressed to a specific sub device by its address (1-254).
 *
 * Protocol Format:
 * [START_BYTE][ADDRESS][LENGTH][COMMAND][DATA...][CHECKSUM]
 *
 * START_BYTE: 0xAA (fixed)
 * ADDRESS: 1-254 (0 = broadcast, 255 = reserved)
 * LENGTH: Number of bytes in COMMAND + DATA
 * COMMAND: Command byte
 * DATA: Variable length data
 * CHECKSUM: XOR of all bytes except START_BYTE
 */

#define START_BYTE 0xAA
#define CONTROLLER_UART Serial1
#define DEBUG_UART Serial

// UART pins (change as needed)
#define TX_PIN 17
#define RX_PIN 16

// Command definitions
#define CMD_PING 0x01
#define CMD_SET_LED 0x02
#define CMD_GET_STATUS 0x03
#define CMD_CUSTOM 0x10

// Response handling
#define RESPONSE_TIMEOUT 1000  // ms
#define MAX_RESPONSE_SIZE 64

struct Message {
  uint8_t address;
  uint8_t command;
  uint8_t* data;
  uint8_t dataLen;
};

void setup() {
  DEBUG_UART.begin(115200);
  CONTROLLER_UART.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  delay(2000);
  DEBUG_UART.println("\n=== WiFivedra Daisy Chain Controller ===");
  DEBUG_UART.println("Controller initialized");
}

void loop() {
  // Example: Ping all devices in sequence
  for (uint8_t addr = 1; addr <= 5; addr++) {
    DEBUG_UART.printf("\n--- Pinging device %d ---\n", addr);
    sendCommand(addr, CMD_PING, NULL, 0);
    delay(100);

    // Check for response
    if (waitForResponse(addr, 500)) {
      DEBUG_UART.printf("Device %d responded!\n", addr);
    } else {
      DEBUG_UART.printf("Device %d no response\n", addr);
    }

    delay(1000);
  }

  delay(5000);
}

/**
 * Send a command to a specific device
 */
void sendCommand(uint8_t address, uint8_t command, uint8_t* data, uint8_t dataLen) {
  uint8_t length = 1 + dataLen;  // command + data
  uint8_t checksum = 0;

  // Send START_BYTE
  CONTROLLER_UART.write(START_BYTE);

  // Send ADDRESS
  CONTROLLER_UART.write(address);
  checksum ^= address;

  // Send LENGTH
  CONTROLLER_UART.write(length);
  checksum ^= length;

  // Send COMMAND
  CONTROLLER_UART.write(command);
  checksum ^= command;

  // Send DATA
  for (uint8_t i = 0; i < dataLen; i++) {
    CONTROLLER_UART.write(data[i]);
    checksum ^= data[i];
  }

  // Send CHECKSUM
  CONTROLLER_UART.write(checksum);

  DEBUG_UART.printf("Sent to addr %d: cmd=0x%02X len=%d checksum=0x%02X\n",
                    address, command, length, checksum);
}

/**
 * Send a broadcast command to all devices
 */
void sendBroadcast(uint8_t command, uint8_t* data, uint8_t dataLen) {
  sendCommand(0, command, data, dataLen);
}

/**
 * Wait for a response from a device
 */
bool waitForResponse(uint8_t expectedAddr, uint16_t timeout) {
  unsigned long startTime = millis();
  uint8_t state = 0;
  uint8_t checksum = 0;
  uint8_t address = 0;
  uint8_t length = 0;
  uint8_t bytesRead = 0;
  uint8_t buffer[MAX_RESPONSE_SIZE];

  while (millis() - startTime < timeout) {
    if (CONTROLLER_UART.available()) {
      uint8_t byte = CONTROLLER_UART.read();

      switch (state) {
        case 0:  // Looking for START_BYTE
          if (byte == START_BYTE) {
            state = 1;
            checksum = 0;
          }
          break;

        case 1:  // ADDRESS
          address = byte;
          checksum ^= byte;
          state = 2;
          break;

        case 2:  // LENGTH
          length = byte;
          checksum ^= byte;
          bytesRead = 0;
          state = 3;
          break;

        case 3:  // DATA
          buffer[bytesRead++] = byte;
          checksum ^= byte;
          if (bytesRead >= length) {
            state = 4;
          }
          break;

        case 4:  // CHECKSUM
          if (byte == checksum) {
            DEBUG_UART.printf("Valid response from addr %d: ", address);
            for (uint8_t i = 0; i < length; i++) {
              DEBUG_UART.printf("0x%02X ", buffer[i]);
            }
            DEBUG_UART.println();
            return (address == expectedAddr);
          } else {
            DEBUG_UART.println("Checksum error!");
          }
          state = 0;
          break;
      }
    }
  }

  return false;
}

/**
 * Example: Set LED on a specific device
 */
void setDeviceLED(uint8_t address, bool state) {
  uint8_t data = state ? 1 : 0;
  sendCommand(address, CMD_SET_LED, &data, 1);
}

/**
 * Example: Get status from a device
 */
void getDeviceStatus(uint8_t address) {
  sendCommand(address, CMD_GET_STATUS, NULL, 0);
}
