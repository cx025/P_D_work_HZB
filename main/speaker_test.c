#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "speaker.h"

#define DEFAULT_TEST_MS 1000
#define MIN_TEST_MS 50
#define MAX_TEST_MS 10000

static const char *TAG = "speaker_test";

static void print_help(void)
{
    printf("\r\nI2S speaker diagnostic commands:\r\n"
           "  HELP                 Show commands\r\n"
           "  STATUS               Show I2S and playback status\r\n"
           "  PLAY                 Play the embedded MP3 from the beginning\r\n"
           "  STOP                 Stop playback\r\n"
           "  TEST [ms]            Play a timed tone (default 1000 ms)\r\n"
           "  ON                   Start continuous tone\r\n"
           "  OFF                  Stop the tone\r\n\r\n");
}

static void print_status(void)
{
    printf("I2S speaker=%s, tone=%s, MP3=%s\r\n",
           speaker_is_ready() ? "ready" : "not ready",
           speaker_is_enabled() ? "ON" : "OFF",
           speaker_is_song_playing() ? "PLAYING" : "STOPPED");
}

static void uppercase_ascii(char *text)
{
    for (; *text != '\0'; ++text) {
        if (*text >= 'a' && *text <= 'z') {
            *text = (char)(*text - ('a' - 'A'));
        }
    }
}

static void test_speaker(int duration_ms)
{
    if (!speaker_is_ready()) {
        ESP_LOGE(TAG, "Test aborted: I2S speaker output is not ready.");
        return;
    }

    ESP_LOGI(TAG, "Playing test tone for %d ms.", duration_ms);
    esp_err_t err = speaker_set_enabled(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start speaker: %s", esp_err_to_name(err));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    err = speaker_set_enabled(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not stop speaker after test: %s",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Speaker test completed.");
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
    } else if (strcmp(command, "PLAY") == 0) {
        esp_err_t err = speaker_play_song();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not start MP3 playback: %s", esp_err_to_name(err));
        }
    } else if (strcmp(command, "STOP") == 0) {
        esp_err_t err = speaker_stop();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not stop speaker: %s", esp_err_to_name(err));
        }
    } else if (strcmp(command, "ON") == 0) {
        esp_err_t err = speaker_set_enabled(true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not start speaker: %s", esp_err_to_name(err));
        }
    } else if (strcmp(command, "OFF") == 0) {
        esp_err_t err = speaker_set_enabled(false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not stop speaker: %s", esp_err_to_name(err));
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
        test_speaker(duration_ms);
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
    ESP_LOGI(TAG, "Standalone NS4168 speaker MP3 playback");
    esp_err_t speaker_err = speaker_init();
    if (speaker_err != ESP_OK) {
        ESP_LOGE(TAG, "Speaker initialization failed: %s",
                 esp_err_to_name(speaker_err));
    } else {
        speaker_err = speaker_play_song();
        if (speaker_err != ESP_OK) {
            ESP_LOGE(TAG, "Could not start embedded MP3 playback: %s",
                     esp_err_to_name(speaker_err));
        }
    }

    print_status();
    print_help();
    if (xTaskCreate(terminal_task, "speaker_terminal", 4096, NULL, 5, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "Could not start speaker command task.");
    }
    printf("Enter a command in the serial monitor and press Enter.\r\n");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
