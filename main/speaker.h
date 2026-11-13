#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "speaker_mp3.h"

esp_err_t speaker_init(void);
esp_err_t speaker_set_volume_percent(uint8_t volume_percent);
uint8_t speaker_get_volume_percent(void);
esp_err_t speaker_set_enabled(bool enabled);
esp_err_t speaker_play_song(void);
esp_err_t speaker_play_clip(speaker_mp3_clip_t clip);
esp_err_t speaker_stop(void);
bool speaker_is_ready(void);
bool speaker_is_enabled(void);
bool speaker_is_song_playing(void);
int64_t speaker_song_start_time_us(void);
