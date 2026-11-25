#include "companion_timer.h"

#include <string.h>

#ifndef CONFIG_COMPANION_FOCUS_SECONDS
#define CONFIG_COMPANION_FOCUS_SECONDS 1500
#endif
#ifndef CONFIG_COMPANION_BREAK_SECONDS
#define CONFIG_COMPANION_BREAK_SECONDS 300
#endif
#ifndef CONFIG_COMPANION_CELEBRATE_SECONDS
#define CONFIG_COMPANION_CELEBRATE_SECONDS 3
#endif

static uint32_t duration_or_default(uint32_t value, uint32_t fallback)
{
    return value ? value : fallback;
}

void companion_timer_reset(companion_timer_t *timer)
{
    if (!timer) return;
    memset(timer, 0, sizeof(*timer));
    timer->phase = COMPANION_PHASE_FOCUS;
    timer->focus_seconds = duration_or_default(CONFIG_COMPANION_FOCUS_SECONDS, 1500);
    timer->break_seconds = duration_or_default(CONFIG_COMPANION_BREAK_SECONDS, 300);
    timer->celebrate_seconds = duration_or_default(CONFIG_COMPANION_CELEBRATE_SECONDS, 3);
    timer->remaining_seconds = timer->focus_seconds;
}

void companion_timer_restore(companion_timer_t *timer, companion_phase_t phase,
                             uint32_t remaining_seconds, uint32_t completed_sessions)
{
    if (!timer) return;
    companion_timer_reset(timer);
    if (phase > COMPANION_PHASE_BREAK) phase = COMPANION_PHASE_FOCUS;
    timer->phase = phase;
    uint32_t total = companion_timer_phase_total(timer);
    timer->remaining_seconds = remaining_seconds < total ? remaining_seconds : total;
    timer->completed_sessions = completed_sessions;
}

void companion_timer_advance(companion_timer_t *timer, uint32_t elapsed_seconds)
{
    if (!timer || elapsed_seconds == 0) return;
    while (elapsed_seconds > 0) {
        if (timer->remaining_seconds > elapsed_seconds) {
            timer->remaining_seconds -= elapsed_seconds;
            return;
        }
        elapsed_seconds -= timer->remaining_seconds;
        timer->remaining_seconds = 0;
        switch (timer->phase) {
        case COMPANION_PHASE_FOCUS:
            if (timer->completed_sessions < UINT32_MAX) timer->completed_sessions++;
            timer->phase = COMPANION_PHASE_CELEBRATE;
            timer->remaining_seconds = timer->celebrate_seconds;
            break;
        case COMPANION_PHASE_CELEBRATE:
            timer->phase = COMPANION_PHASE_BREAK;
            timer->remaining_seconds = timer->break_seconds;
            break;
        case COMPANION_PHASE_BREAK:
            timer->phase = COMPANION_PHASE_FOCUS;
            timer->remaining_seconds = timer->focus_seconds;
            break;
        default:
            timer->phase = COMPANION_PHASE_FOCUS;
            timer->remaining_seconds = timer->focus_seconds;
            break;
        }
    }
}

uint32_t companion_timer_phase_total(const companion_timer_t *timer)
{
    if (!timer) return 0;
    switch (timer->phase) {
    case COMPANION_PHASE_FOCUS: return timer->focus_seconds;
    case COMPANION_PHASE_CELEBRATE: return timer->celebrate_seconds;
    case COMPANION_PHASE_BREAK: return timer->break_seconds;
    default: return timer->focus_seconds;
    }
}

const char *companion_phase_name(companion_phase_t phase)
{
    switch (phase) {
    case COMPANION_PHASE_FOCUS: return "focus";
    case COMPANION_PHASE_CELEBRATE: return "celebrate";
    case COMPANION_PHASE_BREAK: return "break";
    default: return "focus";
    }
}
