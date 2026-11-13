/* Open-loop vibration output; no calibrated motor voltage claim.
 * Inverted LEDC: duty=0 gives a HIGH input. Low pulses select drive;
 * both inputs HIGH select slow-decay. The enable input overrides both channels.
 */
#include "drv8833.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>

#define PWM_TICKS 1024U
#define FADE_US 50000LL
#define UPDATE_GAP_LIMIT_US 10000LL
#define MONITOR_GAP_LIMIT_US 20000LL
#define DRV8833_IN1_GPIO GPIO_NUM_4
#define DRV8833_ENABLE_GPIO GPIO_NUM_1
#define DRV8833_IN2_GPIO GPIO_NUM_5
static SemaphoreHandle_t lock;
static esp_timer_handle_t wave_timer;
static TaskHandle_t s_monitor_task;
static atomic_bool ready, running, idle_pending;
static atomic_int reason;
static atomic_llong last_update, deadline;
static atomic_uint updates, max_gap;
static int64_t started_at;
static unsigned peak_duty_ticks;
static unsigned active_vibration_hz = DRV8833_VIBRATION_HZ;
static unsigned carrier_frequency_hz = DRV8833_CARRIER_HZ;
static atomic_uint model_vm_mv, model_pwm_rms_mv, model_fundamental_mv, peak_ticks_report;
static unsigned fundamental_mv(unsigned vm, unsigned peak_ticks);
static int fault_level(void)
{
    return -1;
}
// Same integer quarter-wave representation as the audio driver, no FPU work.
static const int16_t quarter[65] = {
    0,804,1608,2410,3212,4011,4808,5602,6393,7179,7962,8739,9512,10278,11039,11793,
    12539,13279,14010,14732,15446,16151,16846,17530,18204,18868,19519,20159,
    20787,21403,22005,22594,23170,23731,24279,24811,25329,25832,26319,26790,
    27245,27683,28105,28510,28898,29268,29621,29956,30273,30571,30852,31113,
    31356,31580,31785,31971,32137,32285,32412,32521,32609,32678,32728,32757,32767
};

// Only start/stop and the waveform callback touch LEDC, under lock.
static esp_err_t set_low_pulse(ledc_channel_t channel, unsigned ticks)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, ticks);
    return err == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, channel) : err;
}
// All stop paths drop enable first, even if PWM updates fail or are stale.
static void cut_output(drv8833_reason_t why)
{
    gpio_set_level(DRV8833_ENABLE_GPIO, 0);
    int previous = atomic_load(&reason);
    // Keep the first fault latched even if a delayed callback finishes later.
    while (previous < DRV8833_FAULT_PIN &&
           !atomic_compare_exchange_weak(&reason, &previous, (int)why)) { }
    atomic_store(&running, false);
    atomic_store(&idle_pending, true);
}

static void wave_tick(void *arg)
{
    if (!atomic_load(&running) && !atomic_load(&idle_pending)) return;
    if (xSemaphoreTake(lock, 0) != pdTRUE) return;
    if (!atomic_load(&running)) {
        esp_err_t a = set_low_pulse(LEDC_CHANNEL_0, 0);
        esp_err_t b = set_low_pulse(LEDC_CHANNEL_1, 0);
        if (a != ESP_OK || b != ESP_OK) atomic_store(&reason, DRV8833_PWM_FAULT);
        atomic_store(&idle_pending, false);
        esp_timer_stop(wave_timer);
        xSemaphoreGive(lock);
        return;
    }
    int64_t now = esp_timer_get_time();
    int64_t end = atomic_load(&deadline);
    int64_t gap = now - atomic_load(&last_update);
    if (fault_level() == 0) cut_output(DRV8833_FAULT_PIN);
    else if (end > 0 && now >= end) cut_output(DRV8833_FINISHED);
    else if (gap > UPDATE_GAP_LIMIT_US) cut_output(DRV8833_TIMING_FAULT);
    else {
        if ((uint32_t)gap > atomic_load(&max_gap)) atomic_store(&max_gap, gap);
        int64_t elapsed = now-started_at;
        unsigned phase = ((elapsed % 1000000LL)*active_vibration_hz*256/1000000) & 255U;
        unsigned quadrant = phase >> 6, offset = phase & 63U;
        unsigned magnitude = quarter[(quadrant & 1U) ? 64U-offset : offset];
        int64_t fade = elapsed < FADE_US ? elapsed : FADE_US;
        if (end > 0 && end-now < fade) fade = end-now;
        unsigned ticks = (uint64_t)magnitude*peak_duty_ticks*fade /
                         (32767ULL*FADE_US);
        bool negative = (phase & 128U) != 0;
        // First make the inactive input HIGH, then update the active input.
        esp_err_t err = set_low_pulse(negative ? LEDC_CHANNEL_1 : LEDC_CHANNEL_0, 0);
        if (err == ESP_OK) err = set_low_pulse(negative ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1, ticks);
        if (err != ESP_OK) cut_output(DRV8833_PWM_FAULT);
        else {
            atomic_store(&last_update, now);
            atomic_fetch_add(&updates, 1);
        }
    }
    xSemaphoreGive(lock);
}

