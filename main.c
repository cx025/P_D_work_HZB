#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RTC_I2C_ADDRESS 0x51
#define RTC_SDA_GPIO GPIO_NUM_20
#define RTC_SCL_GPIO GPIO_NUM_21
#define RTC_TIME_REGISTER 0x02
#define TIME_LINE_LENGTH 64

static const char *TAG = "rtc_time";
static i2c_master_dev_handle_t s_rtc_device;

static esp_err_t read_rtc_time(struct tm *timeinfo, time_t *epoch)
{
    const uint8_t reg = RTC_TIME_REGISTER;
    uint8_t data[7];
    esp_err_t err = i2c_master_transmit_receive(
        s_rtc_device, &reg, sizeof(reg), data, sizeof(data), 1000);
    if (err != ESP_OK) {
        return err;
    }

    if ((data[0] & 0x80) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t bcd_values[] = {
        (uint8_t)(data[0] & 0x7f),
        (uint8_t)(data[1] & 0x7f),
        (uint8_t)(data[2] & 0x3f),
        (uint8_t)(data[3] & 0x3f),
        (uint8_t)(data[5] & 0x1f),
        data[6],
    };
    int values[6];
    for (size_t i = 0; i < sizeof(bcd_values); ++i) {
        uint8_t value = bcd_values[i];
        if ((value & 0x0f) > 9 || ((value >> 4) & 0x0f) > 9) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        values[i] = ((value >> 4) * 10) + (value & 0x0f);
    }

    if ((data[4] & 0x07) > 6) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    int year = ((data[5] & 0x80) != 0 ? 1900 : 2000) + values[5];
    *timeinfo = (struct tm) {
        .tm_year = year - 1900,
        .tm_mon = values[4] - 1,
        .tm_mday = values[3],
        .tm_hour = values[2],
        .tm_min = values[1],
        .tm_sec = values[0],
        .tm_isdst = -1,
    };

    time_t converted = mktime(timeinfo);
    struct tm normalized;
    if (converted == (time_t)-1 ||
        localtime_r(&converted, &normalized) == NULL ||
        normalized.tm_year != year - 1900 ||
        normalized.tm_mon != values[4] - 1 ||
        normalized.tm_mday != values[3] ||
        normalized.tm_hour != values[2] ||
        normalized.tm_min != values[1] ||
        normalized.tm_sec != values[0]) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *timeinfo = normalized;
    *epoch = converted;
    return ESP_OK;
}

static esp_err_t write_rtc_time(const struct tm *timeinfo)
{
    int year = timeinfo->tm_year + 1900;
    if (year < 1900 || year > 2099) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t stop_clock[] = {0x00, 0x20};
    esp_err_t err = i2c_master_transmit(
        s_rtc_device, stop_clock, sizeof(stop_clock), 1000);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t data[] = {
        RTC_TIME_REGISTER,
        (uint8_t)(((timeinfo->tm_sec / 10) << 4) | (timeinfo->tm_sec % 10)),
        (uint8_t)(((timeinfo->tm_min / 10) << 4) | (timeinfo->tm_min % 10)),
        (uint8_t)(((timeinfo->tm_hour / 10) << 4) | (timeinfo->tm_hour % 10)),
        (uint8_t)(((timeinfo->tm_mday / 10) << 4) | (timeinfo->tm_mday % 10)),
        (uint8_t)timeinfo->tm_wday,
        (uint8_t)((((timeinfo->tm_mon + 1) / 10) << 4) |
                  ((timeinfo->tm_mon + 1) % 10)),
        (uint8_t)((((year % 100) / 10) << 4) | (year % 10)),
    };
    if (year < 2000) {
        data[6] |= 0x80;
    }

    err = i2c_master_transmit(s_rtc_device, data, sizeof(data), 1000);
    const uint8_t start_clock[] = {0x00, 0x00};
    esp_err_t start_err = i2c_master_transmit(
        s_rtc_device, start_clock, sizeof(start_clock), 1000);
    return err != ESP_OK ? err : start_err;
}

static bool parse_beijing_time(char *line, struct tm *timeinfo, time_t *epoch)
{
    size_t length = strlen(line);
    while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n')) {
        line[--length] = '\0';
    }
    if (length != 19 || line[4] != '-' || line[7] != '-' ||
        line[10] != ' ' || line[13] != ':' || line[16] != ':') {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) {
            continue;
        }
        if (line[i] < '0' || line[i] > '9') {
            return false;
        }
    }

    int year, month, day, hour, minute, second;
    if (sscanf(line, "%4d-%2d-%2d %2d:%2d:%2d",
               &year, &month, &day, &hour, &minute, &second) != 6 ||
        year < 1900 || year > 2099) {
        return false;
    }

    struct tm requested = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
        .tm_isdst = -1,
    };
    time_t converted = mktime(&requested);
    struct tm normalized;
    if (converted == (time_t)-1 ||
        localtime_r(&converted, &normalized) == NULL ||
        normalized.tm_year != year - 1900 ||
        normalized.tm_mon != month - 1 ||
        normalized.tm_mday != day ||
        normalized.tm_hour != hour ||
        normalized.tm_min != minute ||
        normalized.tm_sec != second) {
        return false;
    }

    *timeinfo = normalized;
    *epoch = converted;
    return true;
}

