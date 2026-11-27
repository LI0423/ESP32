#include "companion_model.h"

#include <string.h>
#include <time.h>
#include "companion_progress.h"
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
static companion_progress_t s_progress;
static companion_pet_t s_pet;
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
    s_state.daily_goal = s_progress.goal;
    s_state.streak = s_progress.streak;
    s_state.standby = s_timer.phase == COMPANION_PHASE_STANDBY;
    s_state.work_hours_override = s_work_hours_override;
    s_state.day_key = s_day_key;
    s_state.standby_until_epoch = s_standby_until_epoch;
    s_state.missed_day = s_progress.missed_day;
    s_state.vitality = companion_pet_vitality(&s_pet);
    s_state.pet = s_pet;
}

static void mark_brightness_locked(void)
{
    uint8_t next = companion_effective_brightness(s_configured_brightness, s_state.standby);
    if (next != s_last_brightness) {
        s_last_brightness = next;
        s_brightness_dirty = true;
    }
}

static void note_progress_edit_locked(uint32_t streak_before, bool met_before, bool missed_before)
{
    if (streak_before != s_progress.streak || met_before != s_progress.goal_met_today ||
        missed_before != s_progress.missed_day)
        s_persist_request = true;
}

static void sync_progress_count_locked(void)
{
    /* day_key 0 means the count is not tied to a local day yet (existing first sync). */
    if (s_day_key == 0) return;
    uint32_t streak_before = s_progress.streak;
    bool met_before = s_progress.goal_met_today;
    bool missed_before = s_progress.missed_day;
    s_progress.day_key = s_day_key;
    companion_progress_sync(&s_progress, s_timer.completed_sessions);
    note_progress_edit_locked(streak_before, met_before, missed_before);
}

static void rollover_today_locked(bool time_valid)
{
    if (!time_valid) return;
    struct tm local = {0};
    if (!local_now(&local, NULL)) return;
    int32_t today = companion_local_day_key(&local);
    if (today == 0) return;

    uint32_t streak_before = s_progress.streak;
    bool met_before = s_progress.goal_met_today;
    bool missed_before = s_progress.missed_day;
    uint32_t completed_before = s_timer.completed_sessions;
    int32_t day_before = s_day_key;
    uint8_t pet_stage_before = s_pet.stage;
    int32_t pet_born_before = s_pet.born_day;
    int gap = 0;
    if (s_day_key != 0 && s_day_key != today)
        gap = companion_day_key_gap(s_day_key, today);

    s_progress.completed = s_timer.completed_sessions;
    s_progress.day_key = s_day_key;
    companion_progress_roll_day(&s_progress, today, gap);
    companion_pet_roll_day(&s_pet, today, gap);
    s_timer.completed_sessions = s_progress.completed;
    s_day_key = s_progress.day_key;
    if (streak_before != s_progress.streak || met_before != s_progress.goal_met_today ||
        missed_before != s_progress.missed_day ||
        completed_before != s_timer.completed_sessions || day_before != s_day_key ||
        pet_stage_before != s_pet.stage || pet_born_before != s_pet.born_day)
        s_persist_request = true;
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
    companion_progress_reset(&s_progress);
    companion_pet_reset(&s_pet);
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
    bool standby_before = s_state.standby;
    int64_t standby_until_before = s_standby_until_epoch;
    bool override_before = s_work_hours_override;
    int32_t day_before = s_day_key;
    uint32_t streak_before = s_progress.streak;
    bool met_before = s_progress.goal_met_today;
    bool missed_before = s_progress.missed_day;
    uint8_t action_seconds_before = s_state.pet_action_seconds;

    rollover_today_locked(s_state.time_valid);
    apply_schedule_locked();
    uint32_t completed_before = s_timer.completed_sessions;
    if (s_timer.phase != COMPANION_PHASE_STANDBY)
        companion_timer_advance(&s_timer, elapsed_seconds);

    bool pet_changed = false;
    if (s_state.time_valid) {
        pet_changed = companion_pet_catch_up(&s_pet, s_state.standby, true, (int64_t)time(NULL));
    }
    if (s_timer.completed_sessions > completed_before) {
        uint32_t earned = s_timer.completed_sessions - completed_before;
        while (earned-- > 0) companion_pet_on_focus_completed(&s_pet);
        pet_changed = true;
        s_persist_request = true;
    }
    if (pet_changed) s_persist_request = true;

    if (s_state.pet_action_seconds > 0) {
        s_state.pet_action_seconds--;
        if (s_state.pet_action_seconds == 0) {
            s_state.pet_action = COMPANION_PET_ACTION_NONE;
            s_state.pet_action_result = COMPANION_PET_ACTION_OK;
        }
    }

    sync_progress_count_locked();
    sync_state_locked();
    update_clock_locked();
    mark_brightness_locked();

    if (before != s_timer.phase || standby_before != s_state.standby ||
        standby_until_before != s_standby_until_epoch || override_before != s_work_hours_override)
        s_persist_request = true;

    bool changed = before != s_timer.phase ||
                   completed_before != s_timer.completed_sessions ||
                   standby_before != s_state.standby ||
                   standby_until_before != s_standby_until_epoch ||
                   override_before != s_work_hours_override ||
                   day_before != s_day_key ||
                   streak_before != s_progress.streak ||
                   met_before != s_progress.goal_met_today ||
                   missed_before != s_progress.missed_day ||
                   pet_changed || action_seconds_before != s_state.pet_action_seconds;
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
    if (s_state.time_valid) {
        /*
         * The RTC may boot to a stale/future value before SNTP. A persisted
         * pet timestamp ahead of the synced wall clock would freeze catch-up
         * and block every care action forever, so rebase future timestamps
         * at the first valid clock. Runtime backward corrections are still
         * handled by companion_pet_catch_up().
         */
        if (companion_pet_rebase_future(&s_pet, now_epoch)) {
            ESP_LOGW(TAG, "pet timestamps ahead of clock; rebased to now");
            s_persist_request = true;
        }
        if (companion_pet_catch_up(&s_pet, s_state.standby, true, now_epoch))
            s_persist_request = true;
    }

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
    ESP_LOGI(TAG, "boot policy action=%d phase=%s completed_today=%lu goal=%lu streak=%lu",
             (int)action, companion_phase_name(s_timer.phase),
             (unsigned long)s_timer.completed_sessions,
             (unsigned long)s_progress.goal, (unsigned long)s_progress.streak);
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
    bool plausible = companion_time_plausible((int64_t)time(NULL));
    bool next = valid && plausible;
    if (next == s_state.time_valid) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_state.time_valid = next;
    if (next) {
        /*
         * Pet catch-up belongs to the boot policy on the first sync and to
         * the 1 Hz model tick afterwards. Doing it here raced boot restore
         * and used the pre-policy standby state.
         */
        rollover_today_locked(true);
        sync_state_locked();
    }
    update_clock_locked();
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
    out->streak = s_progress.streak;
    out->goal_met_today = s_progress.goal_met_today;
    out->missed_day = s_progress.missed_day;
    out->pet = s_pet;
    xSemaphoreGive(s_lock);
}

