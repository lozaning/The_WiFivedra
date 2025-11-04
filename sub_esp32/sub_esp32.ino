/*
 * WiFivedra Daisy Chain Bus - Sub ESP32
 *
 * This sub device receives messages on one UART and forwards them on another UART
 * if the message is not addressed to this device.
 *
 * Protocol Format:
 * [START_BYTE][ADDRESS][LENGTH][COMMAND][DATA...][CHECKSUM]
 *
 * Configuration:
 * - Set MY_ADDRESS to a unique value for each sub device (1-254)
 * - Connect RX_IN to previous device's TX
 * - Connect TX_OUT to next device's RX
 */

#define START_BYTE 0xAA

// DEVICE CONFIGURATION - CHANGE THIS FOR EACH SUB!
#define MY_ADDRESS 1

// UART Configuration
#define UART_IN Serial1   // Receives from previous device
#define UART_OUT Serial2  // Sends to next device
#define DEBUG_UART Serial

// Pin definitions (ESP32 - adjust as needed)
#define RX_IN_PIN 16
#define TX_IN_PIN 17
#define RX_OUT_PIN 18
#define TX_OUT_PIN 19

// Optional LED for visual feedback
#define LED_PIN 2

// Command definitions
#define CMD_PING 0x01
#define CMD_SET_LED 0x02
#define CMD_GET_STATUS 0x03
#define CMD_CUSTOM 0x10

// Message buffer
#define MAX_MESSAGE_SIZE 64

struct Message {
  uint8_t address;
  uint8_t length;
  uint8_t command;
  uint8_t data[MAX_MESSAGE_SIZE];
  uint8_t dataLen;
  uint8_t checksum;
};

// State machine for parsing
uint8_t parseState = 0;
uint8_t calcChecksum = 0;
uint8_t bytesRead = 0;
Message currentMsg;

void setup() {
  DEBUG_UART.begin(115200);
  UART_IN.begin(115200, SERIAL_8N1, RX_IN_PIN, TX_IN_PIN);
  UART_OUT.begin(115200, SERIAL_8N1, RX_OUT_PIN, TX_OUT_PIN);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  delay(2000);
  DEBUG_UART.printf("\n=== WiFivedra Sub Device (Addr: %d) ===\n", MY_ADDRESS);
  DEBUG_UART.println("Waiting for messages...");

  // Blink LED to show device is ready
  for (int i = 0; i < MY_ADDRESS; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
}

void loop() {
  // Check for incoming messages
  if (UART_IN.available()) {
    uint8_t byte = UART_IN.read();
    processIncomingByte(byte);
  }
}

/**
 * Process each incoming byte through state machine
 */
void processIncomingByte(uint8_t byte) {
  switch (parseState) {
    case 0:  // Looking for START_BYTE
      if (byte == START_BYTE) {
        parseState = 1;
        calcChecksum = 0;
        DEBUG_UART.println(">>> Message start");
      }
      break;

    case 1:  // ADDRESS
      currentMsg.address = byte;
      calcChecksum ^= byte;
      parseState = 2;
      DEBUG_UART.printf("    Address: %d (Mine: %d)\n", byte, MY_ADDRESS);
      break;

    case 2:  // LENGTH
      currentMsg.length = byte;
      calcChecksum ^= byte;
      bytesRead = 0;
      parseState = 3;
      DEBUG_UART.printf("    Length: %d\n", byte);
      break;

    case 3:  // COMMAND (first byte of data)
      currentMsg.command = byte;
      currentMsg.data[bytesRead++] = byte;
      calcChecksum ^= byte;
      if (bytesRead >= currentMsg.length) {
        parseState = 4;
      }
      DEBUG_UART.printf("    Command: 0x%02X\n", byte);
      break;

    case 4:  // DATA (remaining bytes)
      currentMsg.data[bytesRead++] = byte;
      calcChecksum ^= byte;
      if (bytesRead >= currentMsg.length) {
        parseState = 5;
      }
      break;

    case 5:  // CHECKSUM
      currentMsg.checksum = byte;
      currentMsg.dataLen = bytesRead - 1;  // Subtract command byte

      if (byte == calcChecksum) {
        DEBUG_UART.println("    Checksum: VALID");
        handleMessage();
      } else {
        DEBUG_UART.printf("    Checksum: INVALID (got 0x%02X, expected 0x%02X)\n",
                         byte, calcChecksum);
      }

      parseState = 0;
      break;
  }
}

/**
 * Handle a complete, validated message
 */
void handleMessage() {
  bool isForMe = (currentMsg.address == MY_ADDRESS);
  bool isBroadcast = (currentMsg.address == 0);

  if (isForMe || isBroadcast) {
    DEBUG_UART.printf("<<< Message for ME! Processing command 0x%02X\n", currentMsg.command);
    processCommand();

    // If it's addressed to me specifically, send a response
    if (isForMe) {
      sendResponse();
    }
  }

  // Forward message if not specifically for me (broadcasts still get forwarded)
  if (!isForMe) {
    DEBUG_UART.println("    Forwarding to next device...");
    forwardMessage();
  }
}

/**
 * Process commands addressed to this device
 */
void processCommand() {
  switch (currentMsg.command) {
    case CMD_PING:
      DEBUG_UART.println("    -> PING received");
      digitalWrite(LED_PIN, HIGH);
      delay(100);
      digitalWrite(LED_PIN, LOW);
      break;

    case CMD_SET_LED:
      if (currentMsg.dataLen >= 1) {
        bool state = currentMsg.data[1] != 0;
        digitalWrite(LED_PIN, state ? HIGH : LOW);
        DEBUG_UART.printf("    -> LED set to %s\n", state ? "ON" : "OFF");
      }
      break;

    case CMD_GET_STATUS:
      DEBUG_UART.println("    -> STATUS requested");
      break;

    case CMD_CUSTOM:
      DEBUG_UART.println("    -> CUSTOM command");
      // Add your custom handling here
      break;

    default:
      DEBUG_UART.printf("    -> Unknown command: 0x%02X\n", currentMsg.command);
      break;
  }
}

/**
 * Send a response back to the controller
 */
void sendResponse() {
  uint8_t response[] = {MY_ADDRESS, 0x01, 0xAC};  // ACK with device address
  uint8_t checksum = 0;

  // Send START_BYTE
  UART_IN.write(START_BYTE);

  // Send response data
  for (uint8_t i = 0; i < sizeof(response); i++) {
    UART_IN.write(response[i]);
    checksum ^= response[i];
  }

  // Send CHECKSUM
  UART_IN.write(checksum);

  DEBUG_UART.println("    -> Response sent");
}

/**
 * Forward the message to the next device in the chain
 */
void forwardMessage() {
  // Send START_BYTE
  UART_OUT.write(START_BYTE);

  // Send ADDRESS
  UART_OUT.write(currentMsg.address);

  // Send LENGTH
  UART_OUT.write(currentMsg.length);

  // Send COMMAND and DATA
  for (uint8_t i = 0; i < currentMsg.length; i++) {
    UART_OUT.write(currentMsg.data[i]);
  }

  // Send CHECKSUM
  UART_OUT.write(currentMsg.checksum);
}
