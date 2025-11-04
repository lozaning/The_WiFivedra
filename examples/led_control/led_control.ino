/*
 * LED Control Example
 *
 * This example shows how to control LEDs on multiple sub-devices
 * Create different patterns and sequences across the daisy chain
 */

#include "../../controller_esp32/controller_esp32.ino"

void setup() {
  DEBUG_UART.begin(115200);
  CONTROLLER_UART.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  delay(2000);
  DEBUG_UART.println("\n=== LED Control Example ===");
}

void loop() {
  // Example 1: Turn on each LED in sequence (wave pattern)
  DEBUG_UART.println("\n--- Wave Pattern ---");
  for (uint8_t addr = 1; addr <= 5; addr++) {
    uint8_t on = 1;
    sendCommand(addr, CMD_SET_LED, &on, 1);
    delay(200);
    uint8_t off = 0;
    sendCommand(addr, CMD_SET_LED, &off, 1);
    delay(200);
  }

  delay(1000);

  // Example 2: Turn all on, then all off (broadcast)
  DEBUG_UART.println("\n--- Broadcast All On ---");
  uint8_t on = 1;
  sendBroadcast(CMD_SET_LED, &on, 1);
  delay(2000);

  DEBUG_UART.println("--- Broadcast All Off ---");
  uint8_t off = 0;
  sendBroadcast(CMD_SET_LED, &off, 1);
  delay(2000);

  // Example 3: Alternating pattern
  DEBUG_UART.println("\n--- Alternating Pattern ---");
  for (int i = 0; i < 3; i++) {
    // Odd devices on
    for (uint8_t addr = 1; addr <= 5; addr += 2) {
      uint8_t on = 1;
      sendCommand(addr, CMD_SET_LED, &on, 1);
    }
    // Even devices off
    for (uint8_t addr = 2; addr <= 5; addr += 2) {
      uint8_t off = 0;
      sendCommand(addr, CMD_SET_LED, &off, 1);
    }
    delay(500);

    // Swap
    for (uint8_t addr = 1; addr <= 5; addr += 2) {
      uint8_t off = 0;
      sendCommand(addr, CMD_SET_LED, &off, 1);
    }
    for (uint8_t addr = 2; addr <= 5; addr += 2) {
      uint8_t on = 1;
      sendCommand(addr, CMD_SET_LED, &on, 1);
    }
    delay(500);
  }

  delay(2000);
}
