#include "record_log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "record_log";

#define RECORD_LOG_MOUNT_POINT "/storage"
#define RECORD_LOG_FILE RECORD_LOG_MOUNT_POINT "/records.log"
#define RECORD_LOG_TEMP RECORD_LOG_MOUNT_POINT "/records.tmp"

#define RECORD_MAGIC 0x52454731UL /* "REG1" little-endian */
#define RECORD_ENTRY_SIZE 64
#define MAX_LOG_RECORDS 8192

#define ACK_NAMESPACE "recordlog"
#define ACK_KEY "acked_seq"

/* On-disk layout. Every field after crc is covered by the CRC so a torn write
 * at the end of the file fails validation and is discarded at boot. */
typedef struct {
    uint32_t magic;
    uint32_t crc;
    uint32_t seq;
    uint8_t type;
    uint8_t online;
    uint8_t reserved1;
    uint8_t reserved2;
    uint32_t epoch_seconds;
    uint32_t uptime_seconds;
    uint32_t duration_seconds;
    uint32_t reminders;
    uint8_t responded;
    uint8_t level;
    uint8_t reserved3;
    uint8_t reserved4;
    uint32_t session_seq;
    uint32_t reason;
    uint8_t padding[20];
} record_entry_t;

_Static_assert(sizeof(record_entry_t) == RECORD_ENTRY_SIZE,
               "record_entry_t must stay 64 bytes");

static uint32_t crc32_table[256];
static uint32_t s_count;
static uint32_t s_next_seq;
static uint32_t s_last_seq;
static uint32_t s_acked_seq;
static long s_read_offset;
static uint32_t s_read_after;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1U) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
}

static uint32_t crc32_compute(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < length; ++i) {
        crc = crc32_table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFUL;
}

static void entry_from_record(record_entry_t *entry, const record_t *record)
{
    memset(entry, 0, sizeof(*entry));
    entry->magic = RECORD_MAGIC;
    entry->seq = record->seq;
    entry->type = record->type;
    entry->online = record->online ? 1 : 0;
    entry->epoch_seconds = record->epoch_seconds;
    entry->uptime_seconds = record->uptime_seconds;
    entry->duration_seconds = record->duration_seconds;
    entry->reminders = record->reminders;
    entry->responded = record->responded ? 1 : 0;
    entry->level = record->level;
    entry->session_seq = record->session_seq;
    entry->reason = record->reason;
    entry->crc = crc32_compute((const uint8_t *)entry + 8, sizeof(*entry) - 8);
}

static void record_from_entry(record_t *record, const record_entry_t *entry)
{
    memset(record, 0, sizeof(*record));
    record->seq = entry->seq;
    record->type = entry->type;
    record->online = entry->online != 0;
    record->epoch_seconds = entry->epoch_seconds;
    record->uptime_seconds = entry->uptime_seconds;
    record->duration_seconds = entry->duration_seconds;
    record->reminders = entry->reminders;
    record->responded = entry->responded != 0;
    record->level = entry->level;
    record->session_seq = entry->session_seq;
    record->reason = entry->reason;
}

static bool entry_valid(const record_entry_t *entry)
{
    if (entry->magic != RECORD_MAGIC) return false;
    return entry->crc ==
           crc32_compute((const uint8_t *)entry + 8, sizeof(*entry) - 8);
}

