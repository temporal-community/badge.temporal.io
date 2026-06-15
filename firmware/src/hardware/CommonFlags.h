#ifndef ULP_FLAGS_H
#define ULP_FLAGS_H

// Shared between the main CPU's BatteryGauge and the ULP-RISC-V program.
// Pin numbers are pulled in via HardwareConfig.h, which selects the
// appropriate hardware-specific definitions (for example Echo/Delta/Charlie)
// based on the configured HARDWARE_* target so both sides agree.
#include "HardwareConfig.h"

#define BAT_IIR_SHIFT 3

// Bit positions inside the shared `flags` word in RTC slow memory.
#define FLAG_PGOOD        (0)
#define FLAG_STAT_RAW     (1)
#define FLAG_STAT_LOW     (2)
#define FLAG_BAT_PRESENT  (3)
#define FLAG_CHARGING     (4)
#define FLAG_USB_PRESENT  (5)
#define FLAG_FILTER_INIT  (6)
#define FLAG_READY        (7)
#define FLAG_CPU_RUNNING  (8)

#define MASK_PGOOD        (1U << FLAG_PGOOD)
#define MASK_STAT_RAW     (1U << FLAG_STAT_RAW)
#define MASK_STAT_LOW     (1U << FLAG_STAT_LOW)
#define MASK_BAT_PRESENT  (1U << FLAG_BAT_PRESENT)
#define MASK_CHARGING     (1U << FLAG_CHARGING)
#define MASK_USB_PRESENT  (1U << FLAG_USB_PRESENT)
#define MASK_FILTER_INIT  (1U << FLAG_FILTER_INIT)
#define MASK_READY        (1U << FLAG_READY)
#define MASK_CPU_RUNNING  (1U << FLAG_CPU_RUNNING)

#endif  // ULP_FLAGS_H
