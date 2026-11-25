#include "companion_model.h"

#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "companion"

static SemaphoreHandle_t s_lock;
static companion_timer_t s_timer;
static companion_state_t s_state;

static void update_clock_locked(void)
{
    time_t now = time(NULL);
    struct tm local = {0};
    if (now > 1700000000 && localtime_r(&now, &local)) {
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
}

void companion_model_init(const companion_timer_t *timer)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock || !timer) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_timer = *timer;
    memset(&s_state, 0, sizeof(s_state));
    s_state.online = false;
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
    companion_timer_advance(&s_timer, elapsed_seconds);
    sync_state_locked();
    update_clock_locked();
    bool changed = before != s_timer.phase || completed_before != s_timer.completed_sessions;
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
    xSemaphoreGive(s_lock);
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
    s_state.time_valid = valid && time(NULL) > 1700000000;
    xSemaphoreGive(s_lock);
}

void companion_model_snapshot(companion_state_t *out)
{
    if (!out || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}
