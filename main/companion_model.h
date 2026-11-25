#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "companion_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    companion_phase_t phase;
    uint32_t remaining_seconds;
    uint32_t total_seconds;
    uint32_t completed_sessions;
    bool online;
    bool time_valid;
    char clock[9];
    char date[16];
} companion_state_t;

void companion_model_init(const companion_timer_t *timer);
bool companion_model_tick(uint32_t elapsed_seconds);
void companion_model_restore(const companion_timer_t *timer);
void companion_model_set_network(bool online);
void companion_model_set_time(bool valid);
void companion_model_snapshot(companion_state_t *out);

#ifdef __cplusplus
}
#endif
