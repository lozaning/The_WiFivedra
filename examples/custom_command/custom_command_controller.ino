/*
 * Custom Command Example - Controller
 *
 * This example shows how to send custom commands with data
 * Use this to extend the protocol with your own commands
 */

#include "../../controller_esp32/controller_esp32.ino"

// Define custom commands (0x10 and above)
#define CMD_SET_PWM 0x10
#define CMD_READ_SENSOR 0x11
#define CMD_SET_COLOR 0x12

void setup() {
  DEBUG_UART.begin(115200);
  CONTROLLER_UART.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  delay(2000);
  DEBUG_UART.println("\n=== Custom Command Example ===");
}

void loop() {
  // Example 1: Set PWM value (0-255)
  DEBUG_UART.println("\n--- Setting PWM on device 1 ---");
  setPWM(1, 128);  // 50% duty cycle
  delay(2000);

  // Example 2: Request sensor reading
  DEBUG_UART.println("\n--- Reading sensor from device 2 ---");
  readSensor(2);
  delay(2000);

  // Example 3: Set RGB color
  DEBUG_UART.println("\n--- Setting color on device 3 ---");
  setColor(3, 255, 0, 128);  // Purple
  delay(2000);
}

/**
 * Send PWM value to a device
 */
void setPWM(uint8_t address, uint8_t value) {
  DEBUG_UART.printf("Setting PWM to %d on device %d\n", value, address);
  sendCommand(address, CMD_SET_PWM, &value, 1);
}

/**
 * Request sensor reading from a device
 */
void readSensor(uint8_t address) {
  DEBUG_UART.printf("Requesting sensor data from device %d\n", address);
  sendCommand(address, CMD_READ_SENSOR, NULL, 0);

  // Wait for response with sensor data
  if (waitForResponse(address, 500)) {
    // Response will be in the format: [CMD_READ_SENSOR][sensor_value_high][sensor_value_low]
    DEBUG_UART.println("Sensor data received");
  }
}

/**
 * Set RGB color on a device
 */
void setColor(uint8_t address, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t colorData[3] = {r, g, b};
  DEBUG_UART.printf("Setting color to R:%d G:%d B:%d on device %d\n", r, g, b, address);
  sendCommand(address, CMD_SET_COLOR, colorData, 3);
}
