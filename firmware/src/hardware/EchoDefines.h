#ifndef ECHO_DEFINES_H
#define ECHO_DEFINES_H

#define JOY_Y 12
#define JOY_X 13

#define BUTTON_RIGHT 18
#define BUTTON_DOWN  17
#define BUTTON_LEFT  0 //this is marked as X on schematic. You need to move the 0 ohm resistor to the other position.
#define BUTTON_UP    7

#define SDA_PIN 4
#define SCL_PIN 5

#define IR_RX_PIN 1 // TSOP75338
#define IR_TX_PIN 2

#define MCU_SLEEP_PIN 10
#define INT_PWR_PIN 13
#define INT_GP_PIN 3  // For the LIS2DH12TR accelerometer (interrupt / SA0 per schematic) Also SYSOFF for BQ24079
#define ACCEL_INT_PIN 3
#define TILT_PIN 3

#define SYSOFF_PIN 10

#define BATT_VOLTAGE_PIN 8 // VBATT - 150K - BATT_VOLTAGE_PIN - 150K - GND
#define CHG_GOOD_PIN 14
#define CHG_STAT_PIN 21
#define CE_PIN 11

#define NAPTIME_LIGHT_SLEEP_AFTER_NO_MOTION_MS 30000UL
#define NAPTIME_DEEP_SLEEP_AFTER_NO_MOTION_MS 300000UL
#define NAPTIME_LIGHT_SLEEP_POLL_MS 1000UL

#define MOTOR_PIN 6

#define LIS2DH12_I2C_ADDRESS 0x19

#define LED_MATRIX_INTB_PIN 38    //IS31FL3731 LED Matrix driver
#define LED_MATRIX_AUDIO_PIN 38
#define LED_MATRIX_ENABLE_PIN 9

#define LED_MATRIX_WIDTH 8
#define LED_MATRIX_HEIGHT 8
#define LED_MATRIX_I2C_ADDRESS 0x74

#define LCD_RES_PIN 42
#define LCD_INVERTED 0

#define OLED_I2C_ADDRESS 0x3C

#define OLED_WIDTH 128
#define OLED_HEIGHT 64


// ── Per-peripheral feature flags ─────────────────────────────────────────────
// Code includes/excludes hardware drivers based on these flags.
// Add matching flags to any new hardware config file to enable the peripheral.
#define BADGE_HAS_IMU           // LIS2DH12 accelerometer
#define BADGE_HAS_LED_MATRIX    // IS31FL3731 8×8 LED matrix
#define BADGE_HAS_HAPTICS       // Vibration motor
#define BADGE_HAS_BATTERY_GAUGE // ADC voltage divider on BATT_VOLTAGE_PIN
#define BADGE_HAS_SLEEP_SERVICE // Motion-based sleep (requires IMU)

#define BADGE_ECHO


#endif
