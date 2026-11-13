#include "speaker_mp3.h"

#include <cstddef>
#include <cstdint>

#include "micro_mp3/mp3_decoder.h"

extern const uint8_t fart_soft_mp3_start[] asm("_binary_fart_soft_mp3_start");
extern const uint8_t fart_soft_mp3_end[] asm("_binary_fart_soft_mp3_end");
extern const uint8_t fart_blast_mp3_start[] asm("_binary_fart_blast_mp3_start");
extern const uint8_t fart_blast_mp3_end[] asm("_binary_fart_blast_mp3_end");
extern const uint8_t fart_awkward_mp3_start[] asm("_binary_fart_awkward_mp3_start");
extern const uint8_t fart_awkward_mp3_end[] asm("_binary_fart_awkward_mp3_end");

namespace {
micro_mp3::Mp3Decoder decoder;
size_t input_offset;
const uint8_t *clip_start = fart_soft_mp3_start;
const uint8_t *clip_end = fart_soft_mp3_end;
}

void speaker_mp3_reset(void)
{
    decoder.reset();
    input_offset = 0;
}

int speaker_mp3_select_clip(speaker_mp3_clip_t clip)
{
    switch (clip) {
    case SPEAKER_MP3_CLIP_SOFT:
        clip_start = fart_soft_mp3_start;
        clip_end = fart_soft_mp3_end;
        break;
    case SPEAKER_MP3_CLIP_BLAST:
        clip_start = fart_blast_mp3_start;
        clip_end = fart_blast_mp3_end;
        break;
    case SPEAKER_MP3_CLIP_AWKWARD:
        clip_start = fart_awkward_mp3_start;
        clip_end = fart_awkward_mp3_end;
        break;
    default:
        return -1;
    }
    speaker_mp3_reset();
    return 0;
}

int speaker_mp3_decode_next(int16_t *pcm_buffer, size_t buffer_samples,
                            size_t *samples_per_channel, uint32_t *sample_rate,
                            uint8_t *channels)
{
    if (pcm_buffer == nullptr || samples_per_channel == nullptr ||
        sample_rate == nullptr || channels == nullptr ||
        buffer_samples * sizeof(int16_t) <
            micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES) {
        return static_cast<int>(micro_mp3::MP3_INPUT_INVALID);
    }

    const size_t input_size = clip_end - clip_start;
    *samples_per_channel = 0;

    while (input_offset < input_size) {
        size_t bytes_consumed = 0;
        size_t samples_decoded = 0;
        const micro_mp3::Mp3Result result = decoder.decode(
            clip_start + input_offset, input_size - input_offset,
            reinterpret_cast<uint8_t *>(pcm_buffer),
            buffer_samples * sizeof(int16_t), bytes_consumed, samples_decoded);
        input_offset += bytes_consumed;

        if (result == micro_mp3::MP3_STREAM_INFO_READY ||
            result == micro_mp3::MP3_STREAM_INFO_CHANGED) {
            *sample_rate = decoder.get_sample_rate();
            *channels = decoder.get_channels();
            return SPEAKER_MP3_FORMAT_CHANGED;
        }
        if (result == micro_mp3::MP3_OK && samples_decoded > 0) {
            *samples_per_channel = samples_decoded;
            *sample_rate = decoder.get_sample_rate();
            *channels = decoder.get_channels();
            return SPEAKER_MP3_FRAME;
        }
        if (result == micro_mp3::MP3_DECODE_ERROR) {
            if (bytes_consumed == 0) {
                return static_cast<int>(result);
            }
            continue;
        }
        if (result == micro_mp3::MP3_NEED_MORE_DATA) {
            if (bytes_consumed == 0) {
                return input_offset == input_size
                           ? SPEAKER_MP3_EOF
                           : static_cast<int>(micro_mp3::MP3_DECODE_ERROR);
            }
            continue;
        }
        if (result < 0) {
            return static_cast<int>(result);
        }
    }

    return SPEAKER_MP3_EOF;
}
