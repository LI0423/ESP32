#include "companion_model.h"

#include <string.h>
#include <time.h>
#include "companion_schedule.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "companion"

static SemaphoreHandle_t s_lock;
static companion_timer_t s_timer;
static companion_state_t s_state;
static int64_t s_standby_until_epoch;
static bool s_work_hours_override;
static int32_t s_day_key;
static bool s_brightness_dirty;
static bool s_persist_request;
static uint8_t s_last_brightness = 255;
static uint8_t s_configured_brightness = 75;

static bool local_now(struct tm *out, int64_t *epoch_out)
{
    time_t now = time(NULL);
    if (!companion_time_plausible((int64_t)now) || !localtime_r(&now, out)) return false;
    if (epoch_out) *epoch_out = (int64_t)now;
    return true;
}

static void update_clock_locked(void)
{
    struct tm local = {0};
    if (local_now(&local, NULL)) {
        strftime(s_state.clock, sizeof(s_state.clock), "%H:%M", &local);
        strftime(s_state.date, sizeof(s_state.date), "%a %m/%d", &local);
    } else {
        strlcpy(s_state.clock, "--:--", sizeof(s_state.clock));
        strlcpy(s_state.date, "No time", sizeof(s_state.date));
    }
}

static void sync_state_locked(void)
{
    s_state.phase = s_timer.phase;
    s_state.remaining_seconds = s_timer.remaining_seconds;
    s_state.total_seconds = companion_timer_phase_total(&s_timer);
    s_state.completed_sessions = s_timer.completed_sessions;
    s_state.standby = s_timer.phase == COMPANION_PHASE_STANDBY;
    s_state.work_hours_override = s_work_hours_override;
    s_state.day_key = s_day_key;
    s_state.standby_until_epoch = s_standby_until_epoch;
}

static void mark_brightness_locked(void)
{
    uint8_t next = companion_effective_brightness(s_configured_brightness, s_state.standby);
    if (next != s_last_brightness) {
        s_last_brightness = next;
        s_brightness_dirty = true;
    }
}

static void rollover_today_locked(bool time_valid)
{
    if (!time_valid) return;
    struct tm local = {0};
    if (!local_now(&local, NULL)) return;
    int32_t today = companion_local_day_key(&local);
    if (today == 0) return;
    if (s_day_key == 0 || s_day_key != today) {
        s_timer.completed_sessions = 0;
        s_day_key = today;
    }
}

static void enter_standby_locked(void)
{
    companion_timer_enter_standby(&s_timer);
    s_work_hours_override = false;
    sync_state_locked();
    mark_brightness_locked();
}

static void start_focus_locked(bool manual)
{
    companion_timer_start_focus(&s_timer);
    s_standby_until_epoch = 0;
    /*
     * Manual start always sets override so a later NTP sync into night hours
     * cannot bounce straight back to standby. Auto resume/boot clear it.
     * apply_schedule_locked() clears override once local time is in work hours.
     */
    s_work_hours_override = manual;
    sync_state_locked();
    mark_brightness_locked();
}

static void apply_schedule_locked(void)
{
    if (!s_state.time_valid) return;
    struct tm local = {0};
    int64_t now = 0;
    if (!local_now(&local, &now)) return;

    int minute = companion_local_minute_of_day(&local);
    bool in_standby = s_timer.phase == COMPANION_PHASE_STANDBY;

    if (companion_is_work_hours_minute(minute)) {
        /* Clear overnight manual override once the next work window begins. */
        if (s_work_hours_override) s_work_hours_override = false;
        if (companion_time_plausible(s_standby_until_epoch) && now >= s_standby_until_epoch)
            s_standby_until_epoch = 0;
    }

    if (in_standby) {
        if (companion_should_auto_resume(true, minute, true, s_work_hours_override,
                                         now, s_standby_until_epoch)) {
            start_focus_locked(false);
        }
        return;
    }

    if (companion_should_auto_standby(true, minute, s_work_hours_override,
                                      now, s_standby_until_epoch)) {
        enter_standby_locked();
    }
}

void companion_model_init(const companion_timer_t *timer)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock || !timer) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_timer = *timer;
    memset(&s_state, 0, sizeof(s_state));
    s_state.online = false;
    s_standby_until_epoch = 0;
    s_work_hours_override = false;
    s_day_key = 0;
    s_brightness_dirty = true;
    s_last_brightness = 255;
    sync_state_locked();
    update_clock_locked();
    s_state.time_valid = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "timer started: phase=%s remaining=%lu",
             companion_phase_name(s_timer.phase), (unsigned long)s_timer.remaining_seconds);
}

bool companion_model_tick(uint32_t elapsed_seconds)
{
    if (!s_lock || elapsed_seconds == 0) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    companion_phase_t before = s_timer.phase;
    uint32_t completed_before = s_timer.completed_sessions;
    bool standby_before = s_state.standby;
    int64_t standby_until_before = s_standby_until_epoch;
    bool override_before = s_work_hours_override;
    int32_t day_before = s_day_key;

    rollover_today_locked(s_state.time_valid);
    apply_schedule_locked();
    if (s_timer.phase != COMPANION_PHASE_STANDBY)
        companion_timer_advance(&s_timer, elapsed_seconds);
    sync_state_locked();
    update_clock_locked();
    mark_brightness_locked();

    bool changed = before != s_timer.phase ||
                   completed_before != s_timer.completed_sessions ||
                   standby_before != s_state.standby ||
                   standby_until_before != s_standby_until_epoch ||
                   override_before != s_work_hours_override ||
                   day_before != s_day_key;
    xSemaphoreGive(s_lock);
    return changed;
}

