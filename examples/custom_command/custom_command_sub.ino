/*
 * Custom Command Example - Sub Device
 *
 * This example shows how to handle custom commands on sub devices
 * Modify this to implement your own custom functionality
 */

// Use the base sub_esp32.ino and modify the processCommand() function

// Add these defines to your sub code:
#define CMD_SET_PWM 0x10
#define CMD_READ_SENSOR 0x11
#define CMD_SET_COLOR 0x12

// PWM pin
#define PWM_PIN 25

// RGB LED pins (if using RGB LED)
#define R_PIN 25
#define G_PIN 26
#define B_PIN 27

// Sensor pin (example: temperature sensor)
#define SENSOR_PIN 34

// Add to setup():
void setupCustom() {
  ledcSetup(0, 5000, 8);  // Channel 0, 5kHz, 8-bit
  ledcAttachPin(PWM_PIN, 0);

  pinMode(R_PIN, OUTPUT);
  pinMode(G_PIN, OUTPUT);
  pinMode(B_PIN, OUTPUT);

  pinMode(SENSOR_PIN, INPUT);
}

// Add these cases to processCommand() switch statement:
/*
case CMD_SET_PWM:
  if (currentMsg.dataLen >= 1) {
    uint8_t pwmValue = currentMsg.data[1];
    ledcWrite(0, pwmValue);
    DEBUG_UART.printf("    -> PWM set to %d\n", pwmValue);
  }
  break;

case CMD_READ_SENSOR:
  {
    uint16_t sensorValue = analogRead(SENSOR_PIN);
    DEBUG_UART.printf("    -> Sensor reading: %d\n", sensorValue);

    // Send response with sensor data
    uint8_t response[4];
    response[0] = MY_ADDRESS;
    response[1] = 2;  // Length
    response[2] = (sensorValue >> 8) & 0xFF;  // High byte
    response[3] = sensorValue & 0xFF;  // Low byte

    uint8_t checksum = 0;
    UART_IN.write(START_BYTE);
    for (int i = 0; i < 4; i++) {
      UART_IN.write(response[i]);
      checksum ^= response[i];
    }
    UART_IN.write(checksum);
  }
  break;

case CMD_SET_COLOR:
  if (currentMsg.dataLen >= 3) {
    uint8_t r = currentMsg.data[1];
    uint8_t g = currentMsg.data[2];
    uint8_t b = currentMsg.data[3];

    analogWrite(R_PIN, r);
    analogWrite(G_PIN, g);
    analogWrite(B_PIN, b);

    DEBUG_UART.printf("    -> Color set to R:%d G:%d B:%d\n", r, g, b);
  }
  break;
*/
