#include "companion_timer.h"

#include <string.h>

#ifndef CONFIG_COMPANION_FOCUS_SECONDS
#define CONFIG_COMPANION_FOCUS_SECONDS 1500
#endif
#ifndef CONFIG_COMPANION_BREAK_SECONDS
#define CONFIG_COMPANION_BREAK_SECONDS 300
#endif
#ifndef CONFIG_COMPANION_CELEBRATE_SECONDS
#define CONFIG_COMPANION_CELEBRATE_SECONDS 8
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
    timer->celebrate_seconds = duration_or_default(CONFIG_COMPANION_CELEBRATE_SECONDS, 8);
    timer->remaining_seconds = timer->focus_seconds;
}

void companion_timer_enter_standby(companion_timer_t *timer)
{
    if (!timer) return;
    uint32_t focus = duration_or_default(timer->focus_seconds, 1500);
    uint32_t brk = duration_or_default(timer->break_seconds, 300);
    uint32_t celeb = duration_or_default(timer->celebrate_seconds, 8);
    uint32_t completed = timer->completed_sessions;
    memset(timer, 0, sizeof(*timer));
    timer->focus_seconds = focus;
    timer->break_seconds = brk;
    timer->celebrate_seconds = celeb;
    timer->completed_sessions = completed;
    timer->phase = COMPANION_PHASE_STANDBY;
    timer->remaining_seconds = 0;
}

void companion_timer_start_focus(companion_timer_t *timer)
{
    if (!timer) return;
    uint32_t focus = duration_or_default(timer->focus_seconds, 1500);
    uint32_t brk = duration_or_default(timer->break_seconds, 300);
    uint32_t celeb = duration_or_default(timer->celebrate_seconds, 8);
    uint32_t completed = timer->completed_sessions;
    companion_timer_reset(timer);
    timer->focus_seconds = focus;
    timer->break_seconds = brk;
    timer->celebrate_seconds = celeb;
    timer->completed_sessions = completed;
    timer->phase = COMPANION_PHASE_FOCUS;
    timer->remaining_seconds = timer->focus_seconds;
}

void companion_timer_restore(companion_timer_t *timer, companion_phase_t phase,
                             uint32_t remaining_seconds, uint32_t completed_sessions)
{
    if (!timer) return;
    companion_timer_reset(timer);
    if (phase > COMPANION_PHASE_STANDBY) phase = COMPANION_PHASE_FOCUS;
    timer->phase = phase;
    timer->completed_sessions = completed_sessions;
    if (phase == COMPANION_PHASE_STANDBY) {
        timer->remaining_seconds = 0;
        return;
    }
    uint32_t total = companion_timer_phase_total(timer);
    timer->remaining_seconds = remaining_seconds < total ? remaining_seconds : total;
}

void companion_timer_advance(companion_timer_t *timer, uint32_t elapsed_seconds)
{
    if (!timer || elapsed_seconds == 0) return;
    if (timer->phase == COMPANION_PHASE_STANDBY) return;
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
        case COMPANION_PHASE_STANDBY:
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
    case COMPANION_PHASE_STANDBY: return 0;
    default: return timer->focus_seconds;
    }
}

const char *companion_phase_name(companion_phase_t phase)
{
    switch (phase) {
    case COMPANION_PHASE_FOCUS: return "focus";
    case COMPANION_PHASE_CELEBRATE: return "celebrate";
    case COMPANION_PHASE_BREAK: return "break";
    case COMPANION_PHASE_STANDBY: return "standby";
    default: return "focus";
    }
}

const char *companion_phase_label(companion_phase_t phase)
{
    switch (phase) {
    case COMPANION_PHASE_FOCUS: return "FOCUS";
    case COMPANION_PHASE_BREAK: return "BREAK";
    case COMPANION_PHASE_CELEBRATE: return "COMPLETE";
    case COMPANION_PHASE_STANDBY: return "STANDBY";
    default: return "FOCUS";
    }
}

const char *companion_phase_phrase(companion_phase_t phase)
{
    switch (phase) {
    case COMPANION_PHASE_FOCUS: return "Stay with the moment";
    case COMPANION_PHASE_BREAK: return "Rest your eyes";
    case COMPANION_PHASE_CELEBRATE: return "One session complete";
    case COMPANION_PHASE_STANDBY: return "Quiet until morning";
    default: return "Here with you";
    }
}
