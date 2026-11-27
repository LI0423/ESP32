#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "companion_progress.h"
#include "companion_schedule.h"
#include "companion_serial.h"
#include "companion_timer.h"
#include "deskpal_font.h"
#include "deskpal_sprites.h"

static struct tm make_local(int year, int month, int day, int hour, int minute)
{
    struct tm local = {0};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    return local;
}

static int64_t epoch_of(struct tm local)
{
    time_t value = mktime(&local);
    assert(value > 0);
    return (int64_t)value;
}

static void test_timer_cycle(void)
{
    companion_timer_t timer;
    companion_timer_reset(&timer);
    assert(timer.phase == COMPANION_PHASE_FOCUS);
    assert(timer.remaining_seconds == 1500);
    assert(timer.celebrate_seconds == 8);

    companion_timer_advance(&timer, 1499);
    assert(timer.phase == COMPANION_PHASE_FOCUS);
    assert(timer.remaining_seconds == 1);

    companion_timer_advance(&timer, 1);
    assert(timer.phase == COMPANION_PHASE_CELEBRATE);
    assert(timer.remaining_seconds == 8);
    assert(timer.completed_sessions == 1);

    companion_timer_advance(&timer, 8);
    assert(timer.phase == COMPANION_PHASE_BREAK);
    assert(timer.remaining_seconds == 300);

    companion_timer_advance(&timer, 300);
    assert(timer.phase == COMPANION_PHASE_FOCUS);
    assert(timer.remaining_seconds == 1500);

    companion_timer_reset(&timer);
    companion_timer_advance(&timer, 1500 + 8 + 300 + 10);
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

    companion_timer_enter_standby(&timer);
    assert(timer.phase == COMPANION_PHASE_STANDBY);
    assert(timer.remaining_seconds == 0);
    companion_timer_advance(&timer, 100);
    assert(timer.phase == COMPANION_PHASE_STANDBY);
    assert(timer.completed_sessions == 0);

    companion_timer_start_focus(&timer);
    assert(timer.phase == COMPANION_PHASE_FOCUS);
    assert(timer.remaining_seconds == 1500);
}

static void test_work_hours_boundaries(void)
{
    assert(!companion_is_work_hours_minute(7 * 60 + 59));
    assert(companion_is_work_hours_minute(8 * 60));
    assert(companion_is_work_hours_minute(12 * 60));
    assert(companion_is_work_hours_minute(21 * 60 + 59));
    assert(!companion_is_work_hours_minute(22 * 60));
    assert(!companion_is_work_hours_minute(0));
    assert(!companion_is_work_hours_minute(23 * 60 + 30));

    struct tm morning = make_local(2026, 11, 25, 8, 0);
    struct tm evening = make_local(2026, 11, 25, 22, 0);
    assert(companion_local_minute_of_day(&morning) == 8 * 60);
    assert(companion_local_minute_of_day(&evening) == 22 * 60);
}

static void test_manual_standby_cross_midnight(void)
{
    struct tm evening = make_local(2026, 11, 25, 21, 0);
    int64_t evening_epoch = epoch_of(evening);
    int64_t until = companion_next_work_start_epoch(evening_epoch, &evening);
    time_t until_time = (time_t)until;
    struct tm until_tm = {0};
    assert(localtime_r(&until_time, &until_tm));
    assert(until_tm.tm_hour == 8 && until_tm.tm_min == 0);
    assert(until_tm.tm_mday == 26);

    /* Still active at 00:30 same night — must not clear at midnight. */
    struct tm midnightish = make_local(2026, 11, 26, 0, 30);
    int64_t midnight_epoch = epoch_of(midnightish);
    assert(companion_manual_standby_active(midnight_epoch, until));
    assert(!companion_should_auto_resume(true, companion_local_minute_of_day(&midnightish),
                                         true, false, midnight_epoch, until));

    /* 01:00 trigger also holds until the upcoming 08:00 (same calendar morning). */
    struct tm late = make_local(2026, 11, 26, 1, 0);
    int64_t late_epoch = epoch_of(late);
    int64_t until_late = companion_next_work_start_epoch(late_epoch, &late);
    time_t until_late_time = (time_t)until_late;
    struct tm until_late_tm = {0};
    assert(localtime_r(&until_late_time, &until_late_tm));
    assert(until_late_tm.tm_mday == 26 && until_late_tm.tm_hour == 8);
    assert(companion_manual_standby_active(late_epoch, until_late));

    /* Clears at next work start, not earlier. */
    assert(!companion_manual_standby_active(until, until));
    assert(companion_should_auto_resume(true, 8 * 60, true, false, until, until));
}

