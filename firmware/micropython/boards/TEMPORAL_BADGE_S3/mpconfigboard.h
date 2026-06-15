#define MICROPY_HW_BOARD_NAME               "Temporal Badge ESP32-S3"
#define MICROPY_HW_MCU_NAME                 "ESP32S3"

// Native USB console is preferred on S3.
#define MICROPY_HW_ENABLE_USBDEV            (1)
#define MICROPY_HW_ENABLE_UART_REPL         (0)

// Network defaults.
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT "temporal-badge"

// Badge I2C defaults from CharlieDefines.h.
#define MICROPY_HW_I2C0_SCL                 (9)
#define MICROPY_HW_I2C0_SDA                 (8)
