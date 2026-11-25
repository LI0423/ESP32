#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "companion_timer.h"

int main(void)
{
    companion_timer_t timer;
    companion_timer_reset(&timer);
    assert(timer.phase == COMPANION_PHASE_FOCUS);
    assert(timer.remaining_seconds == 1500);

    companion_timer_advance(&timer, 1499);
    assert(timer.phase == COMPANION_PHASE_FOCUS);
    assert(timer.remaining_seconds == 1);

    companion_timer_advance(&timer, 1);
    assert(timer.phase == COMPANION_PHASE_CELEBRATE);
    assert(timer.remaining_seconds == 3);
    assert(timer.completed_sessions == 1);

    companion_timer_advance(&timer, 3);
    assert(timer.phase == COMPANION_PHASE_BREAK);
    assert(timer.remaining_seconds == 300);

    companion_timer_advance(&timer, 300);
    assert(timer.phase == COMPANION_PHASE_FOCUS);
    assert(timer.remaining_seconds == 1500);

    companion_timer_reset(&timer);
    companion_timer_advance(&timer, 1500 + 3 + 300 + 10);
    assert(timer.phase == COMPANION_PHASE_FOCUS);
    assert(timer.remaining_seconds == 1490);
    assert(timer.completed_sessions == 1);

    companion_timer_restore(&timer, COMPANION_PHASE_BREAK, 12, 7);
    assert(timer.phase == COMPANION_PHASE_BREAK);
    assert(timer.remaining_seconds == 12);
    assert(timer.completed_sessions == 7);
    assert(companion_timer_phase_total(&timer) == 300);

    companion_timer_restore(&timer, (companion_phase_t)99, UINT32_MAX, 0);
    assert(timer.phase == COMPANION_PHASE_FOCUS);
    assert(timer.remaining_seconds == 1500);

    puts("companion timer tests passed");
    return 0;
}
