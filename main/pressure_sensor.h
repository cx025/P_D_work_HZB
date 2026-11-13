#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t pressure_sensor_init(void);
bool pressure_sensor_update(void);
bool pressure_sensor_is_pressed(void);
bool pressure_sensor_is_active_low(void);
int pressure_sensor_get_raw(void);
void pressure_sensor_log_state(void);
void pressure_sensor_set_active_low(bool active_low);
