#include "companion_progress.h"

#include <stdio.h>
#include <string.h>

#define COMPANION_STREAK_STORE_MAX 1000000u

static void sanitize_inplace(companion_progress_t *progress)
{
    progress->goal = companion_progress_sanitize_goal(progress->goal);
    if (progress->streak > COMPANION_STREAK_STORE_MAX)
        progress->streak = COMPANION_STREAK_STORE_MAX;
}

void companion_progress_reset(companion_progress_t *progress)
{
    if (!progress) return;
    memset(progress, 0, sizeof(*progress));
    progress->goal = COMPANION_DAILY_GOAL_DEFAULT;
}

uint32_t companion_progress_sanitize_goal(uint32_t goal)
{
    if (goal < COMPANION_DAILY_GOAL_MIN || goal > COMPANION_DAILY_GOAL_MAX)
        return COMPANION_DAILY_GOAL_DEFAULT;
    return goal;
}

void companion_progress_sync(companion_progress_t *progress, uint32_t completed)
{
    if (!progress) return;
    sanitize_inplace(progress);
    progress->completed = completed;
    bool met = progress->completed >= progress->goal;
    if (met && !progress->goal_met_today) {
        if (progress->streak < COMPANION_STREAK_STORE_MAX) progress->streak++;
        progress->goal_met_today = true;
    } else if (!met && progress->goal_met_today) {
        if (progress->streak > 0) progress->streak--;
        progress->goal_met_today = false;
    }
}

void companion_progress_set_goal(companion_progress_t *progress, uint32_t goal)
{
    if (!progress) return;
    progress->goal = companion_progress_sanitize_goal(goal);
    companion_progress_sync(progress, progress->completed);
}

void companion_progress_roll_day(companion_progress_t *progress, int32_t today, int gap_days)
{
    if (!progress || today <= 0) return;
    sanitize_inplace(progress);
    if (progress->day_key == today) {
        companion_progress_sync(progress, progress->completed);
        return;
    }
    /*
     * First wall-clock sync (no saved day): adopt today and keep any rounds
     * already finished. Do not treat this as a day change, and do not invent
     * a miss. If midnight passed while the clock was invalid, those rounds
     * belong to the local day after sync.
     */
    if (progress->day_key == 0) {
        progress->day_key = today;
        companion_progress_sync(progress, progress->completed);
        return;
    }
    /*
     * Clock moved backward: start this calendar day without calling it a miss
     * and without adding a point.
     */
    if (gap_days < 1) {
        progress->completed = 0;
        progress->goal_met_today = false;
        progress->day_key = today;
        return;
    }
    if (gap_days == 1) {
        if (!progress->goal_met_today) {
            if (progress->completed >= progress->goal) {
                if (progress->streak < COMPANION_STREAK_STORE_MAX) progress->streak++;
            } else {
                progress->streak = 0;
            }
        }
    } else {
        progress->streak = 0;
    }
    progress->completed = 0;
    progress->goal_met_today = false;
    progress->day_key = today;
}

void companion_progress_format_today(char *out, size_t capacity,
                                     uint32_t completed, uint32_t goal)
{
    if (!out || capacity == 0) return;
    snprintf(out, capacity, "Today %lu/%lu",
             (unsigned long)completed, (unsigned long)goal);
}

void companion_progress_format_streak(char *out, size_t capacity, uint32_t streak)
{
    if (!out || capacity == 0) return;
    if (streak > COMPANION_STREAK_DISPLAY_MAX) streak = COMPANION_STREAK_DISPLAY_MAX;
    snprintf(out, capacity, "%luD", (unsigned long)streak);
}
