#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Today's goal on the settings page. N may exceed G; G stays in this range. */
#define COMPANION_DAILY_GOAL_DEFAULT 4u
#define COMPANION_DAILY_GOAL_MIN 1u
#define COMPANION_DAILY_GOAL_MAX 99u

/* HUD text is `ND` and stops at `999D`. The counter itself may sit above that. */
#define COMPANION_STREAK_DISPLAY_MAX 999u

typedef struct {
    uint32_t completed;   /* rounds finished on day_key */
    uint32_t goal;        /* daily goal, always sanitized */
    uint32_t streak;      /* consecutive met days, including today once counted */
    int32_t day_key;      /* local YYYYMMDD for `completed`, 0 if unknown */
    bool goal_met_today;  /* streak already includes day_key */
    bool missed_day;      /* a past day ended short of goal: vitality may be sick */
} companion_progress_t;

/* Three vitality looks. Full is derived from today's count; sick needs a
 * missed day remembered across reboots, so it is not inferable from streak 0
 * (first use is also 0). */
typedef enum {
    COMPANION_VITALITY_FULL = 0,
    COMPANION_VITALITY_HUNGRY,
    COMPANION_VITALITY_SICK,
} companion_vitality_t;

void companion_progress_reset(companion_progress_t *progress);
uint32_t companion_progress_sanitize_goal(uint32_t goal);
void companion_progress_set_goal(companion_progress_t *progress, uint32_t goal);

/* Re-evaluate whether `completed` meets the goal. Idempotent. */
void companion_progress_sync(companion_progress_t *progress, uint32_t completed);

/*
 * Move onto `today` (YYYYMMDD).
 * Same day: sync only.
 * day_key == 0 (first sync): adopt `today` and keep `completed`, without
 * treating this as a missed day. Then sync so a goal already reached counts
 * once.
 * gap_days < 1 (clock stepped backward): clear today's count and adopt
 * `today` without judging the streak.
 * gap_days == 1: if this day was not yet counted, increment when the goal was
 * met and reset the streak to 0 when it was not.
 * gap_days > 1: at least one full day was missed, so the streak becomes 0.
 * A new day always clears `completed` and `goal_met_today`.
 *
 * `missed_day` is set when a day boundary shows yesterday short of the goal
 * (gap_days == 1) or a skipped day (gap_days > 1). Meeting today's goal in
 * companion_progress_sync() clears it again (sick -> full recovery). The
 * first sync (day_key == 0) and a backward clock step never set it.
 */
void companion_progress_roll_day(companion_progress_t *progress, int32_t today, int gap_days);

/*
 * full   : today's goal already met (completed >= goal).
 * hungry : goal not met and the pet is not sick.
 * sick   : goal not met, time is valid, the day is known and a past day was
 *          missed. Time invalid or first use never reports sick.
 */
companion_vitality_t companion_progress_vitality(const companion_progress_t *progress,
                                                 bool time_valid);
const char *companion_vitality_name(companion_vitality_t vitality);

void companion_progress_format_today(char *out, size_t capacity,
                                     uint32_t completed, uint32_t goal);
void companion_progress_format_streak(char *out, size_t capacity, uint32_t streak);

#ifdef __cplusplus
}
#endif
