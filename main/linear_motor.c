#include "linear_motor.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HAPTIC_RESET_GPIO GPIO_NUM_1
#define HAPTIC_SDA_GPIO GPIO_NUM_4
#define HAPTIC_SCL_GPIO GPIO_NUM_5
#define HAPTIC_I2C_ADDRESS 0x5A
#define I2C_FIRST_VALID_ADDRESS 0x08
#define I2C_LAST_VALID_ADDRESS 0x77
#define I2C_HALF_PERIOD_US 5
#define I2C_CLOCK_STRETCH_TIMEOUT_US 1000

#define AW8623_REG_CONTCFG1 0x17
#define AW8623_REG_CONTCFG2 0x18
#define AW8623_REG_CONTCFG3 0x19
#define AW8623_REG_CONTCFG6 0x1C
#define AW8623_REG_CONTCFG7 0x1D
#define AW8623_REG_CONTCFG8 0x1E
#define AW8623_REG_CONTCFG9 0x1F
#define AW8623_REG_PLAYCFG3 0x08
#define AW8623_REG_PLAYCFG4 0x09
#define AW8623_REG_SYSCTRL2 0x46
#define AW8623_REG_VBATCTRL 0x4E
#define AW8623_REG_IDH 0x57
#define AW8623_REG_IDL 0x69
#define AW8623_CHIP_IDH 0x23
#define AW8623_CHIP_IDL 0x40
#define AW8623_STANDBY_MASK 0x40
#define AW8623_RESONANCE_HZ 235

static const char *TAG = "linear_motor";
static bool ready;
static bool running;

