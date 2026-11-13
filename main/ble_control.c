#include "ble_control.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "drv8833.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_control";
static const char *DEVICE_NAME = "ESP32C3-Haptic";
static uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t status_value_handle;
static uint16_t history_value_handle;
static uint8_t own_address_type;
static char framed_command[96];
static size_t framed_command_length;
static bool receiving_framed_command;
static bool audio_pressure;
static uint8_t audio_volume_percent = 70;
static uint32_t audio_delay_1_ms = 1800000;
static uint32_t audio_delay_2_ms = 3600000;
static uint32_t audio_delay_3_ms = 5400000;
static bool audio_settings_saved = true;
static bool audio_reminders_enabled = true;
static bool audio_reminders_saved = true;
static bool audio_session_active;
static bool audio_leave_confirming;
static uint32_t audio_session_seconds;
static uint32_t audio_today_sitting_seconds;
static uint32_t audio_today_sessions;
static uint32_t audio_today_reminders;
static uint32_t audio_last_session_seconds;
static uint32_t audio_last_session_reminders;
static bool audio_last_session_responded;
static bool audio_clock_synced;
static uint16_t audio_blast_frequency_hz = 60;
static uint16_t audio_motor_frequency_hz = 160;
static uint8_t audio_blast_amplitude = 70;
static uint8_t audio_motor_amplitude = 90;
static uint32_t audio_pwm_carrier_hz = DRV8833_CARRIER_HZ;
static bool audio_motor_settings_saved = true;
static bool audio_vibration_level_1_active;
static bool audio_vibration_level_2_active;
static bool audio_motor_direct_running;
static portMUX_TYPE audio_status_lock = portMUX_INITIALIZER_UNLOCKED;

static const ble_uuid128_t service_uuid =
    BLE_UUID128_INIT(0x8d, 0x53, 0x5a, 0x90, 0x7b, 0x62, 0x4d, 0x5e,
                     0xa4, 0x62, 0x44, 0x3e, 0x3e, 0x58, 0x00, 0x01);
static const ble_uuid128_t command_uuid =
    BLE_UUID128_INIT(0x8d, 0x53, 0x5a, 0x90, 0x7b, 0x62, 0x4d, 0x5e,
                     0xa4, 0x62, 0x44, 0x3e, 0x3e, 0x58, 0x00, 0x02);
static const ble_uuid128_t status_uuid =
    BLE_UUID128_INIT(0x8d, 0x53, 0x5a, 0x90, 0x7b, 0x62, 0x4d, 0x5e,
                     0xa4, 0x62, 0x44, 0x3e, 0x3e, 0x58, 0x00, 0x03);
static const ble_uuid128_t history_uuid =
    BLE_UUID128_INIT(0x8d, 0x53, 0x5a, 0x90, 0x7b, 0x62, 0x4d, 0x5e,
                     0xa4, 0x62, 0x44, 0x3e, 0x3e, 0x58, 0x00, 0x04);

static int gap_event(struct ble_gap_event *event, void *arg);