static void test_boot_restore_gaps(void)
{
    int work = 10 * 60;
    int night = 23 * 60;
    int64_t now = 1800000000LL;
    int64_t fresh = now - 10 * 60;
    int64_t stale = now - 31 * 60;

    assert(companion_boot_restore_action(false, now, fresh, work) ==
           COMPANION_BOOT_SAFE_NO_CLOCK);
    assert(companion_boot_restore_action(true, 100, fresh, work) ==
           COMPANION_BOOT_SAFE_NO_CLOCK);

    assert(companion_boot_restore_action(true, now, fresh, work) == COMPANION_BOOT_KEEP);
    assert(companion_boot_restore_action(true, now, fresh, night) == COMPANION_BOOT_KEEP);

    uint32_t completed_before = 4;
    companion_timer_t timer;
    companion_timer_restore(&timer, COMPANION_PHASE_FOCUS, 900, completed_before);
    companion_boot_action_t long_work =
        companion_boot_restore_action(true, now, stale, work);
    assert(long_work == COMPANION_BOOT_FRESH_FOCUS);
    companion_timer_start_focus(&timer);
    assert(timer.phase == COMPANION_PHASE_FOCUS);
    assert(timer.remaining_seconds == 1500);
    assert(timer.completed_sessions == completed_before); /* no invented rounds */

    companion_boot_action_t long_night =
        companion_boot_restore_action(true, now, stale, night);
    assert(long_night == COMPANION_BOOT_ENTER_STANDBY);
    companion_timer_enter_standby(&timer);
    assert(timer.phase == COMPANION_PHASE_STANDBY);
    assert(timer.completed_sessions == completed_before);

    /* Short gap outside hours → keep phase, then schedule may standby. */
    assert(companion_should_auto_standby(true, night, false, now, 0));
    assert(!companion_should_auto_standby(true, work, false, now, 0));
    assert(!companion_should_auto_standby(false, night, false, now, 0));
    assert(!companion_should_auto_standby(true, night, true, now, 0)); /* override */
}

static void test_today_day_key(void)
{
    struct tm day_a = make_local(2026, 11, 25, 23, 59);
    struct tm day_b = make_local(2026, 11, 26, 0, 0);
    assert(companion_local_day_key(&day_a) == 20261125);
    assert(companion_local_day_key(&day_b) == 20261126);
    assert(companion_local_day_key(&day_a) != companion_local_day_key(&day_b));
}

static void test_offline_ui_copy(void)
{
    assert(strcmp(companion_phase_phrase(COMPANION_PHASE_FOCUS), "Waiting for Wi-Fi") != 0);
    assert(strcmp(companion_phase_phrase(COMPANION_PHASE_BREAK), "Waiting for Wi-Fi") != 0);
    assert(strcmp(companion_phase_phrase(COMPANION_PHASE_CELEBRATE), "Waiting for Wi-Fi") != 0);
    assert(strcmp(companion_phase_phrase(COMPANION_PHASE_STANDBY), "Quiet until morning") == 0);
    assert(strcmp(companion_phase_label(COMPANION_PHASE_FOCUS), "FOCUS") == 0);
    assert(strcmp(companion_phase_name(COMPANION_PHASE_STANDBY), "standby") == 0);
    assert(companion_effective_brightness(75, true) == COMPANION_STANDBY_BRIGHTNESS);
    assert(companion_effective_brightness(75, false) == 75);
}

