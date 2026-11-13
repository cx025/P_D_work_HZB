#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "linear_motor.h"

#define DEFAULT_TEST_MS 100
#define MIN_TEST_MS 50
#define MAX_TEST_MS 10000

static const char *TAG = "motor_test";

static void print_help(void)
{
    printf("\r\nAW8623DNR motor diagnostic commands:\r\n"
           "  HELP                 Show this help\r\n"
           "  STATUS               Show driver and motor state\r\n"
           "  TEST [ms]            Run a timed test (default 100 ms)\r\n"
           "  ON                   Start continuous drive\r\n"
           "  OFF                  Stop the motor\r\n\r\n");
}

static void print_status(void)
{
    printf("AW8623DNR=%s, motor=%s\r\n",
           linear_motor_is_ready() ? "detected" : "not detected",
           linear_motor_is_running() ? "ON" : "OFF");
}

static void test_motor(int duration_ms)
{
    if (!linear_motor_is_ready()) {
        ESP_LOGE(TAG, "Test aborted: AW8623DNR was not detected during init.");
        return;
    }

    ESP_LOGI(TAG, "Starting motor test for %d ms.", duration_ms);
    esp_err_t err = linear_motor_set_enabled(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start motor: %s", esp_err_to_name(err));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    err = linear_motor_set_enabled(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not stop motor after test: %s",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Motor test command completed.");
}

static void uppercase_ascii(char *text)
{
    for (; *text != '\0'; ++text) {
        if (*text >= 'a' && *text <= 'z') {
            *text = (char)(*text - ('a' - 'A'));
        }
    }
}

static void process_command(char *line)
{
    char *save_ptr = NULL;
    char *command = strtok_r(line, " \t\r\n", &save_ptr);
    if (command == NULL) {
        return;
    }
    uppercase_ascii(command);

    if (strcmp(command, "HELP") == 0) {
        print_help();
    } else if (strcmp(command, "STATUS") == 0) {
        print_status();
    } else if (strcmp(command, "ON") == 0) {
        if (!linear_motor_is_ready()) {
            ESP_LOGE(TAG, "Cannot start: AW8623DNR is not ready.");
        } else {
            esp_err_t err = linear_motor_set_enabled(true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not start motor: %s",
                         esp_err_to_name(err));
            }
        }
    } else if (strcmp(command, "OFF") == 0) {
        if (linear_motor_is_ready()) {
            esp_err_t err = linear_motor_set_enabled(false);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not stop motor: %s",
                         esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Driver not detected; cannot send stop command.");
        }
    } else if (strcmp(command, "TEST") == 0) {
        char *duration_text = strtok_r(NULL, " \t\r\n", &save_ptr);
        int duration_ms = DEFAULT_TEST_MS;
        if (duration_text != NULL) {
            char *end = NULL;
            long parsed = strtol(duration_text, &end, 10);
            if (end == duration_text || *end != '\0' ||
                parsed < MIN_TEST_MS || parsed > MAX_TEST_MS) {
                printf("Test duration must be an integer from %d to %d ms.\r\n",
                       MIN_TEST_MS, MAX_TEST_MS);
                return;
            }
            duration_ms = (int)parsed;
        }
        test_motor(duration_ms);
    } else {
        printf("Unknown command. Type HELP.\r\n");
    }
}

static void terminal_task(void *arg)
{
    char line[64];
    (void)arg;

    while (true) {
        if (fgets(line, sizeof(line), stdin) != NULL) {
            process_command(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Standalone AW8623DNR 100 ms motor test");
    esp_err_t err = linear_motor_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Driver initialization failed: %s",
                 esp_err_to_name(err));
    }

    if (linear_motor_is_ready()) {
        test_motor(DEFAULT_TEST_MS);
    } else {
        ESP_LOGE(TAG, "Startup test skipped because the driver is not ready.");
    }

    print_status();
    print_help();
    if (xTaskCreate(terminal_task, "motor_terminal", 3072, NULL, 5, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "Could not start serial command task.");
    }
    printf("Enter a command and press Enter.\r\n");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
