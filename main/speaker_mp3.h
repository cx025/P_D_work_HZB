#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPEAKER_MP3_FRAME 0
#define SPEAKER_MP3_FORMAT_CHANGED 1
#define SPEAKER_MP3_EOF 2

typedef enum {
    SPEAKER_MP3_CLIP_SOFT,
    SPEAKER_MP3_CLIP_BLAST,
    SPEAKER_MP3_CLIP_AWKWARD,
    SPEAKER_MP3_CLIP_COUNT,
} speaker_mp3_clip_t;

void speaker_mp3_reset(void);
int speaker_mp3_select_clip(speaker_mp3_clip_t clip);
int speaker_mp3_decode_next(int16_t *pcm_buffer, size_t buffer_samples,
                            size_t *samples_per_channel, uint32_t *sample_rate,
                            uint8_t *channels);

#ifdef __cplusplus
}
#endif