static companion_progress_t progress_with(uint32_t completed, uint32_t goal, uint32_t streak,
                                          int32_t day_key, bool goal_met_today)
{
    companion_progress_t progress;
    companion_progress_reset(&progress);
    progress.completed = completed;
    progress.goal = goal;
    progress.streak = streak;
    progress.day_key = day_key;
    progress.goal_met_today = goal_met_today;
    return progress;
}

static companion_progress_t progress_with_missed(uint32_t completed, uint32_t goal,
                                                 int32_t day_key, bool missed_day)
{
    companion_progress_t progress = progress_with(completed, goal, 0, day_key, false);
    progress.missed_day = missed_day;
    return progress;
}

static void test_vitality_states(void)
{
    /* Met yesterday, midnight passes: Today 0/4, 1D-ish streak, hungry not sick. */
    companion_progress_t progress = progress_with(4, 4, 6, 20261125, true);
    companion_progress_roll_day(&progress, 20261126, 1);
    assert(progress.completed == 0 && !progress.goal_met_today);
    assert(progress.streak == 6);
    assert(!progress.missed_day);
    assert(companion_progress_vitality(&progress, true) == COMPANION_VITALITY_HUNGRY);

    /* Yesterday ended short of goal -> sick. */
    progress = progress_with(3, 4, 5, 20261125, false);
    companion_progress_roll_day(&progress, 20261126, 1);
    assert(progress.streak == 0);
    assert(progress.missed_day);
    assert(companion_progress_vitality(&progress, true) == COMPANION_VITALITY_SICK);
    /* Staying on the same day keeps it sick until the goal is met. */
    companion_progress_sync(&progress, 2);
    assert(companion_progress_vitality(&progress, true) == COMPANION_VITALITY_SICK);

    /* Meeting today's goal recovers to full and clears the flag. */
    companion_progress_sync(&progress, 4);
    assert(progress.missed_day == false);
    assert(progress.goal_met_today);
    assert(companion_progress_vitality(&progress, true) == COMPANION_VITALITY_FULL);

    /* A skipped day (gap > 1) is a miss even if yesterday looked met. */
    progress = progress_with(4, 4, 6, 20261125, true);
    companion_progress_roll_day(&progress, 20261127, 2);
    assert(progress.missed_day);
    assert(companion_progress_vitality(&progress, true) == COMPANION_VITALITY_SICK);

    /* First use / first sync never looks sick, even with no streak. */
    progress = progress_with(0, 4, 0, 0, false);
    assert(companion_progress_vitality(&progress, true) == COMPANION_VITALITY_HUNGRY);
    assert(companion_progress_vitality(&progress, false) == COMPANION_VITALITY_HUNGRY);
    companion_progress_roll_day(&progress, 20261126, 0);
    assert(!progress.missed_day);
    assert(companion_progress_vitality(&progress, true) == COMPANION_VITALITY_HUNGRY);

    /* Invalid clock (USB restart before NTP): do not judge sick. */
    progress = progress_with_missed(0, 4, 20261126, true);
    assert(companion_progress_vitality(&progress, false) == COMPANION_VITALITY_HUNGRY);
    /* A met goal is full regardless of the clock. */
    progress.completed = 4;
    assert(companion_progress_vitality(&progress, false) == COMPANION_VITALITY_FULL);

    /* Backward clock step does not invent a miss. */
    progress = progress_with(4, 4, 6, 20261126, true);
    companion_progress_roll_day(&progress, 20261125, -1);
    assert(!progress.missed_day);
    assert(companion_progress_vitality(&progress, true) == COMPANION_VITALITY_HUNGRY);

    /* Lowering the goal onto today's count recovers a sick pet to full. */
    progress = progress_with_missed(2, 4, 20261126, true);
    companion_progress_set_goal(&progress, 2);
    assert(!progress.missed_day);
    assert(companion_progress_vitality(&progress, true) == COMPANION_VITALITY_FULL);

    assert(strcmp(companion_vitality_name(COMPANION_VITALITY_FULL), "full") == 0);
    assert(strcmp(companion_vitality_name(COMPANION_VITALITY_HUNGRY), "hungry") == 0);
    assert(strcmp(companion_vitality_name(COMPANION_VITALITY_SICK), "sick") == 0);
}