// Independent of esp_timer task and USB parsing. This cuts stale PWM via GPIO.
// Blocks while the driver is idle so it does not wake the CPU every tick.
static void monitor(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (atomic_load(&running)) {
            vTaskDelay(1);
            if (!atomic_load(&running)) break;
            if (xSemaphoreTake(lock, 0) != pdTRUE) {
                // Never wait behind a stuck waveform operation.
                if (esp_timer_get_time()-atomic_load(&last_update) > MONITOR_GAP_LIMIT_US)
                    cut_output(DRV8833_TIMING_FAULT);
                continue;
            }
            if (atomic_load(&running)) {
                int64_t now = esp_timer_get_time();
                if (fault_level() == 0) cut_output(DRV8833_FAULT_PIN);
                else if (atomic_load(&deadline) > 0 && now >= atomic_load(&deadline))
                    cut_output(DRV8833_FINISHED);
                else if (now-atomic_load(&last_update) > MONITOR_GAP_LIMIT_US)
                    cut_output(DRV8833_TIMING_FAULT);
            }
            xSemaphoreGive(lock);
        }
    }
}

esp_err_t drv8833_init(void)
{
    if (atomic_load(&ready)) return ESP_OK;
    gpio_config_t outputs = {.pin_bit_mask=1ULL<<DRV8833_ENABLE_GPIO,
        .mode=GPIO_MODE_OUTPUT};
    esp_err_t err = gpio_config(&outputs);
    if (err != ESP_OK) return err;
    gpio_set_level(DRV8833_ENABLE_GPIO, 0);
    lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;
    ledc_timer_config_t timer = {.speed_mode=LEDC_LOW_SPEED_MODE,
        .duty_resolution=LEDC_TIMER_10_BIT, .timer_num=LEDC_TIMER_0,
        .freq_hz=DRV8833_CARRIER_HZ, .clk_cfg=LEDC_USE_XTAL_CLK};
    err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    for (unsigned ch=0; ch<2; ++ch) {
        ledc_channel_config_t channel = {
            .gpio_num=ch ? DRV8833_IN2_GPIO : DRV8833_IN1_GPIO,
            .speed_mode=LEDC_LOW_SPEED_MODE, .channel=(ledc_channel_t)ch,
            .intr_type=LEDC_INTR_DISABLE, .timer_sel=LEDC_TIMER_0,
            .duty=0, .hpoint=0, .flags.output_invert=1};
        err = ledc_channel_config(&channel);
        if (err != ESP_OK) return err;
    }
    esp_timer_create_args_t args = {.callback=wave_tick, .name="drv8833_wave",
        .dispatch_method=ESP_TIMER_TASK, .skip_unhandled_events=true};
    err = esp_timer_create(&args, &wave_timer);
    if (err != ESP_OK) return err;
    // The waveform timer is only started while a vibration is running; leaving
    // it running 2000x/s while idle defeats modem sleep. See start_pwm().
    if (xTaskCreate(monitor, "drv8833_guard", 2048, NULL, 5,
                    &s_monitor_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&ready, true);
    return ESP_OK;
}

esp_err_t drv8833_set_carrier_frequency(unsigned frequency_hz)
{
    if (frequency_hz < DRV8833_MIN_CARRIER_HZ ||
        frequency_hz > DRV8833_MAX_CARRIER_HZ) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&ready)) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0,
                                  frequency_hz);
    if (err == ESP_OK) carrier_frequency_hz = frequency_hz;
    xSemaphoreGive(lock);
    return err;
}

unsigned drv8833_get_carrier_frequency(void)
{
    return carrier_frequency_hz;
}