static int map_command_error(esp_err_t err)
{
    if (err == ESP_OK) return 0;
    if (err == ESP_ERR_INVALID_ARG) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    if (err == ESP_ERR_NO_MEM || err == ESP_ERR_TIMEOUT) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static void reset_framed_command(void)
{
    framed_command_length = 0;
    receiving_framed_command = false;
}

static int command_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    char packet[96];
    if (length == 0 || length >= sizeof(packet)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (os_mbuf_copydata(ctxt->om, 0, length, packet) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (receiving_framed_command || packet[0] == '@') {
        size_t offset = 0;
        if (!receiving_framed_command) {
            receiving_framed_command = true;
            offset = 1;
        }
        for (; offset < length; ++offset) {
            const char character = packet[offset];
            if (character == '\r') continue;
            if (character == '\n') {
                if (framed_command_length == 0) {
                    reset_framed_command();
                    return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
                }
                framed_command[framed_command_length] = '\0';
                const esp_err_t err = process_terminal_line(framed_command);
                reset_framed_command();
                return map_command_error(err);
            }
            if (framed_command_length + 1 >= sizeof(framed_command)) {
                reset_framed_command();
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            framed_command[framed_command_length++] = character;
        }
        return 0;
    }

    packet[length] = '\0';
    return map_command_error(process_terminal_line(packet));
}

static int status_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    char status[256];
    portENTER_CRITICAL(&audio_status_lock);
    const bool pressure = audio_pressure;
    const uint8_t volume = audio_volume_percent;
    const uint32_t delay_1 = audio_delay_1_ms;
    const uint32_t delay_2 = audio_delay_2_ms;
    const uint32_t delay_3 = audio_delay_3_ms;
    const bool saved = audio_settings_saved;
    const bool reminders_enabled = audio_reminders_enabled;
    const bool reminders_saved = audio_reminders_saved;
    const bool session_active = audio_session_active;
    const bool leave_confirming = audio_leave_confirming;
    const uint32_t session_seconds = audio_session_seconds;
    const uint32_t today_sitting_seconds = audio_today_sitting_seconds;
    const uint32_t today_sessions = audio_today_sessions;
    const uint32_t today_reminders = audio_today_reminders;
    const uint32_t last_session_seconds = audio_last_session_seconds;
    const uint32_t last_session_reminders = audio_last_session_reminders;
    const bool last_session_responded = audio_last_session_responded;
    const bool clock_synced = audio_clock_synced;
    const uint16_t blast_frequency_hz = audio_blast_frequency_hz;
    const uint16_t motor_frequency_hz = audio_motor_frequency_hz;
    const uint8_t blast_amplitude = audio_blast_amplitude;
    const uint8_t motor_amplitude = audio_motor_amplitude;
    const uint32_t pwm_carrier_hz = audio_pwm_carrier_hz;
    const bool motor_settings_saved = audio_motor_settings_saved;
    const bool vibration_level_1_active =
        audio_vibration_level_1_active;
    const bool vibration_level_2_active =
        audio_vibration_level_2_active;
    const bool motor_direct_running = audio_motor_direct_running;
    portEXIT_CRITICAL(&audio_status_lock);

    const int length = snprintf(status, sizeof(status),
                                "{\"p\":%u,\"v\":%u,"
                                "\"d\":[%" PRIu32 ",%" PRIu32
                                ",%" PRIu32 "],\"ok\":%u,\"on\":%u,\"k\":%u,"
                                "\"a\":%u,\"lv\":%u,\"s\":%" PRIu32 ","
                                "\"t\":%" PRIu32 ",\"n\":%" PRIu32 ","
                                "\"r\":%" PRIu32 ",\"l\":%" PRIu32 ","
                                "\"q\":%" PRIu32 ",\"x\":%u,\"c\":%u,"
                                "\"bh\":%u,\"mh\":%u,\"ba\":%u,\"ma\":%u,"
                                "\"pc\":%" PRIu32 ","
                                "\"mk\":%u,\"v1\":%u,\"v2\":%u,\"mr\":%u}",
                                pressure ? 1U : 0U, volume, delay_1,
                                delay_2, delay_3, saved ? 1U : 0U,
                                reminders_enabled ? 1U : 0U,
                                reminders_saved ? 1U : 0U,
                                session_active ? 1U : 0U,
                                leave_confirming ? 1U : 0U,
                                session_seconds, today_sitting_seconds,
                                today_sessions, today_reminders,
                                last_session_seconds, last_session_reminders,
                                last_session_responded ? 1U : 0U,
                                clock_synced ? 1U : 0U, blast_frequency_hz,
                                motor_frequency_hz, blast_amplitude,
                                motor_amplitude, pwm_carrier_hz,
                                motor_settings_saved ? 1U : 0U,
                                vibration_level_1_active ? 1U : 0U,
                                vibration_level_2_active ? 1U : 0U,
                                motor_direct_running ? 1U : 0U);
    if (length <= 0 || length >= sizeof(status)) {
        ESP_LOGE(TAG, "Could not format audio status");
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, status, length) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int history_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &command_uuid.u,
                    .access_cb = command_access,
                    .flags = BLE_GATT_CHR_F_WRITE |
                             BLE_GATT_CHR_F_WRITE_NO_RSP,
                },
                {
                    .uuid = &status_uuid.u,
                    .access_cb = status_access,
                    .val_handle = &status_value_handle,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                },
                {
                    .uuid = &history_uuid.u,
                    .access_cb = history_access,
                    .val_handle = &history_value_handle,
                    .flags = BLE_GATT_CHR_F_NOTIFY,
                },
                {0},
            },
    },
    {0},
};

static void configure_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

    struct ble_hs_adv_fields response_fields;
    memset(&response_fields, 0, sizeof(response_fields));
    response_fields.uuids128 = &service_uuid;
    response_fields.num_uuids128 = 1;
    response_fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising data setup failed: %d", rc);
    }
    rc = ble_gap_adv_rsp_set_fields(&response_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Scan response setup failed: %d", rc);
    }
}

static void start_advertising(void)
{
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER,
                                     &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising as %s", DEVICE_NAME);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        reset_framed_command();
        if (event->connect.status == 0) {
            connection_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE client connected");
        } else {
            start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        reset_framed_command();
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "Web client disconnected");
        start_advertising();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &own_address_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Bluetooth address setup failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "NimBLE synchronized; own address type=%u", own_address_type);
    configure_advertising();
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset; reason=%d", reason);
}

static void nimble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_control_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE device name setup failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_count_cfg(gatt_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(gatt_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT service registration failed: %d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_att_set_preferred_mtu(256);
    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "NimBLE initialized; waiting for host synchronization");
    return ESP_OK;
}

bool ble_control_is_connected(void)
{
    return connection_handle != BLE_HS_CONN_HANDLE_NONE;
}