static void test_goal_default_and_change(void)
{
    companion_progress_t progress;
    companion_progress_reset(&progress);
    assert(progress.goal == COMPANION_DAILY_GOAL_DEFAULT);
    assert(progress.goal == 4);
    assert(progress.streak == 0);
    assert(!progress.goal_met_today);
    assert(companion_progress_sanitize_goal(0) == 4);
    assert(companion_progress_sanitize_goal(100) == 4);
    assert(companion_progress_sanitize_goal(1) == 1);
    assert(companion_progress_sanitize_goal(99) == 99);

    companion_progress_set_goal(&progress, 6);
    assert(progress.goal == 6);
    assert(progress.streak == 0);

    /* Lowering the goal under today's count counts today once. */
    progress.completed = 3;
    progress.streak = 5;
    companion_progress_set_goal(&progress, 3);
    assert(progress.goal == 3);
    assert(progress.streak == 6);
    assert(progress.goal_met_today);

    /* Raising it back above today's count removes only today's point. */
    companion_progress_set_goal(&progress, 8);
    assert(progress.goal == 8);
    assert(progress.streak == 5);
    assert(!progress.goal_met_today);

    /* A stored out-of-range value falls back to the default, not a wipe. */
    companion_progress_set_goal(&progress, 0);
    assert(progress.goal == 4);
}

static void test_streak_on_met_and_missed_day(void)
{
    /* Mock: 5D with Today 3/4 becomes 6D at Today 4/4, and extra rounds stay 6D. */
    companion_progress_t progress = progress_with(3, 4, 5, 20261125, false);
    companion_progress_sync(&progress, 4);
    assert(progress.streak == 6);
    assert(progress.goal_met_today);
    assert(progress.completed == 4);
    companion_progress_sync(&progress, 5);
    assert(progress.streak == 6);
    assert(progress.completed == 5);

    char today[24];
    char streak[8];
    companion_progress_format_today(today, sizeof(today), 3, 4);
    companion_progress_format_streak(streak, sizeof(streak), 5);
    assert(strcmp(today, "Today 3/4") == 0);
    assert(strcmp(streak, "5D") == 0);
    companion_progress_format_today(today, sizeof(today), 5, 4);
    companion_progress_format_streak(streak, sizeof(streak), 6);
    assert(strcmp(today, "Today 5/4") == 0);
    assert(strcmp(streak, "6D") == 0);
    companion_progress_format_streak(streak, sizeof(streak), 1000);
    assert(strcmp(streak, "999D") == 0);
    companion_progress_format_streak(streak, sizeof(streak), 0);
    assert(strcmp(streak, "0D") == 0);

    /* The day ends short of the goal: the streak clears. */
    progress = progress_with(3, 4, 5, 20261125, false);
    companion_progress_roll_day(&progress, 20261126, 1);
    assert(progress.streak == 0);
    assert(progress.completed == 0);
    assert(!progress.goal_met_today);
    assert(progress.day_key == 20261126);

    /* Met yesterday and already counted: the next morning keeps the streak. */
    progress = progress_with(4, 4, 6, 20261125, true);
    companion_progress_roll_day(&progress, 20261126, 1);
    assert(progress.streak == 6);
    assert(progress.completed == 0);
    assert(!progress.goal_met_today);

    /* Met yesterday but the increment was not saved: count it once at the boundary. */
    progress = progress_with(4, 4, 5, 20261125, false);
    companion_progress_roll_day(&progress, 20261126, 1);
    assert(progress.streak == 6);
    assert(progress.completed == 0);
    assert(!progress.goal_met_today);

    /* Two calendar days later, the empty day in between breaks the streak. */
    progress = progress_with(4, 4, 6, 20261125, true);
    companion_progress_roll_day(&progress, 20261127, 2);
    assert(progress.streak == 0);
    assert(progress.day_key == 20261127);
    assert(progress.completed == 0);
}

