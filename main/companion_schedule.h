#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Epoch below this is treated as unsynced / implausible wall clock. */
#define COMPANION_EPOCH_MIN 1700000000LL

/* Local work window [08:00, 22:00). Outside → automatic standby. */
#define COMPANION_WORK_START_MIN (8 * 60)
#define COMPANION_WORK_END_MIN (22 * 60)

/*
 * Stale-save gap for boot restore (NOT "user presence").
 *
 * NVS timer records are written only on phase transitions, so saved_epoch is the
 * last phase-boundary timestamp — not the exact power-loss moment. A gap of
 * >= 30 minutes means the snapshot is stale relative to wall clock: we do not
 * fast-forward cycles and never invent completed sessions. A shorter gap means
 * we restore the saved phase remaining as a best-effort resume.
 */
#define COMPANION_STALE_SAVE_SECONDS (30 * 60)

/* Temporary backlight while standing by; does not rewrite NVS brightness. */
#define COMPANION_STANDBY_BRIGHTNESS 12

typedef enum {
    /* Time invalid: keep NVS phase/remaining; do not use wall clock for standby. */
    COMPANION_BOOT_SAFE_NO_CLOCK = 0,
    /* Gap < 30 min: keep saved phase/remaining (caller may still auto-standby). */
    COMPANION_BOOT_KEEP,
    /* Gap >= 30 min and inside work hours: start a fresh focus; no catch-up. */
    COMPANION_BOOT_FRESH_FOCUS,
    /* Gap >= 30 min and outside work hours: enter standby; no catch-up. */
    COMPANION_BOOT_ENTER_STANDBY,
} companion_boot_action_t;

bool companion_time_plausible(int64_t epoch);
bool companion_is_work_hours_minute(int minute_of_day);
int companion_local_minute_of_day(const struct tm *local);
int32_t companion_local_day_key(const struct tm *local);

/*
 * Next local 08:00 epoch.
 * Before 08:00 → today's 08:00; at/after 08:00 → tomorrow's 08:00.
 * Used by "standby tonight" so 21:00 and 01:00 both hold until the next 08:00
 * and never clear at midnight.
 */
int64_t companion_next_work_start_epoch(int64_t now_epoch, const struct tm *local);

companion_boot_action_t companion_boot_restore_action(bool time_valid,
                                                      int64_t now_epoch,
                                                      int64_t saved_epoch,
                                                      int minute_of_day);

bool companion_manual_standby_active(int64_t now_epoch, int64_t standby_until_epoch);

bool companion_should_auto_standby(bool time_valid, int minute_of_day,
                                   bool work_hours_override,
                                   int64_t now_epoch,
                                   int64_t standby_until_epoch);

bool companion_should_auto_resume(bool time_valid, int minute_of_day,
                                  bool in_standby,
                                  bool work_hours_override,
                                  int64_t now_epoch,
                                  int64_t standby_until_epoch);

uint8_t companion_effective_brightness(uint8_t configured, bool standby);

#ifdef __cplusplus
}
#endif