static esp_err_t wait_for_scl_high(void)
{
    for (int elapsed_us = 0; elapsed_us < I2C_CLOCK_STRETCH_TIMEOUT_US;
         ++elapsed_us) {
        if (gpio_get_level(HAPTIC_SCL_GPIO) != 0) {
            return ESP_OK;
        }
        esp_rom_delay_us(1);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t i2c_start(bool repeated)
{
    esp_err_t err;
    if (repeated) {
        gpio_set_level(HAPTIC_SDA_GPIO, 1);
        esp_rom_delay_us(I2C_HALF_PERIOD_US);
    } else {
        gpio_set_level(HAPTIC_SDA_GPIO, 1);
        gpio_set_level(HAPTIC_SCL_GPIO, 1);
        err = wait_for_scl_high();
        if (err != ESP_OK) {
            return err;
        }
        if (gpio_get_level(HAPTIC_SDA_GPIO) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_rom_delay_us(I2C_HALF_PERIOD_US);
    }

    gpio_set_level(HAPTIC_SCL_GPIO, 1);
    err = wait_for_scl_high();
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(I2C_HALF_PERIOD_US);
    gpio_set_level(HAPTIC_SDA_GPIO, 0);
    esp_rom_delay_us(I2C_HALF_PERIOD_US);
    gpio_set_level(HAPTIC_SCL_GPIO, 0);
    return ESP_OK;
}

static esp_err_t i2c_stop(void)
{
    gpio_set_level(HAPTIC_SCL_GPIO, 0);
    gpio_set_level(HAPTIC_SDA_GPIO, 0);
    esp_rom_delay_us(I2C_HALF_PERIOD_US);
    gpio_set_level(HAPTIC_SCL_GPIO, 1);

    esp_err_t err = wait_for_scl_high();
    if (err != ESP_OK) {
        gpio_set_level(HAPTIC_SDA_GPIO, 1);
        return err;
    }
    esp_rom_delay_us(I2C_HALF_PERIOD_US);
    gpio_set_level(HAPTIC_SDA_GPIO, 1);
    esp_rom_delay_us(I2C_HALF_PERIOD_US);
    return ESP_OK;
}

static esp_err_t i2c_write_byte(uint8_t byte)
{
    for (int bit = 7; bit >= 0; --bit) {
        gpio_set_level(HAPTIC_SCL_GPIO, 0);
        gpio_set_level(HAPTIC_SDA_GPIO, (byte >> bit) & 1U);
        esp_rom_delay_us(I2C_HALF_PERIOD_US);
        gpio_set_level(HAPTIC_SCL_GPIO, 1);

        esp_err_t err = wait_for_scl_high();
        if (err != ESP_OK) {
            return err;
        }
        esp_rom_delay_us(I2C_HALF_PERIOD_US);
    }

    gpio_set_level(HAPTIC_SCL_GPIO, 0);
    gpio_set_level(HAPTIC_SDA_GPIO, 1);
    esp_rom_delay_us(I2C_HALF_PERIOD_US);
    gpio_set_level(HAPTIC_SCL_GPIO, 1);

    esp_err_t err = wait_for_scl_high();
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(I2C_HALF_PERIOD_US);
    const bool acknowledged = gpio_get_level(HAPTIC_SDA_GPIO) == 0;
    gpio_set_level(HAPTIC_SCL_GPIO, 0);
    return acknowledged ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t i2c_read_byte(uint8_t *byte, bool acknowledge)
{
    uint8_t value = 0;
    gpio_set_level(HAPTIC_SDA_GPIO, 1);

    for (int bit = 7; bit >= 0; --bit) {
        gpio_set_level(HAPTIC_SCL_GPIO, 0);
        esp_rom_delay_us(I2C_HALF_PERIOD_US);
        gpio_set_level(HAPTIC_SCL_GPIO, 1);

        esp_err_t err = wait_for_scl_high();
        if (err != ESP_OK) {
            return err;
        }
        esp_rom_delay_us(I2C_HALF_PERIOD_US);
        value = (uint8_t)((value << 1) |
                          (gpio_get_level(HAPTIC_SDA_GPIO) != 0));
    }

    gpio_set_level(HAPTIC_SCL_GPIO, 0);
    gpio_set_level(HAPTIC_SDA_GPIO, acknowledge ? 0 : 1);
    esp_rom_delay_us(I2C_HALF_PERIOD_US);
    gpio_set_level(HAPTIC_SCL_GPIO, 1);

    esp_err_t err = wait_for_scl_high();
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(I2C_HALF_PERIOD_US);
    gpio_set_level(HAPTIC_SCL_GPIO, 0);
    gpio_set_level(HAPTIC_SDA_GPIO, 1);
    *byte = value;
    return ESP_OK;
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    esp_err_t err = i2c_start(false);
    if (err == ESP_OK) {
        err = i2c_write_byte((HAPTIC_I2C_ADDRESS << 1) | 0U);
    }
    if (err == ESP_OK) {
        err = i2c_write_byte(reg);
    }
    if (err == ESP_OK) {
        err = i2c_write_byte(value);
    }

    const esp_err_t stop_err = i2c_stop();
    return err == ESP_OK ? stop_err : err;
}

static esp_err_t probe_address(uint8_t address)
{
    esp_err_t err = i2c_start(false);
    if (err == ESP_OK) {
        err = i2c_write_byte((address << 1) | 0U);
    }
    const esp_err_t stop_err = i2c_stop();
    return err == ESP_OK ? stop_err : err;
}

static esp_err_t read_register(uint8_t reg, uint8_t *value)
{
    esp_err_t err = i2c_start(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C START before register 0x%02X failed: %s",
                 reg, esp_err_to_name(err));
        return err;
    }
    err = i2c_write_byte((HAPTIC_I2C_ADDRESS << 1) | 0U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW8623 address 0x%02X write phase failed: %s",
                 HAPTIC_I2C_ADDRESS, esp_err_to_name(err));
        i2c_stop();
        return err;
    }
    err = i2c_write_byte(reg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW8623 NACK while selecting register 0x%02X: %s",
                 reg, esp_err_to_name(err));
        i2c_stop();
        return err;
    }
    err = i2c_start(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Repeated START for register 0x%02X failed: %s",
                 reg, esp_err_to_name(err));
        i2c_stop();
        return err;
    }
    err = i2c_write_byte((HAPTIC_I2C_ADDRESS << 1) | 1U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW8623 address 0x%02X read phase failed: %s",
                 HAPTIC_I2C_ADDRESS, esp_err_to_name(err));
        i2c_stop();
        return err;
    }
    err = i2c_read_byte(value, false);
    const esp_err_t stop_err = i2c_stop();
    return err == ESP_OK ? stop_err : err;
}

static esp_err_t update_register(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current;
    esp_err_t err = read_register(reg, &current);
    if (err != ESP_OK) {
        return err;
    }
    return write_register(reg, (current & (uint8_t)~mask) | (value & mask));
}

