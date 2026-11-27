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
        progress->missed_day = false; /* met today: sick recovers to full */
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
        /* Yesterday met (counted or not yet counted) keeps the pet healthy. */
        bool met_yesterday = progress->goal_met_today ||
                             progress->completed >= progress->goal;
        progress->missed_day = !met_yesterday;
        if (!progress->goal_met_today) {
            if (progress->completed >= progress->goal) {
                if (progress->streak < COMPANION_STREAK_STORE_MAX) progress->streak++;
            } else {
                progress->streak = 0;
            }
        }
    } else {
        progress->streak = 0;
        progress->missed_day = true;
    }
    progress->completed = 0;
    progress->goal_met_today = false;
    progress->day_key = today;
}

companion_vitality_t companion_progress_vitality(const companion_progress_t *progress,
                                                 bool time_valid)
{
    if (!progress) return COMPANION_VITALITY_HUNGRY;
    if (progress->completed >= progress->goal) return COMPANION_VITALITY_FULL;
    /*
     * Never judge sick without a valid clock or before the first wall-clock
     * sync: a USB restart before NTP, or a brand-new pad, must not look sick.
     */
    if (!time_valid || progress->day_key == 0) return COMPANION_VITALITY_HUNGRY;
    if (progress->missed_day) return COMPANION_VITALITY_SICK;
    return COMPANION_VITALITY_HUNGRY;
}

const char *companion_vitality_name(companion_vitality_t vitality)
{
    switch (vitality) {
    case COMPANION_VITALITY_FULL: return "full";
    case COMPANION_VITALITY_SICK: return "sick";
    case COMPANION_VITALITY_HUNGRY:
    default: return "hungry";
    }
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