static void test_day_boundary_gap(void)
{
    assert(companion_day_key_gap(20261125, 20261125) == 0);
    assert(companion_day_key_gap(20261125, 20261126) == 1);
    assert(companion_day_key_gap(20261130, 20261201) == 1);
    assert(companion_day_key_gap(20261231, 20270101) == 1);
    assert(companion_day_key_gap(20260228, 20260301) == 1);
    assert(companion_day_key_gap(20240228, 20240229) == 1);
    assert(companion_day_key_gap(20240228, 20240301) == 2);
    assert(companion_day_key_gap(20261126, 20261125) == -1);
    assert(companion_day_key_gap(20261131, 20261201) == -1);

    /* First wall-clock sync adopts the day and keeps today's rounds, without a miss. */
    companion_progress_t progress = progress_with(2, 4, 0, 0, false);
    companion_progress_roll_day(&progress, 20261125, 0);
    assert(progress.day_key == 20261125);
    assert(progress.completed == 2);
    assert(progress.streak == 0);
    assert(!progress.goal_met_today);

    /* Rounds finished before the first sync stay on the local day after sync. */
    progress = progress_with(3, 4, 0, 0, false);
    companion_progress_roll_day(&progress, 20261126, 0);
    assert(progress.day_key == 20261126);
    assert(progress.completed == 3);
    assert(progress.streak == 0);
    assert(!progress.goal_met_today);

    /* If the goal was already reached, first sync counts that day once. */
    progress = progress_with(4, 4, 0, 0, false);
    companion_progress_roll_day(&progress, 20261126, 0);
    assert(progress.day_key == 20261126);
    assert(progress.completed == 4);
    assert(progress.streak == 1);
    assert(progress.goal_met_today);

    /* Same local day only refreshes the count; it does not clear a live streak. */
    progress = progress_with(3, 4, 5, 20261125, false);
    companion_progress_roll_day(&progress, 20261125, 0);
    assert(progress.streak == 5);
    assert(progress.completed == 3);
    assert(progress.day_key == 20261125);

    /* A backwards step clears today's count and does not invent a miss or a point. */
    progress = progress_with(4, 4, 6, 20261126, true);
    companion_progress_roll_day(&progress, 20261125, -1);
    assert(progress.streak == 6);
    assert(progress.completed == 0);
    assert(!progress.goal_met_today);
    assert(progress.day_key == 20261125);
}

static void test_hud_anchors_unchanged(void)
{
    assert(DESKPAL_CHAR_W == 128 && DESKPAL_CHAR_H == 128);
    assert(DESKPAL_CHAR_X == 56 && DESKPAL_CHAR_Y == 43);
    assert(DESKPAL_STAGE_CX == 120 && DESKPAL_STAGE_CY == 20);
    assert(DESKPAL_TIME_CX == 120 && DESKPAL_TIME_CY == 198);
    assert(DESKPAL_TODAY_CX == 120 && DESKPAL_TODAY_CY == 219);
    assert(DESKPAL_DOT_CX == 222 && DESKPAL_DOT_CY == 18 && DESKPAL_DOT_R == 5);
    assert(DESKPAL_STREAK_X == 10 && DESKPAL_STREAK_CY == 20);
    /* 8px glyph, cy 20 → top y 16. "999D" is 32px, so the reserved box ends at x 42. */
    assert(DESKPAL_FONT_W == 8 && DESKPAL_FONT_H == 8);
    assert(DESKPAL_STREAK_CY - DESKPAL_FONT_H / 2 == 16);
    assert(DESKPAL_STREAK_X + 4 * DESKPAL_FONT_W == 42);

    assert(DESKPAL_SPRITE_COUNT == 14);
    for (int id = 0; id < DESKPAL_SPRITE_COUNT; id++) {
        assert(deskpal_sprite_data((deskpal_sprite_id_t)id) != NULL);
        assert(deskpal_sprite_palette((deskpal_sprite_id_t)id) != NULL);
    }
    assert(deskpal_sprite_data(DESKPAL_SPRITE_BABY) != deskpal_sprite_data(DESKPAL_SPRITE_ADULT));
    assert(deskpal_sprite_palette(DESKPAL_SPRITE_BABY) == deskpal_sprite_palette(DESKPAL_SPRITE_FOCUS));
    assert(deskpal_sick_pill != NULL);
    assert(deskpal_hungry_rice != NULL);
}