esp_err_t linear_motor_init(void)
{
    const gpio_config_t bus_gpio_config = {
        .pin_bit_mask = (1ULL << HAPTIC_SDA_GPIO) | (1ULL << HAPTIC_SCL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&bus_gpio_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Software I2C GPIO setup failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    gpio_set_level(HAPTIC_SDA_GPIO, 1);
    gpio_set_level(HAPTIC_SCL_GPIO, 1);

    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << HAPTIC_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&reset_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW8623 reset GPIO setup failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    gpio_set_level(HAPTIC_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(HAPTIC_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(HAPTIC_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    const int sda_idle_level = gpio_get_level(HAPTIC_SDA_GPIO);
    const int scl_idle_level = gpio_get_level(HAPTIC_SCL_GPIO);
    ESP_LOGI(TAG, "Software I2C idle levels: SDA(GPIO%d)=%d, SCL(GPIO%d)=%d",
             HAPTIC_SDA_GPIO, sda_idle_level, HAPTIC_SCL_GPIO,
             scl_idle_level);
    if (sda_idle_level == 0 || scl_idle_level == 0) {
        ESP_LOGE(TAG, "I2C bus is not idle; check SDA/SCL shorts and pull-ups.");
        return ESP_ERR_INVALID_STATE;
    }

    bool target_acknowledged = false;
    unsigned int acknowledged_count = 0;
    for (uint8_t address = I2C_FIRST_VALID_ADDRESS;
         address <= I2C_LAST_VALID_ADDRESS; ++address) {
        err = probe_address(address);
        if (err == ESP_OK) {
            ++acknowledged_count;
            ESP_LOGI(TAG, "Software I2C device ACK at address 0x%02X", address);
            if (address == HAPTIC_I2C_ADDRESS) {
                target_acknowledged = true;
            }
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Software I2C clock timeout probing 0x%02X",
                     address);
            return err;
        } else if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Software I2C probe at 0x%02X failed: %s",
                     address, esp_err_to_name(err));
            return err;
        }
    }
    if (!target_acknowledged) {
        ESP_LOGE(TAG,
                 "Address scan complete: %u device(s) ACK; AW8623 expected "
                 "at 0x%02X %s.",
                 acknowledged_count, HAPTIC_I2C_ADDRESS,
                 acknowledged_count == 0 ? "not found" : "did not ACK");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t chip_id_high;
    uint8_t chip_id_low;
    err = read_register(AW8623_REG_IDH, &chip_id_high);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Software I2C read of AW8623DNR IDH failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = read_register(AW8623_REG_IDL, &chip_id_low);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Software I2C read of AW8623DNR IDL failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    if (chip_id_high != AW8623_CHIP_IDH || chip_id_low != AW8623_CHIP_IDL) {
        ESP_LOGE(TAG,
                 "Unexpected haptic driver ID: 0x%02X 0x%02X "
                 "(expected 0x%02X 0x%02X)",
                 chip_id_high, chip_id_low, AW8623_CHIP_IDH, AW8623_CHIP_IDL);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint8_t configuration[][2] = {
        {AW8623_REG_CONTCFG1, 0x10},
        {AW8623_REG_CONTCFG2, 24000 / AW8623_RESONANCE_HZ},
        {AW8623_REG_CONTCFG3, 48000 / AW8623_RESONANCE_HZ},
        {AW8623_REG_CONTCFG6, 0xFF},
        {AW8623_REG_CONTCFG7, 0x7F},
        {AW8623_REG_PLAYCFG3, 0x06},
        {AW8623_REG_VBATCTRL, 0x40},
    };
    for (size_t i = 0; i < sizeof(configuration) / sizeof(configuration[0]); ++i) {
        err = write_register(configuration[i][0], configuration[i][1]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AW8623 setup failed at register 0x%02X: %s",
                     configuration[i][0], esp_err_to_name(err));
            return err;
        }
    }

    err = update_register(AW8623_REG_SYSCTRL2, AW8623_STANDBY_MASK,
                          AW8623_STANDBY_MASK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not put AW8623 into standby: %s",
                 esp_err_to_name(err));
        return err;
    }

    ready = true;
    ESP_LOGI(TAG,
             "AW8623DNR detected (ID 0x%02X 0x%02X), "
             "continuous mode configured for %d Hz",
             chip_id_high, chip_id_low, AW8623_RESONANCE_HZ);
    return ESP_OK;
}

esp_err_t linear_motor_set_enabled(bool enabled)
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;
    if (enabled) {
        err = update_register(AW8623_REG_SYSCTRL2, AW8623_STANDBY_MASK, 0x00);
        if (err == ESP_OK) {
            err = write_register(AW8623_REG_CONTCFG8, 0xFF);
        }
        if (err == ESP_OK) {
            err = write_register(AW8623_REG_CONTCFG9, 0xFF);
        }
        if (err == ESP_OK) {
            err = write_register(AW8623_REG_PLAYCFG4, 0x01);
        }
    } else {
        err = write_register(AW8623_REG_PLAYCFG4, 0x02);
        const esp_err_t standby_err =
            update_register(AW8623_REG_SYSCTRL2, AW8623_STANDBY_MASK,
                            AW8623_STANDBY_MASK);
        if (err == ESP_OK) {
            err = standby_err;
        }
    }

    if (err == ESP_OK) {
        running = enabled;
        ESP_LOGI(TAG, "AW8623DNR motor %s", enabled ? "ON" : "OFF");
    } else {
        ESP_LOGE(TAG, "AW8623DNR control failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool linear_motor_is_ready(void)
{
    return ready;
}

bool linear_motor_is_running(void)
{
    return running;
}