void ble_control_update_audio_status(bool pressure, uint8_t volume_percent,
                                     uint32_t delay_1_ms,
                                     uint32_t delay_2_ms,
                                     uint32_t delay_3_ms, bool saved,
                                     bool reminders_enabled,
                                     bool reminders_saved,
                                     bool session_active,
                                     bool leave_confirming,
                                     uint32_t session_seconds,
                                     uint32_t today_sitting_seconds,
                                     uint32_t today_sessions,
                                     uint32_t today_reminders,
                                     uint32_t last_session_seconds,
                                     uint32_t last_session_reminders,
                                     bool last_session_responded,
                                     bool clock_synced,
                                     uint16_t blast_frequency_hz,
                                     uint16_t motor_frequency_hz,
                                     uint8_t blast_amplitude,
                                     uint8_t motor_amplitude,
                                     uint32_t pwm_carrier_hz,
                                     bool motor_settings_saved,
                                     bool vibration_level_1_active,
                                     bool vibration_level_2_active,
                                     bool motor_direct_running)
{
    portENTER_CRITICAL(&audio_status_lock);
    audio_pressure = pressure;
    audio_volume_percent = volume_percent;
    audio_delay_1_ms = delay_1_ms;
    audio_delay_2_ms = delay_2_ms;
    audio_delay_3_ms = delay_3_ms;
    audio_settings_saved = saved;
    audio_reminders_enabled = reminders_enabled;
    audio_reminders_saved = reminders_saved;
    audio_session_active = session_active;
    audio_leave_confirming = leave_confirming;
    audio_session_seconds = session_seconds;
    audio_today_sitting_seconds = today_sitting_seconds;
    audio_today_sessions = today_sessions;
    audio_today_reminders = today_reminders;
    audio_last_session_seconds = last_session_seconds;
    audio_last_session_reminders = last_session_reminders;
    audio_last_session_responded = last_session_responded;
    audio_clock_synced = clock_synced;
    audio_blast_frequency_hz = blast_frequency_hz;
    audio_motor_frequency_hz = motor_frequency_hz;
    audio_blast_amplitude = blast_amplitude;
    audio_motor_amplitude = motor_amplitude;
    audio_pwm_carrier_hz = pwm_carrier_hz;
    audio_motor_settings_saved = motor_settings_saved;
    audio_vibration_level_1_active = vibration_level_1_active;
    audio_vibration_level_2_active = vibration_level_2_active;
    audio_motor_direct_running = motor_direct_running;
    portEXIT_CRITICAL(&audio_status_lock);

    if (connection_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    char status[256];
    const int length = snprintf(status, sizeof(status),
                                "{\"p\":%u,\"v\":%u,"
                                "\"d\":[%" PRIu32 ",%" PRIu32
                                ",%" PRIu32 "],\"ok\":%u,\"on\":%u,\"k\":%u,"
                                "\"a\":%u,\"lv\":%u,\"s\":%" PRIu32 ","
                                "\"t\":%" PRIu32 ",\"n\":%" PRIu32 ","
                                "\"r\":%" PRIu32 ",\"l\":%" PRIu32 ","
                                "\"q\":%" PRIu32 ",\"x\":%u,\"c\":%u,"
                                "\"bh\":%u,\"mh\":%u,\"ba\":%u,\"ma\":%u,"
                                "\"pc\":%" PRIu32 ","
                                "\"mk\":%u,\"v1\":%u,\"v2\":%u,\"mr\":%u}",
                                pressure ? 1U : 0U, volume_percent,
                                delay_1_ms, delay_2_ms, delay_3_ms,
                                saved ? 1U : 0U,
                                reminders_enabled ? 1U : 0U,
                                reminders_saved ? 1U : 0U,
                                session_active ? 1U : 0U,
                                leave_confirming ? 1U : 0U,
                                session_seconds, today_sitting_seconds,
                                today_sessions, today_reminders,
                                last_session_seconds, last_session_reminders,
                                last_session_responded ? 1U : 0U,
                                clock_synced ? 1U : 0U, blast_frequency_hz,
                                motor_frequency_hz, blast_amplitude,
                                motor_amplitude, pwm_carrier_hz,
                                motor_settings_saved ? 1U : 0U,
                                vibration_level_1_active ? 1U : 0U,
                                vibration_level_2_active ? 1U : 0U,
                                motor_direct_running ? 1U : 0U);
    if (length <= 0 || length >= sizeof(status)) {
        ESP_LOGE(TAG, "Could not format audio status notification");
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, length);
    if (om == NULL) {
        ESP_LOGE(TAG, "Could not allocate audio status notification");
        return;
    }
    const int rc = ble_gatts_notify_custom(connection_handle,
                                           status_value_handle, om);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "Audio status notification failed: %d", rc);
    }
}

void ble_control_notify_history(const char *json)
{
    if (connection_handle == BLE_HS_CONN_HANDLE_NONE) return;

    const size_t length = strlen(json);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, length);
    if (om == NULL) {
        ESP_LOGE(TAG, "Could not allocate history notification");
        return;
    }
    const int rc = ble_gatts_notify_custom(connection_handle,
                                           history_value_handle, om);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "History notification failed: %d", rc);
    }
}