static void test_manual_focus_override_boundary(void)
{
    int64_t now = 1800000000LL;
    int night = 23 * 60;
    /* Night + override → do not snap back to standby. */
    assert(!companion_should_auto_standby(true, night, true, now, 0));
    /* Manual standby still wins over override once standby_until is set. */
    int64_t until = now + 8 * 3600;
    assert(companion_should_auto_standby(true, night, false, now, until));
    assert(!companion_should_auto_resume(true, night, true, true, now, until));
}

static void test_serial_commands(void)
{
    companion_serial_result_t result;

    companion_serial_parse("goal 1", &result);
    assert(result.command == COMPANION_SERIAL_CMD_GOAL && result.goal == 1);
    companion_serial_parse("goal 99", &result);
    assert(result.command == COMPANION_SERIAL_CMD_GOAL && result.goal == 99);
    companion_serial_parse("goal 4", &result);
    assert(result.command == COMPANION_SERIAL_CMD_GOAL && result.goal == 4);
    companion_serial_parse("focus", &result);
    assert(result.command == COMPANION_SERIAL_CMD_FOCUS);
    companion_serial_parse("standby", &result);
    assert(result.command == COMPANION_SERIAL_CMD_STANDBY);
    companion_serial_parse("feed", &result);
    assert(result.command == COMPANION_SERIAL_CMD_FEED);
    companion_serial_parse("medicine", &result);
    assert(result.command == COMPANION_SERIAL_CMD_MEDICINE);
    companion_serial_parse("play", &result);
    assert(result.command == COMPANION_SERIAL_CMD_PLAY);
    companion_serial_parse("clean", &result);
    assert(result.command == COMPANION_SERIAL_CMD_CLEAN);
    companion_serial_parse("token", &result);
    assert(result.command == COMPANION_SERIAL_CMD_TOKEN);
    companion_serial_parse("status", &result);
    assert(result.command == COMPANION_SERIAL_CMD_STATUS);

    /* CRLF and surrounding whitespace are tolerated. */
    companion_serial_parse("goal 6\r\n", &result);
    assert(result.command == COMPANION_SERIAL_CMD_GOAL && result.goal == 6);
    companion_serial_parse("  goal\t7  ", &result);
    assert(result.command == COMPANION_SERIAL_CMD_GOAL && result.goal == 7);
    companion_serial_parse("\r\n", &result);
    assert(result.command == COMPANION_SERIAL_CMD_UNKNOWN);
    companion_serial_parse("  status  \r\n", &result);
    assert(result.command == COMPANION_SERIAL_CMD_STATUS);

    /* Rejected goals change nothing. */
    static const char *bad_goals[] = {
        "goal 0", "goal 100", "goal", "goal abc", "goal 6 1", "goal -1", "goal 1e2", "goal 000",
    };
    for (size_t i = 0; i < sizeof(bad_goals) / sizeof(bad_goals[0]); i++) {
        companion_serial_parse(bad_goals[i], &result);
        assert(result.command == COMPANION_SERIAL_CMD_INVALID);
        assert(result.goal == 0);
    }

    /* Unknown lines are ignored. */
    static const char *unknown[] = {"hello", "", "   ", "goalkeeper", "goalf 6"};
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
        companion_serial_parse(unknown[i], &result);
        assert(result.command == COMPANION_SERIAL_CMD_UNKNOWN);
    }

    companion_serial_parse("feed now", &result);
    assert(result.command == COMPANION_SERIAL_CMD_INVALID);
    companion_serial_parse("medicine 1", &result);
    assert(result.command == COMPANION_SERIAL_CMD_INVALID);
}

int main(void)
{
    test_timer_cycle();
    test_work_hours_boundaries();
    test_manual_standby_cross_midnight();
    test_boot_restore_gaps();
    test_today_day_key();
    test_offline_ui_copy();
    test_goal_default_and_change();
    test_streak_on_met_and_missed_day();
    test_day_boundary_gap();
    test_vitality_states();
    test_hud_anchors_unchanged();
    test_manual_focus_override_boundary();
    test_serial_commands();
    puts("companion timer tests passed");
    return 0;
}