static void load_acked_seq(void)
{
    s_acked_seq = 0;
    nvs_handle_t handle;
    if (nvs_open(ACK_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint32_t acked = 0;
    if (nvs_get_u32(handle, ACK_KEY, &acked) == ESP_OK) s_acked_seq = acked;
    nvs_close(handle);
}

esp_err_t record_log_init(void)
{
    crc32_init();

    const esp_vfs_littlefs_conf_t conf = {
        .base_path = RECORD_LOG_MOUNT_POINT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    load_acked_seq();

    s_count = 0;
    s_last_seq = 0;
    FILE *file = fopen(RECORD_LOG_FILE, "rb");
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        const long file_size = ftell(file);
        fseek(file, 0, SEEK_SET);

        record_entry_t entry;
        uint32_t expected_seq = 0;
        long good_end = 0;
        while (fread(&entry, sizeof(entry), 1, file) == 1) {
            if (!entry_valid(&entry) || entry.seq <= expected_seq) break;
            expected_seq = entry.seq;
            good_end = ftell(file);
        }
        fclose(file);

        if (good_end < file_size) {
            file = fopen(RECORD_LOG_FILE, "rb+");
            if (file != NULL) {
                if (ftruncate(fileno(file), good_end) != 0) {
                    ESP_LOGW(TAG, "Could not truncate torn log tail.");
                }
                fclose(file);
            }
            ESP_LOGW(TAG, "Dropped a torn/corrupt tail after %ld bytes.",
                     good_end);
        }
        s_count = (uint32_t)(good_end / sizeof(entry));
        s_last_seq = expected_seq;
    }

    s_next_seq = s_last_seq + 1;
    if (s_acked_seq > s_last_seq) s_acked_seq = s_last_seq;
    s_read_offset = 0;
    s_read_after = 0;
    ESP_LOGI(TAG, "Record log ready: %lu records, next seq=%lu, acked=%lu",
             (unsigned long)s_count, (unsigned long)s_next_seq,
             (unsigned long)s_acked_seq);
    return ESP_OK;
}

static esp_err_t record_log_compact(void)
{
    /* Drop records that have already been acknowledged. If nothing has been
     * synced yet, drop the oldest half so the log can keep going. */
    uint32_t keep_after = s_acked_seq;
    if (keep_after == 0 && s_last_seq > 1) keep_after = s_last_seq / 2;

    FILE *src = fopen(RECORD_LOG_FILE, "rb");
    FILE *dst = fopen(RECORD_LOG_TEMP, "wb");
    if (src == NULL || dst == NULL) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        ESP_LOGE(TAG, "Could not open log for compaction.");
        return ESP_FAIL;
    }

    uint32_t kept = 0;
    record_entry_t entry;
    while (fread(&entry, sizeof(entry), 1, src) == 1) {
        if (entry_valid(&entry) && entry.seq > keep_after) {
            if (fwrite(&entry, sizeof(entry), 1, dst) != 1) break;
            kept++;
        }
    }
    fflush(dst);
    fsync(fileno(dst));
    fclose(src);
    fclose(dst);

    remove(RECORD_LOG_FILE);
    if (rename(RECORD_LOG_TEMP, RECORD_LOG_FILE) != 0) {
        ESP_LOGE(TAG, "Could not finalize compacted log.");
        return ESP_FAIL;
    }

    s_count = kept;
    s_read_offset = 0;
    s_read_after = 0;
    ESP_LOGI(TAG, "Compacted log to %lu records.", (unsigned long)kept);
    return ESP_OK;
}

esp_err_t record_log_append(record_t *record)
{
    if (s_count >= MAX_LOG_RECORDS) {
        if (record_log_compact() != ESP_OK) {
            ESP_LOGE(TAG, "Log is full and could not be compacted.");
            return ESP_ERR_NO_MEM;
        }
    }

    record->seq = s_next_seq;
    record_entry_t entry;
    entry_from_record(&entry, record);

    FILE *file = fopen(RECORD_LOG_FILE, "ab");
    if (file == NULL) {
        ESP_LOGE(TAG, "Could not open log for append.");
        return ESP_FAIL;
    }
    if (fwrite(&entry, sizeof(entry), 1, file) != 1) {
        fclose(file);
        ESP_LOGE(TAG, "Could not append record.");
        return ESP_FAIL;
    }
    fflush(file);
    fsync(fileno(file));
    fclose(file);

    s_last_seq = record->seq;
    s_next_seq = record->seq + 1;
    s_count++;
    return ESP_OK;
}

uint32_t record_log_count(void)
{
    return s_count;
}

uint32_t record_log_last_seq(void)
{
    return s_last_seq;
}

uint32_t record_log_acked_seq(void)
{
    return s_acked_seq;
}

bool record_log_read_next(uint32_t after_seq, record_t *out)
{
    if (after_seq != s_read_after) {
        s_read_offset = 0;
        s_read_after = after_seq;
    }

    FILE *file = fopen(RECORD_LOG_FILE, "rb");
    if (file == NULL) return false;
    if (s_read_offset != 0) fseek(file, s_read_offset, SEEK_SET);

    record_entry_t entry;
    while (fread(&entry, sizeof(entry), 1, file) == 1) {
        if (entry_valid(&entry) && entry.seq > after_seq) {
            s_read_offset = ftell(file);
            fclose(file);
            record_from_entry(out, &entry);
            return true;
        }
    }
    fclose(file);
    return false;
}

esp_err_t record_log_ack(uint32_t seq)
{
    if (seq <= s_acked_seq) return ESP_OK;
    s_acked_seq = seq;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ACK_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(handle, ACK_KEY, seq);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not persist sync cursor: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t record_log_clear(void)
{
    FILE *file = fopen(RECORD_LOG_FILE, "wb");
    if (file != NULL) fclose(file);

    s_count = 0;
    s_last_seq = 0;
    s_next_seq = 1;
    s_read_offset = 0;
    s_read_after = 0;
    return record_log_ack(0);
}