void companion_model_restore(const companion_timer_t *timer)
{
    if (!s_lock || !timer) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_timer = *timer;
    sync_state_locked();
    update_clock_locked();
    mark_brightness_locked();
    xSemaphoreGive(s_lock);
}

void companion_model_apply_boot_policy(bool time_valid, int64_t now_epoch, int64_t saved_epoch)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.time_valid = time_valid && companion_time_plausible(now_epoch);
    update_clock_locked();

    struct tm local = {0};
    int minute = 0;
    time_t now_time = (time_t)now_epoch;
    if (s_state.time_valid && localtime_r(&now_time, &local))
        minute = companion_local_minute_of_day(&local);

    rollover_today_locked(s_state.time_valid);

    if (companion_manual_standby_active(now_epoch, s_standby_until_epoch)) {
        enter_standby_locked();
        xSemaphoreGive(s_lock);
        return;
    }

    companion_boot_action_t action =
        companion_boot_restore_action(s_state.time_valid, now_epoch, saved_epoch, minute);
    switch (action) {
    case COMPANION_BOOT_SAFE_NO_CLOCK:
        /* Keep restored phase/remaining; do not invent standby from wall clock. */
        break;
    case COMPANION_BOOT_KEEP:
        if (companion_should_auto_standby(true, minute, s_work_hours_override,
                                          now_epoch, s_standby_until_epoch))
            enter_standby_locked();
        break;
    case COMPANION_BOOT_FRESH_FOCUS:
        start_focus_locked(false);
        break;
    case COMPANION_BOOT_ENTER_STANDBY:
        enter_standby_locked();
        break;
    }
    sync_state_locked();
    mark_brightness_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "boot policy action=%d phase=%s completed_today=%lu",
             (int)action, companion_phase_name(s_timer.phase),
             (unsigned long)s_timer.completed_sessions);
}

void companion_model_set_network(bool online)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.online = online;
    xSemaphoreGive(s_lock);
}

void companion_model_set_time(bool valid)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    update_clock_locked();
    bool plausible = companion_time_plausible((int64_t)time(NULL));
    s_state.time_valid = valid && plausible;
    if (s_state.time_valid) {
        rollover_today_locked(true);
        sync_state_locked();
    }
    xSemaphoreGive(s_lock);
}

void companion_model_snapshot(companion_state_t *out)
{
    if (!out || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

void companion_model_persist_snapshot(companion_persist_snapshot_t *out)
{
    if (!out || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->phase = s_timer.phase;
    out->remaining_seconds = s_timer.remaining_seconds;
    out->completed_sessions = s_timer.completed_sessions;
    out->day_key = s_day_key;
    out->standby_until_epoch = s_standby_until_epoch;
    out->work_hours_override = s_work_hours_override;
    out->standby = s_timer.phase == COMPANION_PHASE_STANDBY;
    xSemaphoreGive(s_lock);
}

void companion_model_load_schedule(int32_t day_key, int64_t standby_until_epoch,
                                   bool work_hours_override)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_day_key = day_key;
    s_standby_until_epoch = standby_until_epoch;
    s_work_hours_override = work_hours_override;
    sync_state_locked();
    xSemaphoreGive(s_lock);
}

void companion_model_start_fresh_focus(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /*
     * Manual "start new focus" always begins a full focus cycle and sets
     * work_hours_override so a schedule tick (or later NTP sync into night)
     * does not immediately bounce back into standby. Override clears at 08:00
     * or when the user chooses standby-until-morning.
     */
    start_focus_locked(true);
    s_persist_request = true;
    bool override = s_work_hours_override;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "manual fresh focus (override=%d)", (int)override);
}

void companion_model_standby_until_morning(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    struct tm local = {0};
    int64_t now = 0;
    if (local_now(&local, &now)) {
        s_standby_until_epoch = companion_next_work_start_epoch(now, &local);
    } else {
        /* Without wall clock, enter standby but do not invent an until-epoch. */
        s_standby_until_epoch = 0;
    }
    enter_standby_locked();
    s_persist_request = true;
    int64_t until = s_standby_until_epoch;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "manual standby until epoch=%lld", (long long)until);
}

bool companion_model_persist_requested(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool requested = s_persist_request;
    s_persist_request = false;
    xSemaphoreGive(s_lock);
    return requested;
}

bool companion_model_brightness_changed(uint8_t *out_percent)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool dirty = s_brightness_dirty;
    if (dirty) {
        if (out_percent) *out_percent = s_last_brightness == 255
            ? companion_effective_brightness(s_configured_brightness, s_state.standby)
            : s_last_brightness;
        s_brightness_dirty = false;
    }
    xSemaphoreGive(s_lock);
    return dirty;
}

void companion_model_set_configured_brightness(uint8_t brightness)
{
    if (!s_lock) return;
    if (brightness < 5) brightness = 5;
    if (brightness > 100) brightness = 100;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_configured_brightness = brightness;
    mark_brightness_locked();
    xSemaphoreGive(s_lock);
}
