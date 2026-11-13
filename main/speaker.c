#include "speaker.h"

#include <inttypes.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "speaker_mp3.h"

#define NS4168_CTRL_GPIO GPIO_NUM_3
#define SPEAKER_BCLK_GPIO GPIO_NUM_6
#define SPEAKER_LRC_GPIO GPIO_NUM_7
#define SPEAKER_DIN_GPIO GPIO_NUM_10
#define SPEAKER_SAMPLE_RATE 16000
#define SPEAKER_BUFFER_FRAMES 128
#define SPEAKER_MP3_GAIN 1
#define NS4168_CTRL_OFF_US 120
#define NS4168_BOOT_MS 10

typedef enum {
    SPEAKER_MODE_IDLE,
    SPEAKER_MODE_TONE,
    SPEAKER_MODE_SONG,
} speaker_mode_t;

static const char *TAG = "speaker";
static i2s_chan_handle_t tx_channel;
static bool ready;
static speaker_mode_t mode;
static speaker_mp3_clip_t selected_clip;
static uint8_t volume_percent = 100;
static uint32_t song_generation;
static int64_t song_start_time_us;
static TaskHandle_t s_speaker_task;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;

static void get_state(speaker_mode_t *current_mode,
                      uint32_t *current_song_generation,
                      speaker_mp3_clip_t *current_clip)
{
    portENTER_CRITICAL(&state_lock);
    *current_mode = mode;
    *current_song_generation = song_generation;
    *current_clip = selected_clip;
    portEXIT_CRITICAL(&state_lock);
}

static esp_err_t set_sample_rate(uint32_t sample_rate)
{
    esp_err_t err = i2s_channel_disable(tx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not pause I2S before clock change: %s",
                 esp_err_to_name(err));
        return err;
    }

    const i2s_std_clk_config_t clock_config =
        I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    err = i2s_channel_reconfig_std_clock(tx_channel, &clock_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set I2S sample rate to %" PRIu32 " Hz: %s",
                 sample_rate, esp_err_to_name(err));
    }

    const esp_err_t enable_err = i2s_channel_enable(tx_channel);
    if (enable_err != ESP_OK) {
        ESP_LOGE(TAG, "Could not resume I2S after clock change: %s",
                 esp_err_to_name(enable_err));
        return enable_err;
    }
    return err;
}

/* Reconfigure the clock and start a channel that is currently disabled (parked
 * while idle). Reconfiguring requires the channel to be in READY (stopped)
 * state, so this cannot reuse set_sample_rate() which disables it first. */
static esp_err_t resume_channel(uint32_t sample_rate)
{
    const i2s_std_clk_config_t clock_config =
        I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    esp_err_t err = i2s_channel_reconfig_std_clock(tx_channel, &clock_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set I2S sample rate to %" PRIu32 " Hz: %s",
                 sample_rate, esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_enable(tx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not resume parked I2S channel: %s",
                 esp_err_to_name(err));
    }
    return err;
}

