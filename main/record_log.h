#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Offline usage history is kept as an append-only, fixed-size record log in a
 * LittleFS partition. Each record carries its own magic and CRC32 so that a
 * torn tail from a power cut can be detected and dropped on the next boot. */

typedef enum {
    RECORD_SESSION_START = 1, /* a sitting session became active */
    RECORD_SESSION_END = 2,   /* a session ended after an effective leave */
    RECORD_REMINDER = 3,      /* a reminder clip actually played */
    RECORD_INTERRUPT = 4,     /* the device lost power/reset mid-session */
} record_type_t;

typedef struct {
    uint32_t seq;              /* monotonic id, assigned on append */
    uint8_t type;              /* record_type_t */
    uint8_t online;            /* 1 when BLE was connected at record time */
    uint32_t epoch_seconds;    /* wall-clock seconds, 0 when clock unknown */
    uint32_t uptime_seconds;   /* device uptime at the event */
    uint32_t duration_seconds; /* session end / interrupted partial duration */
    uint32_t reminders;        /* session end: reminder count */
    uint8_t responded;         /* session end: 1 if a reminder played */
    uint8_t level;             /* reminder: 1..3 */
    uint32_t session_seq;      /* reminder/interrupt: owning session start seq */
    uint32_t reason;           /* interrupt: esp_reset_reason() code */
} record_t;

/* Mounts LittleFS, opens the log, drops any torn tail, and recovers the next
 * sequence number and the persisted sync cursor. Call after nvs_flash_init(). */
esp_err_t record_log_init(void);

/* Appends a record and assigns record->seq. Flushes to flash before returning. */
esp_err_t record_log_append(record_t *record);

/* Number of valid records currently on disk. */
uint32_t record_log_count(void);

/* Highest valid sequence on disk (0 when empty). */
uint32_t record_log_last_seq(void);

/* Highest sequence acknowledged by the phone, persisted in NVS. */
uint32_t record_log_acked_seq(void);

/* First valid record with seq > after_seq, in ascending order. Iterate by
 * passing the returned seq back as after_seq; returns false at the end. */
bool record_log_read_next(uint32_t after_seq, record_t *out);

/* Persists the sync cursor. Returns ESP_OK on success. */
esp_err_t record_log_ack(uint32_t seq);

/* Erases every record and resets the sequence and cursor. */
esp_err_t record_log_clear(void);