static void usb_time_sync_task(void *arg)
{
    (void)arg;
    char line[TIME_LINE_LENGTH];

    ESP_LOGI(TAG, "Send Beijing time over USB as YYYY-MM-DD HH:MM:SS to set the RTC");
    while (fgets(line, sizeof(line), stdin) != NULL) {
        ESP_LOGI(TAG, "Received time over USB: %s", line);
        struct tm timeinfo;
        time_t epoch;
        if (!parse_beijing_time(line, &timeinfo, &epoch)) {
            ESP_LOGW(TAG, "Invalid time; expected YYYY-MM-DD HH:MM:SS");
            continue;
        }

        struct timeval tv = {
            .tv_sec = epoch,
            .tv_usec = 0,
        };
        if (settimeofday(&tv, NULL) != 0) {
            ESP_LOGE(TAG, "Failed to set system time");
            continue;
        }

        esp_err_t err = write_rtc_time(&timeinfo);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "System time set, but failed to save RTC: %s",
                     esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "Time synchronized and saved to PCF8563");
        break;
    }

    if (feof(stdin)) {
        ESP_LOGE(TAG, "USB serial input closed before RTC synchronization");
    }
    vTaskDelete(NULL);
}

static esp_err_t init_rtc(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = RTC_SDA_GPIO,
        .scl_io_num = RTC_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_master_probe(bus, RTC_I2C_ADDRESS, 1000);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2C probe: PCF8563 ACK at address 0x%02X",
                 RTC_I2C_ADDRESS);
    } else {
        ESP_LOGE(TAG, "I2C probe: no ACK at address 0x%02X (%s)",
                 RTC_I2C_ADDRESS, esp_err_to_name(err));
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus, &device_config, &s_rtc_device);
}

void app_main(void)
{
    if (setenv("TZ", "CST-8", 1) != 0) {
        ESP_LOGE(TAG, "Failed to set timezone to China Standard Time");
        return;
    }
    tzset();

    esp_err_t err = init_rtc();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize PCF8563 on I2C: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PCF8563 I2C ready (SDA GPIO%d, SCL GPIO%d)",
                 RTC_SDA_GPIO, RTC_SCL_GPIO);
        struct tm timeinfo;
        time_t epoch;
        err = read_rtc_time(&timeinfo, &epoch);
        if (err == ESP_OK) {
            struct timeval tv = {
                .tv_sec = epoch,
                .tv_usec = 0,
            };
            if (settimeofday(&tv, NULL) == 0) {
                ESP_LOGI(TAG, "System time restored from PCF8563");
            } else {
                ESP_LOGE(TAG, "Failed to restore system time from PCF8563");
            }
        } else {
            ESP_LOGW(TAG, "RTC time is unavailable or invalid (%s); sync from PC",
                     esp_err_to_name(err));
        }
    }

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_config =
            USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        err = usb_serial_jtag_driver_install(&usb_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install USB Serial/JTAG driver: %s",
                     esp_err_to_name(err));
        }
    }

    if (usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_vfs_use_driver();
        if (xTaskCreate(usb_time_sync_task, "usb_time_sync", 4096,
                        NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create USB time sync task");
        }
    }

    while (1) {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) != 0) {
            ESP_LOGE(TAG, "Failed to read system time");
        } else if (tv.tv_sec < 1577836800) {
            ESP_LOGI(TAG, "Waiting for valid RTC time or PC synchronization");
        } else {
            time_t now = tv.tv_sec;
            struct tm timeinfo;
            if (localtime_r(&now, &timeinfo) == NULL) {
                ESP_LOGE(TAG, "Failed to convert system time");
            } else {
                char time_string[32];
                if (strftime(time_string, sizeof(time_string),
                             "%Y-%m-%d %H:%M:%S", &timeinfo) == 0) {
                    ESP_LOGE(TAG, "Failed to format system time");
                } else {
                    ESP_LOGI(TAG, "Current time: %s.%06ld",
                             time_string, (long)tv.tv_usec);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