static void speaker_task(void *arg)
{
    static const int16_t tone[32] = {
        0,  1567,  3061,  4417,  5571,  6474,  7081,  7365,
        7365, 7081,  6474,  5571,  4417,  3061,  1567,     0,
        0, -1567, -3061, -4417, -5571, -6474, -7081, -7365,
        -7365, -7081, -6474, -5571, -4417, -3061, -1567,     0,
    };
    int16_t samples[1152 * 2];
    size_t tone_index = 0;
    speaker_mode_t active_mode = SPEAKER_MODE_IDLE;
    uint32_t active_song_generation = 0;
    uint32_t active_sample_rate = SPEAKER_SAMPLE_RATE;
    bool logged_first_pcm_frame = false;
    (void)arg;

    while (true) {
        speaker_mode_t requested_mode;
        uint32_t requested_song_generation;
        speaker_mp3_clip_t requested_clip;
        get_state(&requested_mode, &requested_song_generation,
                  &requested_clip);

        /* Nothing to play: park the I2S channel and block until playback is
         * requested, instead of streaming silence forever (I2S + DMA drain). */
        if (requested_mode == SPEAKER_MODE_IDLE) {
            if (active_mode == SPEAKER_MODE_SONG) {
                speaker_mp3_reset();
            }
            if (active_sample_rate != 0) {
                i2s_channel_disable(tx_channel);
                active_sample_rate = 0;
            }
            active_mode = SPEAKER_MODE_IDLE;
            tone_index = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        /* Wake-up: bring the channel back up at the default rate; the SONG
         * path reconfigures to the MP3's real rate on the first frame. */
        if (active_sample_rate == 0) {
            if (resume_channel(SPEAKER_SAMPLE_RATE) != ESP_OK) {
                portENTER_CRITICAL(&state_lock);
                mode = SPEAKER_MODE_IDLE;
                portEXIT_CRITICAL(&state_lock);
                active_mode = SPEAKER_MODE_IDLE;
                continue;
            }
            active_sample_rate = SPEAKER_SAMPLE_RATE;
        }

        const bool restart_song =
            requested_mode == SPEAKER_MODE_SONG &&
            requested_song_generation != active_song_generation;
        if (requested_mode != active_mode || restart_song) {
            if (active_mode == SPEAKER_MODE_SONG) {
                speaker_mp3_reset();
            }
            if (requested_mode == SPEAKER_MODE_SONG) {
                const int select_result =
                    speaker_mp3_select_clip(requested_clip);
                if (select_result != 0) {
                    ESP_LOGE(TAG, "Invalid MP3 clip selection: %d",
                             requested_clip);
                    portENTER_CRITICAL(&state_lock);
                    mode = SPEAKER_MODE_IDLE;
                    portEXIT_CRITICAL(&state_lock);
                    active_mode = SPEAKER_MODE_IDLE;
                    continue;
                }
                active_song_generation = requested_song_generation;
                logged_first_pcm_frame = false;
            } else if (active_sample_rate != SPEAKER_SAMPLE_RATE) {
                if (set_sample_rate(SPEAKER_SAMPLE_RATE) == ESP_OK) {
                    active_sample_rate = SPEAKER_SAMPLE_RATE;
                }
            }
            active_mode = requested_mode;
            tone_index = 0;
        }

        if (active_mode == SPEAKER_MODE_SONG) {
            size_t decoded_samples = 0;
            uint32_t sample_rate = 0;
            uint8_t channels = 0;
            const int result = speaker_mp3_decode_next(
                samples, sizeof(samples) / sizeof(samples[0]), &decoded_samples,
                &sample_rate, &channels);

            if (result == SPEAKER_MP3_FORMAT_CHANGED) {
                if (sample_rate == 0 || (channels != 1 && channels != 2)) {
                    ESP_LOGE(TAG, "Invalid MP3 format: %" PRIu32 " Hz, %u channels",
                             sample_rate, channels);
                    portENTER_CRITICAL(&state_lock);
                    mode = SPEAKER_MODE_IDLE;
                    portEXIT_CRITICAL(&state_lock);
                } else {
                    ESP_LOGI(TAG, "MP3 format: %" PRIu32 " Hz, %u channel(s)",
                             sample_rate, channels);
                    if (sample_rate != active_sample_rate) {
                        const esp_err_t err = set_sample_rate(sample_rate);
                        if (err != ESP_OK) {
                            portENTER_CRITICAL(&state_lock);
                            mode = SPEAKER_MODE_IDLE;
                            portEXIT_CRITICAL(&state_lock);
                        } else {
                            active_sample_rate = sample_rate;
                        }
                    }
                }
                continue;
            }
            if (result == SPEAKER_MP3_EOF) {
                ESP_LOGI(TAG, "MP3 clip playback completed.");
                portENTER_CRITICAL(&state_lock);
                mode = SPEAKER_MODE_IDLE;
                portEXIT_CRITICAL(&state_lock);
                continue;
            }
            if (result < 0) {
                ESP_LOGE(TAG, "MP3 decode failed: %d", result);
                portENTER_CRITICAL(&state_lock);
                mode = SPEAKER_MODE_IDLE;
                portEXIT_CRITICAL(&state_lock);
                continue;
            }
            if (decoded_samples == 0) {
                continue;
            }
            if (channels == 1) {
                for (size_t i = decoded_samples; i > 0; --i) {
                    const int16_t sample = samples[i - 1];
                    samples[(i - 1) * 2] = sample;
                    samples[(i - 1) * 2 + 1] = sample;
                }
            } else {
                for (size_t i = 0; i < decoded_samples; ++i) {
                    const int32_t left = samples[i * 2];
                    const int32_t right = samples[i * 2 + 1];
                    const int16_t mono = (int16_t)((left + right) / 2);
                    samples[i * 2] = mono;
                    samples[i * 2 + 1] = mono;
                }
            }

            if (!logged_first_pcm_frame) {
                int32_t peak = 0;
                for (size_t i = 0; i < decoded_samples * 2; ++i) {
                    const int32_t value = samples[i];
                    const int32_t magnitude = value < 0 ? -value : value;
                    if (magnitude > peak) {
                        peak = magnitude;
                    }
                }
                ESP_LOGI(TAG, "MP3 decoded: %u samples/channel, PCM peak=%" PRId32,
                         (unsigned)decoded_samples, peak);
                logged_first_pcm_frame = true;
            }

            portENTER_CRITICAL(&state_lock);
            const uint8_t current_volume = volume_percent;
            portEXIT_CRITICAL(&state_lock);
            for (size_t i = 0; i < decoded_samples * 2; ++i) {
                int32_t amplified =
                    (int32_t)samples[i] * SPEAKER_MP3_GAIN * current_volume /
                    100;
                if (amplified > INT16_MAX) {
                    amplified = INT16_MAX;
                } else if (amplified < INT16_MIN) {
                    amplified = INT16_MIN;
                }
                samples[i] = (int16_t)amplified;
            }

            const size_t output_bytes =
                decoded_samples * 2 * sizeof(samples[0]);
            size_t bytes_written = 0;
            portENTER_CRITICAL(&state_lock);
            const bool clip_is_current = mode == SPEAKER_MODE_SONG &&
                song_generation == active_song_generation;
            if (clip_is_current && song_start_time_us == 0) {
                song_start_time_us = esp_timer_get_time();
            }
            portEXIT_CRITICAL(&state_lock);
            if (!clip_is_current) continue;
            const esp_err_t err =
                i2s_channel_write(tx_channel, samples, output_bytes,
                                  &bytes_written, portMAX_DELAY);
            if (err != ESP_OK || bytes_written != output_bytes) {
                ESP_LOGE(TAG, "MP3 I2S write failed (%u/%u bytes): %s",
                         (unsigned)bytes_written, (unsigned)output_bytes,
                         esp_err_to_name(err));
                portENTER_CRITICAL(&state_lock);
                mode = SPEAKER_MODE_IDLE;
                portEXIT_CRITICAL(&state_lock);
            }
            continue;
        }

        const bool play_tone = active_mode == SPEAKER_MODE_TONE;
        for (size_t frame = 0; frame < SPEAKER_BUFFER_FRAMES; ++frame) {
            const int16_t sample = play_tone ? tone[tone_index] : 0;
            samples[frame * 2] = sample;
            samples[frame * 2 + 1] = sample;
            if (play_tone) {
                tone_index = (tone_index + 1) % (sizeof(tone) / sizeof(tone[0]));
            } else {
                tone_index = 0;
            }
        }

        const size_t output_bytes =
            SPEAKER_BUFFER_FRAMES * 2 * sizeof(samples[0]);
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(tx_channel, samples, output_bytes,
                                          &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S audio write failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (bytes_written != output_bytes) {
            ESP_LOGE(TAG, "I2S audio write was incomplete (%u/%u bytes)",
                     (unsigned)bytes_written, (unsigned)output_bytes);
        }
    }
}

esp_err_t speaker_init(void)
{
    const gpio_config_t ctrl_gpio_config = {
        .pin_bit_mask = 1ULL << NS4168_CTRL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&ctrl_gpio_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NS4168 CTRL GPIO configuration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(NS4168_CTRL_GPIO, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not hold NS4168 in shutdown: %s",
                 esp_err_to_name(err));
        return err;
    }
    esp_rom_delay_us(NS4168_CTRL_OFF_US);
    err = gpio_set_level(NS4168_CTRL_GPIO, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start NS4168: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(NS4168_BOOT_MS));

    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    err = i2s_new_channel(&channel_config, &tx_channel, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel creation failed: %s", esp_err_to_name(err));
        return err;
    }

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPEAKER_BCLK_GPIO,
            .ws = SPEAKER_LRC_GPIO,
            .dout = SPEAKER_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(tx_channel, &standard_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S standard mode setup failed: %s",
                 esp_err_to_name(err));
        i2s_del_channel(tx_channel);
        tx_channel = NULL;
        return err;
    }

    err = i2s_channel_enable(tx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(tx_channel);
        tx_channel = NULL;
        return err;
    }

    if (xTaskCreate(speaker_task, "speaker_audio", 8192, NULL, 5,
                    &s_speaker_task) != pdPASS) {
        ESP_LOGE(TAG, "Could not start I2S audio task.");
        i2s_channel_disable(tx_channel);
        i2s_del_channel(tx_channel);
        tx_channel = NULL;
        return ESP_FAIL;
    }

    ready = true;
    ESP_LOGI(TAG, "NS4168 and I2S audio ready "
                  "(CTRL=GPIO%d, BCLK=GPIO%d, LRC=GPIO%d, SDATA=GPIO%d)",
             NS4168_CTRL_GPIO,
             SPEAKER_BCLK_GPIO, SPEAKER_LRC_GPIO, SPEAKER_DIN_GPIO);
    return ESP_OK;
}

esp_err_t speaker_set_enabled(bool new_enabled)
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&state_lock);
    const bool changed = (mode == SPEAKER_MODE_TONE) != new_enabled;
    mode = new_enabled ? SPEAKER_MODE_TONE : SPEAKER_MODE_IDLE;
    portEXIT_CRITICAL(&state_lock);

    if (changed) {
        ESP_LOGI(TAG, "Speaker tone %s", new_enabled ? "ON" : "OFF");
    }
    if (changed && new_enabled && s_speaker_task != NULL) {
        xTaskNotifyGive(s_speaker_task);
    }
    return ESP_OK;
}

