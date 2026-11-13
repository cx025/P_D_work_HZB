#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t ble_control_init(void);
bool ble_control_is_connected(void);
void ble_control_update_audio_status(bool pressure, uint8_t volume_percent,
                                     uint32_t delay_1_ms,
                                     uint32_t delay_2_ms,
                                     uint32_t delay_3_ms, bool saved,
                                     bool reminders_enabled,
                                     bool reminders_saved,
                                     bool session_active,
                                     bool leave_confirming,
                                     uint32_t session_seconds,
                                     uint32_t today_sitting_seconds,
                                     uint32_t today_sessions,
                                     uint32_t today_reminders,
                                     uint32_t last_session_seconds,
                                     uint32_t last_session_reminders,
                                     bool last_session_responded,
                                     bool clock_synced,
                                     uint16_t blast_frequency_hz,
                                     uint16_t motor_frequency_hz,
                                     uint8_t blast_amplitude,
                                     uint8_t motor_amplitude,
                                     uint32_t pwm_carrier_hz,
                                     bool motor_settings_saved,
                                     bool vibration_level_1_active,
                                     bool vibration_level_2_active,
                                     bool motor_direct_running);
void ble_control_notify_history(const char *json);
esp_err_t process_terminal_line(char *line);