static esp_err_t start_pwm(uint32_t duration_ms, unsigned peak_ticks,
                           unsigned assumed_vm, unsigned estimate_mv,
                           unsigned vibration_hz)
{
    if ((duration_ms && (duration_ms < 100 || duration_ms > DRV8833_MAX_TEST_MS)) ||
        !peak_ticks || peak_ticks > PWM_TICKS*DRV8833_MAX_LEVEL/100U ||
        !vibration_hz ||
        vibration_hz > DRV8833_MAX_VIBRATION_HZ) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&ready)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    const drv8833_reason_t previous_reason = atomic_load(&reason);
    // A new request may recover a timing fault through the normal wake and
    // PWM setup below. Other faults still require explicit recovery.
    if (atomic_load(&running) ||
        (previous_reason >= DRV8833_FAULT_PIN &&
         previous_reason != DRV8833_TIMING_FAULT)) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (previous_reason == DRV8833_TIMING_FAULT) {
        atomic_store(&reason, DRV8833_IDLE);
    }
    if (fault_level() == 0) {
        cut_output(DRV8833_FAULT_PIN);
        err = ESP_ERR_INVALID_STATE;
    } else {
        gpio_set_level(DRV8833_ENABLE_GPIO, 0);
        err = set_low_pulse(LEDC_CHANNEL_0, 0);
        if (err == ESP_OK) err = set_low_pulse(LEDC_CHANNEL_1, 0);
        if (err == ESP_OK) {
            gpio_set_level(DRV8833_ENABLE_GPIO, 1);
            // Round up: at FreeRTOS 100Hz, pdMS_TO_TICKS(2) alone is zero.
            vTaskDelay(pdMS_TO_TICKS(2)+1);
            if (fault_level() == 0) {
                cut_output(DRV8833_FAULT_PIN);
                err = ESP_ERR_INVALID_STATE;
            } else {
                peak_duty_ticks = peak_ticks;
                active_vibration_hz = vibration_hz;
                atomic_store(&peak_ticks_report, peak_ticks);
                atomic_store(&model_vm_mv, assumed_vm);
                atomic_store(&model_pwm_rms_mv, estimate_mv);
                atomic_store(&model_fundamental_mv, fundamental_mv(assumed_vm,peak_ticks));
                started_at = esp_timer_get_time();
                atomic_store(&deadline, duration_ms ? started_at+(int64_t)duration_ms*1000 : 0);
                atomic_store(&last_update, started_at);
                atomic_store(&updates, 0);
                atomic_store(&max_gap, 0);
                atomic_store(&reason, DRV8833_RUNNING);
                atomic_store(&idle_pending, false);
                atomic_store(&running, true);
                if (!esp_timer_is_active(wave_timer)) {
                    esp_timer_start_periodic(wave_timer, DRV8833_UPDATE_US);
                }
                if (s_monitor_task != NULL) {
                    xTaskNotifyGive(s_monitor_task);
                }
            }
        } else cut_output(DRV8833_PWM_FAULT);
    }
    xSemaphoreGive(lock);
    return err;
}

esp_err_t drv8833_start(uint32_t duration_ms, unsigned level)
{
    return drv8833_start_with_frequency(duration_ms, level,
                                        DRV8833_VIBRATION_HZ);
}

esp_err_t drv8833_start_with_frequency(uint32_t duration_ms, unsigned level,
                                       unsigned vibration_hz)
{
    if (!level || level > DRV8833_MAX_LEVEL) return ESP_ERR_INVALID_ARG;
    return start_pwm(duration_ms, level*PWM_TICKS/100U, 0, 0, vibration_hz);
}

