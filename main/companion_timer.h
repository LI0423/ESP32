#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMPANION_PHASE_FOCUS = 0,
    COMPANION_PHASE_CELEBRATE,
    COMPANION_PHASE_BREAK,
    COMPANION_PHASE_STANDBY,
} companion_phase_t;

typedef struct {
    companion_phase_t phase;
    uint32_t remaining_seconds;
    uint32_t completed_sessions; /* today's completed focus rounds (local day) */
    uint32_t focus_seconds;
    uint32_t break_seconds;
    uint32_t celebrate_seconds;
} companion_timer_t;

void companion_timer_reset(companion_timer_t *timer);
void companion_timer_enter_standby(companion_timer_t *timer);
void companion_timer_start_focus(companion_timer_t *timer);
void companion_timer_restore(companion_timer_t *timer, companion_phase_t phase,
                             uint32_t remaining_seconds, uint32_t completed_sessions);
void companion_timer_advance(companion_timer_t *timer, uint32_t elapsed_seconds);
uint32_t companion_timer_phase_total(const companion_timer_t *timer);
const char *companion_phase_name(companion_phase_t phase);
const char *companion_phase_label(companion_phase_t phase);
const char *companion_phase_phrase(companion_phase_t phase);

#ifdef __cplusplus
}
#endif