void companion_model_load_progress(uint32_t daily_goal, uint32_t streak, bool goal_met_today,
                                   bool missed_day)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_progress.goal = companion_progress_sanitize_goal(daily_goal);
    s_progress.streak = streak > 1000000u ? 0u : streak;
    s_progress.goal_met_today = goal_met_today;
    s_progress.missed_day = missed_day;
    s_progress.completed = s_timer.completed_sessions;
    s_progress.day_key = s_day_key;
    /*
     * Do not sync here. A same-day count that already met the goal, or a miss
     * that crosses midnight, is judged once the wall clock is valid so a
     * reboot cannot add the point twice before it is saved.
     */
    sync_state_locked();
    xSemaphoreGive(s_lock);
}

void companion_model_load_pet(const companion_pet_t *pet)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (pet && companion_pet_valid(pet)) s_pet = *pet;
    else companion_pet_reset(&s_pet);
    companion_pet_sanitize(&s_pet);
    sync_state_locked();
    s_persist_request = true;
    xSemaphoreGive(s_lock);
}

companion_pet_action_result_t companion_model_pet_action(companion_pet_action_t action)
{
    if (!s_lock) return COMPANION_PET_ACTION_NO_TIME;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool pet_changed = false;
    int64_t now = (int64_t)time(NULL);
    if (s_state.time_valid)
        pet_changed = companion_pet_catch_up(&s_pet, s_state.standby, true, now);

    companion_pet_action_result_t result;
    switch (action) {
    case COMPANION_PET_ACTION_FEED:
        result = companion_pet_feed(&s_pet, s_state.time_valid, now);
        break;
    case COMPANION_PET_ACTION_MEDICINE:
        result = companion_pet_medicine(&s_pet, s_state.time_valid, now);
        break;
    case COMPANION_PET_ACTION_PLAY:
        result = companion_pet_play(&s_pet, s_state.time_valid, now);
        break;
    case COMPANION_PET_ACTION_CLEAN:
        result = companion_pet_clean(&s_pet, s_state.time_valid, now);
        break;
    default:
        result = COMPANION_PET_ACTION_NO_TIME;
        break;
    }

    if (result == COMPANION_PET_ACTION_OK || result == COMPANION_PET_ACTION_OVERFED) {
        s_state.pet_action = action;
        s_state.pet_action_result = result;
        s_state.pet_action_seconds = 3;
        pet_changed = true;
    }
    if (pet_changed) s_persist_request = true;
    sync_state_locked();
    xSemaphoreGive(s_lock);
    return result;
}

void companion_model_set_daily_goal(uint32_t daily_goal)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t streak_before = s_progress.streak;
    bool met_before = s_progress.goal_met_today;
    bool missed_before = s_progress.missed_day;
    companion_progress_set_goal(&s_progress, daily_goal);
    sync_state_locked();
    note_progress_edit_locked(streak_before, met_before, missed_before);
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
