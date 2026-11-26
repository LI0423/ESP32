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
    uint32_t completed_sessions; /* today's completed focus rounds */
    bool online;
    bool time_valid;
    bool standby;
    bool work_hours_override;
    int32_t day_key;
    int64_t standby_until_epoch;
    char clock[9];
    char date[16];
} companion_state_t;

typedef struct {
    companion_phase_t phase;
    uint32_t remaining_seconds;
    uint32_t completed_sessions;
    int32_t day_key;
    int64_t standby_until_epoch;
    bool work_hours_override;
    bool standby;
} companion_persist_snapshot_t;

void companion_model_init(const companion_timer_t *timer);
bool companion_model_tick(uint32_t elapsed_seconds);
void companion_model_restore(const companion_timer_t *timer);
void companion_model_apply_boot_policy(bool time_valid, int64_t now_epoch, int64_t saved_epoch);
void companion_model_set_network(bool online);
void companion_model_set_time(bool valid);
void companion_model_snapshot(companion_state_t *out);
void companion_model_persist_snapshot(companion_persist_snapshot_t *out);
void companion_model_load_schedule(int32_t day_key, int64_t standby_until_epoch,
                                   bool work_hours_override);
void companion_model_start_fresh_focus(void);
void companion_model_standby_until_morning(void);
void companion_model_set_configured_brightness(uint8_t brightness);
bool companion_model_brightness_changed(uint8_t *out_percent);
bool companion_model_persist_requested(void);

#ifdef __cplusplus
}
#endif
