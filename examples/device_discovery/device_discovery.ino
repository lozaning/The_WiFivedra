/*
 * Device Discovery Example
 *
 * This example shows how to automatically discover devices on the bus
 * Useful for initial setup and debugging
 */

#include "../../controller_esp32/controller_esp32.ino"

#define MAX_DEVICES 254
bool devicePresent[MAX_DEVICES + 1];  // Track which devices are present

void setup() {
  DEBUG_UART.begin(115200);
  CONTROLLER_UART.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  delay(2000);
  DEBUG_UART.println("\n=== Device Discovery Example ===");

  // Initialize device tracking
  for (int i = 0; i <= MAX_DEVICES; i++) {
    devicePresent[i] = false;
  }
}

void loop() {
  DEBUG_UART.println("\n--- Starting Device Discovery ---");
  int deviceCount = discoverDevices();

  DEBUG_UART.printf("\nFound %d device(s)\n", deviceCount);
  DEBUG_UART.print("Active addresses: ");
  for (uint8_t addr = 1; addr <= MAX_DEVICES; addr++) {
    if (devicePresent[addr]) {
      DEBUG_UART.printf("%d ", addr);
    }
  }
  DEBUG_UART.println();

  // Wait before next discovery
  delay(10000);
}

/**
 * Discover all devices on the bus
 * Returns the number of devices found
 */
int discoverDevices() {
  int count = 0;

  // Ping all possible addresses (1-254)
  // For faster discovery, you can limit the range
  for (uint8_t addr = 1; addr <= 20; addr++) {
    DEBUG_UART.printf("Probing address %d... ", addr);

    sendCommand(addr, CMD_PING, NULL, 0);

    if (waitForResponse(addr, 200)) {
      DEBUG_UART.println("FOUND!");
      devicePresent[addr] = true;
      count++;
    } else {
      DEBUG_UART.println("no response");
      devicePresent[addr] = false;
    }

    delay(50);  // Small delay between pings
  }

  return count;
}

/**
 * Check if a specific device is present
 */
bool isDevicePresent(uint8_t address) {
  if (address == 0 || address > MAX_DEVICES) return false;
  return devicePresent[address];
}

/**
 * Get list of active devices
 */
void getActiveDevices(uint8_t* list, int* count) {
  *count = 0;
  for (uint8_t addr = 1; addr <= MAX_DEVICES; addr++) {
    if (devicePresent[addr]) {
      list[*count] = addr;
      (*count)++;
    }
  }
}
