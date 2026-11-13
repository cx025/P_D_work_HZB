#include "pressure_sensor.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PRESSURE_GPIO GPIO_NUM_0
#define PRESSURE_ACTIVE_LOW_DEFAULT true
#define SENSOR_DEBOUNCE_MS 30

static const char *TAG = "pressure_sensor";
static bool active_low = PRESSURE_ACTIVE_LOW_DEFAULT;
static bool candidate_pressed;
static bool stable_pressed;
static TickType_t candidate_since;

static bool get_sampled_state(void)
{
    const int raw = gpio_get_level(PRESSURE_GPIO);
    return active_low ? raw == 0 : raw != 0;
}

esp_err_t pressure_sensor_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << PRESSURE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d configuration failed: %s",
                 PRESSURE_GPIO, esp_err_to_name(err));
        return err;
    }

    stable_pressed = get_sampled_state();
    candidate_pressed = stable_pressed;
    candidate_since = xTaskGetTickCount();
    ESP_LOGI(TAG, "Pressure switch initialized on GPIO%d, active %s, initial raw=%d",
             PRESSURE_GPIO, active_low ? "LOW" : "HIGH",
             gpio_get_level(PRESSURE_GPIO));
    return ESP_OK;
}

bool pressure_sensor_update(void)
{
    const TickType_t now = xTaskGetTickCount();
    const bool sampled_pressed = get_sampled_state();

    if (sampled_pressed != candidate_pressed) {
        candidate_pressed = sampled_pressed;
        candidate_since = now;
    } else if (stable_pressed != candidate_pressed &&
               (now - candidate_since) >= pdMS_TO_TICKS(SENSOR_DEBOUNCE_MS)) {
        stable_pressed = candidate_pressed;
        ESP_LOGI(TAG, "Pressure switch %s",
                 stable_pressed ? "ACTIVE" : "released");
        return true;
    }
    return false;
}

bool pressure_sensor_is_pressed(void)
{
    return stable_pressed;
}

bool pressure_sensor_is_active_low(void)
{
    return active_low;
}

int pressure_sensor_get_raw(void)
{
    return gpio_get_level(PRESSURE_GPIO);
}

void pressure_sensor_log_state(void)
{
    ESP_LOGI(TAG, "GPIO%d raw=%d, active %s, stable %s",
             PRESSURE_GPIO, gpio_get_level(PRESSURE_GPIO),
             active_low ? "LOW" : "HIGH",
             stable_pressed ? "PRESSED" : "released");
}

void pressure_sensor_set_active_low(bool new_active_low)
{
    active_low = new_active_low;
    stable_pressed = get_sampled_state();
    candidate_pressed = stable_pressed;
    candidate_since = xTaskGetTickCount();
    ESP_LOGI(TAG, "Pressure switch polarity set to active %s",
             active_low ? "LOW" : "HIGH");
}
