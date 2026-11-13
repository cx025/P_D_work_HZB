#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DEFAULT_TEST_MS 300

#include "linear_motor.h"
#include "pressure_sensor.h"
#include "speaker.h"

static const char *TAG = "pressure_haptic";
static QueueHandle_t command_queue;
static bool automatic_mode = true;
static bool unavailable_warning_printed;
static bool speaker_warning_printed;

typedef enum {
    COMMAND_STATUS,
    COMMAND_AUTO,
    COMMAND_TEST,
    COMMAND_STOP,
    COMMAND_POLARITY,
} command_type_t;

typedef struct {
    command_type_t type;
    int value;
} command_t;

static void print_help(void)
{
    printf("\r\nCommands:\r\n"
           "  HELP                 Show commands\r\n"
           "  STATUS               Show sensor, motor and speaker status\r\n"
           "  AUTO ON|OFF          Enable or disable pressure-triggered output\r\n"
           "  MOTOR TEST [ms]      Run a timed motor test (default 300 ms)\r\n"
           "  MOTOR STOP           Stop the motor and disable automatic mode\r\n"
           "  POLARITY LOW|HIGH    Set the pressure switch's active level\r\n\r\n");
}

static void uppercase_ascii(char *text)
{
    for (; *text != '\0'; ++text) {
        if (*text >= 'a' && *text <= 'z') {
            *text = (char)(*text - ('a' - 'A'));
        }
    }
}

static bool queue_command(command_type_t type, int value)
{
    command_t command = {.type = type, .value = value};
    if (xQueueSend(command_queue, &command, 0) != pdTRUE) {
        printf("Command queue is full; try again.\r\n");
        return false;
    }
    return true;
}

void process_terminal_line(char *line)
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
        queue_command(COMMAND_STATUS, 0);
    } else if (strcmp(command, "AUTO") == 0) {
        char *value = strtok_r(NULL, " \t\r\n", &save_ptr);
        if (value != NULL) {
            uppercase_ascii(value);
        }
        if (value != NULL && strcmp(value, "ON") == 0) {
            queue_command(COMMAND_AUTO, 1);
        } else if (value != NULL && strcmp(value, "OFF") == 0) {
            queue_command(COMMAND_AUTO, 0);
        } else {
            printf("Usage: AUTO ON|OFF\r\n");
        }
    } else if (strcmp(command, "MOTOR") == 0) {
        char *action = strtok_r(NULL, " \t\r\n", &save_ptr);
        if (action != NULL) {
            uppercase_ascii(action);
        }
        if (action != NULL && strcmp(action, "TEST") == 0) {
            char *duration = strtok_r(NULL, " \t\r\n", &save_ptr);
            int milliseconds = duration == NULL
                                   ? DEFAULT_TEST_MS
                                   : (int)strtol(duration, NULL, 10);
            if (milliseconds < 50 || milliseconds > 10000) {
                printf("Test duration must be between 50 and 10000 ms.\r\n");
                return;
            }
            queue_command(COMMAND_TEST, milliseconds);
        } else if (action != NULL && strcmp(action, "STOP") == 0) {
            queue_command(COMMAND_STOP, 0);
        } else {
            printf("Usage: MOTOR TEST [ms] | MOTOR STOP\r\n");
        }
    } else if (strcmp(command, "POLARITY") == 0) {
        char *value = strtok_r(NULL, " \t\r\n", &save_ptr);
        if (value != NULL) {
            uppercase_ascii(value);
        }
        if (value != NULL && strcmp(value, "LOW") == 0) {
            queue_command(COMMAND_POLARITY, 1);
        } else if (value != NULL && strcmp(value, "HIGH") == 0) {
            queue_command(COMMAND_POLARITY, 0);
        } else {
            printf("Usage: POLARITY LOW|HIGH\r\n");
        }
    } else {
        printf("Unknown command. Type HELP.\r\n");
    }
}