esp_err_t speaker_set_volume_percent(uint8_t new_volume_percent)
{
    if (new_volume_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&state_lock);
    volume_percent = new_volume_percent;
    portEXIT_CRITICAL(&state_lock);
    ESP_LOGI(TAG, "MP3 volume set to %u%%", new_volume_percent);
    return ESP_OK;
}

uint8_t speaker_get_volume_percent(void)
{
    uint8_t current;
    portENTER_CRITICAL(&state_lock);
    current = volume_percent;
    portEXIT_CRITICAL(&state_lock);
    return current;
}

esp_err_t speaker_play_song(void)
{
    return speaker_play_clip(SPEAKER_MP3_CLIP_SOFT);
}

esp_err_t speaker_play_clip(speaker_mp3_clip_t clip)
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (clip < SPEAKER_MP3_CLIP_SOFT || clip >= SPEAKER_MP3_CLIP_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&state_lock);
    selected_clip = clip;
    mode = SPEAKER_MODE_SONG;
    song_start_time_us = 0;
    ++song_generation;
    portEXIT_CRITICAL(&state_lock);
    if (s_speaker_task != NULL) {
        xTaskNotifyGive(s_speaker_task);
    }
    ESP_LOGI(TAG, "Starting embedded MP3 clip %d.", clip);
    return ESP_OK;
}

esp_err_t speaker_stop(void)
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&state_lock);
    mode = SPEAKER_MODE_IDLE;
    song_start_time_us = 0;
    portEXIT_CRITICAL(&state_lock);
    ESP_LOGI(TAG, "Speaker playback stopped.");
    return ESP_OK;
}

bool speaker_is_ready(void)
{
    return ready;
}

bool speaker_is_enabled(void)
{
    bool current;
    portENTER_CRITICAL(&state_lock);
    current = mode == SPEAKER_MODE_TONE;
    portEXIT_CRITICAL(&state_lock);
    return current;
}

bool speaker_is_song_playing(void)
{
    bool current;
    portENTER_CRITICAL(&state_lock);
    current = mode == SPEAKER_MODE_SONG;
    portEXIT_CRITICAL(&state_lock);
    return current;
}

int64_t speaker_song_start_time_us(void)
{
    int64_t start_time;
    portENTER_CRITICAL(&state_lock);
    start_time = song_start_time_us;
    portEXIT_CRITICAL(&state_lock);
    return start_time;
}
