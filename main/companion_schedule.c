#include "companion_schedule.h"

#include <time.h>

bool companion_time_plausible(int64_t epoch)
{
    return epoch > COMPANION_EPOCH_MIN;
}

bool companion_is_work_hours_minute(int minute_of_day)
{
    return minute_of_day >= COMPANION_WORK_START_MIN &&
           minute_of_day < COMPANION_WORK_END_MIN;
}

int companion_local_minute_of_day(const struct tm *local)
{
    if (!local) return 0;
    int hour = local->tm_hour;
    int minute = local->tm_min;
    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    if (minute < 0) minute = 0;
    if (minute > 59) minute = 59;
    return hour * 60 + minute;
}

int32_t companion_local_day_key(const struct tm *local)
{
    if (!local) return 0;
    int year = local->tm_year + 1900;
    int month = local->tm_mon + 1;
    int day = local->tm_mday;
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    return (int32_t)(year * 10000 + month * 100 + day);
}

static bool split_day_key(int32_t key, int *year, int *month, int *day)
{
    if (key < 19700101 || key > 21001231) return false;
    int y = (int)(key / 10000);
    int m = (int)((key / 100) % 100);
    int d = (int)(key % 100);
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    *year = y;
    *month = m;
    *day = d;
    return true;
}

static bool noon_epoch(int year, int month, int day, time_t *out)
{
    struct tm local = {0};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = 12;
    local.tm_isdst = -1;
    time_t value = mktime(&local);
    if (value < 0) return false;
    if (local.tm_year != year - 1900 || local.tm_mon != month - 1 || local.tm_mday != day)
        return false;
    *out = value;
    return true;
}

int companion_day_key_gap(int32_t from_key, int32_t to_key)
{
    int from_year, from_month, from_day;
    int to_year, to_month, to_day;
    if (!split_day_key(from_key, &from_year, &from_month, &from_day) ||
        !split_day_key(to_key, &to_year, &to_month, &to_day)) return -1;
    if (from_key == to_key) return 0;
    time_t from_time = 0;
    time_t to_time = 0;
    if (!noon_epoch(from_year, from_month, from_day, &from_time) ||
        !noon_epoch(to_year, to_month, to_day, &to_time)) return -1;
    int64_t seconds = (int64_t)to_time - (int64_t)from_time;
    if (seconds <= 0) return -1;
    /* Noon-to-noon is 86400s in CST-8. Round so a DST hour does not drop a day. */
    int days = (int)((seconds + 43200) / 86400);
    return days > 0 ? days : -1;
}

int64_t companion_next_work_start_epoch(int64_t now_epoch, const struct tm *local)
{
    if (!local || !companion_time_plausible(now_epoch)) return 0;
    struct tm boundary = *local;
    boundary.tm_hour = 8;
    boundary.tm_min = 0;
    boundary.tm_sec = 0;
    boundary.tm_isdst = -1;
    if (companion_local_minute_of_day(local) >= COMPANION_WORK_START_MIN)
        boundary.tm_mday += 1;
    time_t target = mktime(&boundary);
    if (target < 0) return 0;
    return (int64_t)target;
}

companion_boot_action_t companion_boot_restore_action(bool time_valid,
                                                      int64_t now_epoch,
                                                      int64_t saved_epoch,
                                                      int minute_of_day)
{
    if (!time_valid || !companion_time_plausible(now_epoch))
        return COMPANION_BOOT_SAFE_NO_CLOCK;

    if (!companion_time_plausible(saved_epoch) || now_epoch < saved_epoch)
        return companion_is_work_hours_minute(minute_of_day)
                   ? COMPANION_BOOT_FRESH_FOCUS
                   : COMPANION_BOOT_ENTER_STANDBY;

    int64_t gap = now_epoch - saved_epoch;
    if (gap < (int64_t)COMPANION_STALE_SAVE_SECONDS)
        return COMPANION_BOOT_KEEP;

    return companion_is_work_hours_minute(minute_of_day)
               ? COMPANION_BOOT_FRESH_FOCUS
               : COMPANION_BOOT_ENTER_STANDBY;
}

bool companion_manual_standby_active(int64_t now_epoch, int64_t standby_until_epoch)
{
    return companion_time_plausible(standby_until_epoch) &&
           companion_time_plausible(now_epoch) &&
           now_epoch < standby_until_epoch;
}

bool companion_should_auto_standby(bool time_valid, int minute_of_day,
                                   bool work_hours_override,
                                   int64_t now_epoch,
                                   int64_t standby_until_epoch)
{
    if (!time_valid || !companion_time_plausible(now_epoch)) return false;
    if (work_hours_override) return false;
    if (companion_manual_standby_active(now_epoch, standby_until_epoch)) return true;
    return !companion_is_work_hours_minute(minute_of_day);
}

bool companion_should_auto_resume(bool time_valid, int minute_of_day,
                                  bool in_standby,
                                  bool work_hours_override,
                                  int64_t now_epoch,
                                  int64_t standby_until_epoch)
{
    (void)work_hours_override;
    if (!in_standby || !time_valid || !companion_time_plausible(now_epoch)) return false;
    if (companion_manual_standby_active(now_epoch, standby_until_epoch)) return false;
    return companion_is_work_hours_minute(minute_of_day);
}

uint8_t companion_effective_brightness(uint8_t configured, bool standby)
{
    if (!standby) {
        if (configured < 5) return 5;
        if (configured > 100) return 100;
        return configured;
    }
    return COMPANION_STANDBY_BRIGHTNESS;
}