static void terminal_task(void *arg)
{
    char line[96];
    (void)arg;

    while (fgets(line, sizeof(line), stdin) != NULL) {
        process_terminal_line(line);
    }
}

static void handle_command(const command_t *command, TickType_t now,
                           TickType_t *test_until)
{
    switch (command->type) {
    case COMMAND_STATUS: {
        printf("Pressure=%s (GPIO0 raw=%d), auto=%s, motor=%s, "
               "speaker=%s, AW8623DNR=%s\r\n",
               pressure_sensor_is_pressed() ? "ACTIVE" : "idle",
               pressure_sensor_get_raw(),
               automatic_mode ? "ON" : "OFF",
               linear_motor_is_running() ? "ON" : "OFF",
               speaker_is_enabled() ? "ON" : "OFF",
               linear_motor_is_ready() ? "ready" : "not detected");
        break;
    }
    case COMMAND_AUTO:
        automatic_mode = command->value != 0;
        printf("Automatic mode %s.\r\n", automatic_mode ? "enabled" : "disabled");
        break;
    case COMMAND_TEST:
        *test_until = now + pdMS_TO_TICKS(command->value);
        printf("Motor test started for %d ms.\r\n", command->value);
        break;
    case COMMAND_STOP:
        automatic_mode = false;
        *test_until = 0;
        if (linear_motor_is_ready()) {
            linear_motor_set_enabled(false);
        }
        printf("Motor stopped; automatic mode disabled.\r\n");
        break;
    case COMMAND_POLARITY:
        pressure_sensor_set_active_low(command->value != 0);
        printf("Pressure switch active level: %s.\r\n",
               pressure_sensor_is_active_low() ? "LOW" : "HIGH");
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(pressure_sensor_init());

    if (linear_motor_init() != ESP_OK) {
        ESP_LOGE(TAG, "Pressure monitoring will run, but motor output is disabled.");
    }
    if (speaker_init() != ESP_OK) {
        ESP_LOGE(TAG, "Pressure monitoring will run, but speaker output is disabled.");
    }

    command_queue = xQueueCreate(8, sizeof(command_t));
    if (command_queue == NULL) {
        ESP_LOGE(TAG, "Could not create command queue.");
        return;
    }
    if (xTaskCreate(terminal_task, "terminal", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start terminal task.");
        return;
    }
    printf("\r\nESP32-C3 pressure-to-haptic controller ready. Type HELP.\r\n");

    TickType_t test_until = 0;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        command_t command;
        while (xQueueReceive(command_queue, &command, 0) == pdTRUE) {
            handle_command(&command, now, &test_until);
        }

        pressure_sensor_update();

        bool test_active = test_until != 0 && (int32_t)(test_until - now) > 0;
        bool pressure_active = automatic_mode && pressure_sensor_is_pressed();
        bool should_run_motor =
            pressure_active || test_active;
        if (should_run_motor && !linear_motor_is_ready() &&
            !unavailable_warning_printed) {
            ESP_LOGE(TAG, "Motor request ignored because AW8623DNR was not detected.");
            unavailable_warning_printed = true;
        } else if (!should_run_motor) {
            unavailable_warning_printed = false;
        }
        if (pressure_active && !speaker_is_ready() &&
            !speaker_warning_printed) {
            ESP_LOGE(TAG, "Speaker request ignored because MAX98357A is not ready.");
            speaker_warning_printed = true;
        } else if (!pressure_active) {
            speaker_warning_printed = false;
        }
        if (speaker_is_ready() &&
            pressure_active != speaker_is_enabled()) {
            esp_err_t err = speaker_set_enabled(pressure_active);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not update speaker output: %s",
                         esp_err_to_name(err));
            }
        }
        if (linear_motor_is_ready() &&
            should_run_motor != linear_motor_is_running()) {
            esp_err_t err = linear_motor_set_enabled(should_run_motor);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not update motor output: %s",
                         esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