esp_err_t drv8833_start_effect(drv8833_effect_t effect)
{
    switch (effect) {
    case DRV8833_EFFECT_WEAK:
        return drv8833_start_with_frequency(DRV8833_EFFECT_SHORT_MS, 80, 50);
    case DRV8833_EFFECT_STRONG:
        return drv8833_start_with_frequency(DRV8833_EFFECT_SHORT_MS, 80, 60);
    case DRV8833_EFFECT_LONG:
        return drv8833_start_with_frequency(DRV8833_EFFECT_LONG_MS, 70, 60);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

// Sum the actual quantized pulse widths over a uniform 256-phase cycle.
// Model: each PWM output is +VM or -VM while driven, zero while recirculating.
// Dead-time, diode conduction, chip current chopping and task jitter are excluded.
static uint32_t pulse_sum(unsigned peak_ticks)
{
    uint32_t sum = 0;
    for (unsigned phase=0; phase<256; ++phase) {
        unsigned q = phase>>6, offset = phase&63U;
        unsigned mag = quarter[(q&1U) ? 64U-offset : offset];
        sum += mag*peak_ticks/32767U;
    }
    return sum;
}

static unsigned pwm_rms_mv(unsigned vm, unsigned peak_ticks)
{
    uint64_t energy=(uint64_t)vm*vm*pulse_sum(peak_ticks);
    unsigned lo=0, hi=vm;
    while (lo<hi) {
        unsigned mid=(lo+hi+1)/2;
        if ((uint64_t)mid*mid*256U*PWM_TICKS<=energy) lo=mid;
        else hi=mid-1;
    }
    return lo;
}

// Sine projection of the signed PWM-cycle-average output over 256 phases.
// sqrt(2) is approximated in millionths; all results are model estimates.
#define FUNDAMENTAL_DENOM (128ULL*PWM_TICKS*32767U*1414214U)
static uint64_t fundamental_numerator(unsigned vm, unsigned peak_ticks)
{
    uint64_t sum=0;
    for (unsigned phase=0; phase<256; ++phase) {
        unsigned q=phase>>6, offset=phase&63U;
        unsigned mag=quarter[(q&1U) ? 64U-offset : offset];
        sum += (uint64_t)(mag*peak_ticks/32767U)*mag;
    }
    return (uint64_t)vm*sum*1000000U;
}
static unsigned fundamental_mv(unsigned vm, unsigned peak_ticks)
{
    return fundamental_numerator(vm,peak_ticks)/FUNDAMENTAL_DENOM;
}

esp_err_t drv8833_start_estimated_rms(uint32_t duration_ms, unsigned rms_mv)
{
    const uint32_t vm = DRV8833_ASSUMED_VM_MV;
    if (rms_mv < 200 || rms_mv > DRV8833_TARGET_RMS_MV || vm < 3300 || vm > 5500)
        return ESP_ERR_INVALID_ARG;
    uint64_t budget = (uint64_t)rms_mv*rms_mv*256U*PWM_TICKS;
    unsigned best=0;
    for (unsigned peak=1; peak<=PWM_TICKS/4; ++peak) {
        if ((uint64_t)vm*vm*pulse_sum(peak)>budget) break;
        best=peak;
    }
    if (!best) return ESP_ERR_INVALID_ARG;
    return start_pwm(duration_ms,best,vm,pwm_rms_mv(vm,best),
                     DRV8833_VIBRATION_HZ);
}

esp_err_t drv8833_start_fundamental(uint32_t duration_ms, unsigned rms_mv)
{
    const unsigned vm=DRV8833_ASSUMED_VM_MV;
    if (rms_mv<200 || rms_mv>DRV8833_TARGET_FUNDAMENTAL_MV || vm<4500 || vm>5500)
        return ESP_ERR_INVALID_ARG;
    unsigned best=0;
    uint64_t budget=(uint64_t)rms_mv*FUNDAMENTAL_DENOM;
    for (unsigned peak=1; peak<=PWM_TICKS/4; ++peak) {
        if (fundamental_numerator(vm,peak)>budget) break;
        best=peak;
    }
    if (!best) return ESP_ERR_INVALID_ARG;
    return start_pwm(duration_ms,best,vm,pwm_rms_mv(vm,best),
                     DRV8833_VIBRATION_HZ);
}

void drv8833_stop(void)
{
    gpio_set_level(DRV8833_ENABLE_GPIO, 0);
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    drv8833_reason_t why = atomic_load(&reason);
    cut_output(why >= DRV8833_FAULT_PIN ? why : DRV8833_MANUAL_STOP);
    esp_err_t a = set_low_pulse(LEDC_CHANNEL_0, 0);
    esp_err_t b = set_low_pulse(LEDC_CHANNEL_1, 0);
    if (a != ESP_OK || b != ESP_OK) atomic_store(&reason, DRV8833_PWM_FAULT);
    atomic_store(&idle_pending, false);
    esp_timer_stop(wave_timer);
    xSemaphoreGive(lock);
}

bool drv8833_clear_fault(void)
{
    if (!atomic_load(&ready)) return false;
    drv8833_stop();
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = fault_level() != 0;
    // A PWM API error requires reset/reinitialization, not just clearing a flag.
    if (atomic_load(&reason) == DRV8833_PWM_FAULT) ok = false;
    if (ok) atomic_store(&reason, DRV8833_IDLE);
    xSemaphoreGive(lock);
    return ok;
}

drv8833_status_t drv8833_status(void)
{
    bool active = atomic_load(&running);
    int64_t end = atomic_load(&deadline);
    int64_t left = end-esp_timer_get_time();
    return (drv8833_status_t){.ready=atomic_load(&ready), .running=active,
        .continuous=active && end==0,
        .fault_pin=fault_level(), .reason=atomic_load(&reason),
        .updates=atomic_load(&updates), .max_gap_us=atomic_load(&max_gap),
        .peak_ticks=atomic_load(&peak_ticks_report),
        .model_vm_mv=atomic_load(&model_vm_mv), .model_pwm_rms_mv=atomic_load(&model_pwm_rms_mv),
        .model_fundamental_mv=atomic_load(&model_fundamental_mv),
        .remaining_ms=active && left>0 ? (uint32_t)((left+999)/1000) : 0};
}
