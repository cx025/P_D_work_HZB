#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t linear_motor_init(void);
esp_err_t linear_motor_set_enabled(bool enabled);
bool linear_motor_is_ready(void);
bool linear_motor_is_running(void);
