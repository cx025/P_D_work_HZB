#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define DRV8833_CARRIER_HZ 20000U
#define DRV8833_MIN_CARRIER_HZ 5000U
#define DRV8833_MAX_CARRIER_HZ 30000U
#define DRV8833_VIBRATION_HZ 65U
#define DRV8833_MAX_VIBRATION_HZ 200U
#define DRV8833_UPDATE_US 500U
#define DRV8833_MAX_LEVEL 90U
#define DRV8833_MAX_TEST_MS 5000U
#define DRV8833_EFFECT_SHORT_MS 1000U
#define DRV8833_EFFECT_LONG_MS DRV8833_MAX_TEST_MS
// Measured/regulated VM must match this assumption. There is no voltage ADC.
#define DRV8833_ASSUMED_VM_MV 5000U
#define DRV8833_TARGET_RMS_MV 1200U
#define DRV8833_TARGET_FUNDAMENTAL_MV 800U

typedef enum {
    DRV8833_IDLE, DRV8833_RUNNING, DRV8833_MANUAL_STOP, DRV8833_FINISHED,
    DRV8833_FAULT_PIN, DRV8833_TIMING_FAULT, DRV8833_PWM_FAULT
} drv8833_reason_t;
typedef enum {
    DRV8833_EFFECT_WEAK,
    DRV8833_EFFECT_STRONG,
    DRV8833_EFFECT_LONG
} drv8833_effect_t;
typedef struct {
    bool ready, running, continuous;
    int fault_pin; // -1 means no physical fault feedback configured.
    drv8833_reason_t reason;
    uint32_t updates, remaining_ms, max_gap_us;
    uint32_t peak_ticks, model_vm_mv, model_pwm_rms_mv, model_fundamental_mv;
} drv8833_status_t;

esp_err_t drv8833_init(void);
esp_err_t drv8833_set_carrier_frequency(unsigned frequency_hz);
unsigned drv8833_get_carrier_frequency(void);
// duration_ms=0 runs continuously. Other durations: 100..5000ms.
// level (vibration amplitude) is the peak LOW-pulse percent in slow-decay
// mode, not volts/Vrms; vibration_hz is the vibration frequency (40..200 Hz).
esp_err_t drv8833_start(uint32_t duration_ms, unsigned level);
esp_err_t drv8833_start_with_frequency(uint32_t duration_ms, unsigned level,
                                       unsigned vibration_hz);
esp_err_t drv8833_start_effect(drv8833_effect_t effect);
// Ideal total PWM RMS estimate, NOT measured RMS or sine-wave fundamental RMS.
esp_err_t drv8833_start_estimated_rms(uint32_t duration_ms, unsigned rms_mv);
// Ideal low-frequency fundamental RMS, not the total PWM RMS or a measurement.
esp_err_t drv8833_start_fundamental(uint32_t duration_ms, unsigned rms_mv);
void drv8833_stop(void);
bool drv8833_clear_fault(void);
drv8833_status_t drv8833_status(void);
