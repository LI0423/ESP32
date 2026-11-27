#include "companion_pet.h"

#include <string.h>
#include <time.h>
#include "companion_schedule.h"

static uint8_t clamp_u8(int value)
{
    if (value < 0) return 0;
    if (value > 100) return 100;
    return (uint8_t)value;
}

static uint32_t clamp_xp(uint32_t value)
{
    return value > COMPANION_PET_XP_MAX ? COMPANION_PET_XP_MAX : value;
}

static companion_pet_stage_t stage_for(const companion_pet_t *pet)
{
    if (pet->age_days >= 7 && pet->xp >= 600) return COMPANION_PET_STAGE_ADULT;
    if (pet->age_days >= 3 && pet->xp >= 300) return COMPANION_PET_STAGE_TEEN;
    if (pet->age_days >= 1 && pet->xp >= 100) return COMPANION_PET_STAGE_CHILD;
    return COMPANION_PET_STAGE_BABY;
}

static void update_stage(companion_pet_t *pet)
{
    if (pet) pet->stage = (uint8_t)stage_for(pet);
}

static void make_sick(companion_pet_t *pet, uint8_t reason)
{
    if (!pet || pet->sick) return;
    pet->sick = true;
    pet->sick_reason = reason;
    if (pet->health > 10) pet->health = 10;
}

static bool epoch_valid_or_zero(int64_t epoch)
{
    return epoch == 0 || (companion_time_plausible(epoch) && epoch <= COMPANION_PET_EPOCH_MAX);
}

static bool cooldown_active(int64_t last_epoch, int64_t now_epoch, uint32_t seconds)
{
    return last_epoch > 0 && (now_epoch < last_epoch ||
           (uint64_t)(now_epoch - last_epoch) < seconds);
}

static bool action_time_ready(const companion_pet_t *pet, bool time_valid, int64_t now_epoch)
{
    return time_valid && companion_time_plausible(now_epoch) &&
           now_epoch <= COMPANION_PET_EPOCH_MAX &&
           (pet->last_update_epoch == 0 || now_epoch >= pet->last_update_epoch);
}

void companion_pet_reset(companion_pet_t *pet)
{
    if (!pet) return;
    memset(pet, 0, sizeof(*pet));
    pet->satiety = 80;
    pet->health = 100;
    pet->mood = 80;
    pet->hygiene = 100;
    pet->food = 5;
    pet->medicine = 2;
}

bool companion_pet_valid(const companion_pet_t *pet)
{
    if (!pet) return false;
    if (pet->satiety > 100 || pet->health > 100 || pet->health < 10 ||
        pet->mood > 100 || pet->hygiene > 100)
        return false;
    if (pet->food > COMPANION_PET_FOOD_MAX || pet->medicine > COMPANION_PET_MEDICINE_MAX)
        return false;
    if (pet->xp > COMPANION_PET_XP_MAX || pet->focus_count > COMPANION_PET_XP_MAX)
        return false;
    if (pet->born_day != 0 &&
        (pet->born_day < 19700101 || pet->born_day > 21001231 ||
         companion_day_key_gap(pet->born_day, pet->born_day) != 0))
        return false;
    if (pet->age_days > 36500 || pet->stage != (uint8_t)stage_for(pet))
        return false;
    if (pet->sick_reason > COMPANION_PET_SICK_DIRT)
        return false;
    if (pet->sick && pet->sick_reason == COMPANION_PET_SICK_NONE) return false;
    if (!pet->sick && pet->sick_reason != COMPANION_PET_SICK_NONE) return false;
    if (pet->overfeed_count > 10)
        return false;
    if (!epoch_valid_or_zero(pet->last_fed_epoch) ||
        !epoch_valid_or_zero(pet->last_medicine_epoch) ||
        !epoch_valid_or_zero(pet->last_played_epoch) ||
        !epoch_valid_or_zero(pet->last_cleaned_epoch) ||
        !epoch_valid_or_zero(pet->last_update_epoch) ||
        !epoch_valid_or_zero(pet->hungry_since_epoch) ||
        !epoch_valid_or_zero(pet->dirty_since_epoch) ||
        !epoch_valid_or_zero(pet->overfeed_window_epoch))
        return false;
    if (pet->satiety_carry_seconds >= 1800 || pet->health_carry_seconds >= 3600 ||
        pet->hygiene_carry_seconds >= 7200 || pet->mood_carry_seconds >= 7200)
        return false;
    return true;
}

void companion_pet_sanitize(companion_pet_t *pet)
{
    if (!pet) return;
    pet->satiety = pet->satiety > 100 ? 100 : pet->satiety;
    pet->health = pet->health > 100 ? 100 : pet->health;
    if (pet->health < 10) pet->health = 10;
    pet->mood = pet->mood > 100 ? 100 : pet->mood;
    pet->hygiene = pet->hygiene > 100 ? 100 : pet->hygiene;
    if (pet->food > COMPANION_PET_FOOD_MAX) pet->food = COMPANION_PET_FOOD_MAX;
    if (pet->medicine > COMPANION_PET_MEDICINE_MAX) pet->medicine = COMPANION_PET_MEDICINE_MAX;
    pet->xp = clamp_xp(pet->xp);
    if (pet->focus_count > COMPANION_PET_XP_MAX) pet->focus_count = COMPANION_PET_XP_MAX;
    if (pet->born_day < 0 || pet->born_day > 21001231 ||
        companion_day_key_gap(pet->born_day, pet->born_day) != 0)
        pet->born_day = 0;
    if (pet->age_days > 36500) pet->age_days = 36500;
    if (pet->sick_reason > COMPANION_PET_SICK_DIRT) pet->sick_reason = COMPANION_PET_SICK_NONE;
    if (!pet->sick) pet->sick_reason = COMPANION_PET_SICK_NONE;
    else if (pet->sick_reason == COMPANION_PET_SICK_NONE)
        pet->sick_reason = COMPANION_PET_SICK_STARVATION;
    if (pet->overfeed_count > 10) pet->overfeed_count = 10;
    if (pet->last_fed_epoch < 0) pet->last_fed_epoch = 0;
    if (pet->last_medicine_epoch < 0) pet->last_medicine_epoch = 0;
    if (pet->last_played_epoch < 0) pet->last_played_epoch = 0;
    if (pet->last_cleaned_epoch < 0) pet->last_cleaned_epoch = 0;
    if (pet->last_update_epoch < 0) pet->last_update_epoch = 0;
    if (pet->hungry_since_epoch < 0) pet->hungry_since_epoch = 0;
    if (pet->dirty_since_epoch < 0) pet->dirty_since_epoch = 0;
    if (pet->overfeed_window_epoch < 0) pet->overfeed_window_epoch = 0;
    if (pet->satiety_carry_seconds >= 1800) pet->satiety_carry_seconds = 0;
    if (pet->health_carry_seconds >= 3600) pet->health_carry_seconds = 0;
    if (pet->hygiene_carry_seconds >= 7200) pet->hygiene_carry_seconds = 0;
    if (pet->mood_carry_seconds >= 7200) pet->mood_carry_seconds = 0;
    update_stage(pet);
}

bool companion_pet_rebase_future(companion_pet_t *pet, int64_t now_epoch)
{
    if (!pet || !companion_time_plausible(now_epoch) ||
        now_epoch > COMPANION_PET_EPOCH_MAX) return false;
    bool changed = false;
#define REBASE_FUTURE(field) \
    do { if (pet->field > now_epoch) { pet->field = now_epoch; changed = true; } } while (0)
    REBASE_FUTURE(last_fed_epoch);
    REBASE_FUTURE(last_medicine_epoch);
    REBASE_FUTURE(last_played_epoch);
    REBASE_FUTURE(last_cleaned_epoch);
    REBASE_FUTURE(last_update_epoch);
    REBASE_FUTURE(hungry_since_epoch);
    REBASE_FUTURE(dirty_since_epoch);
    REBASE_FUTURE(overfeed_window_epoch);
#undef REBASE_FUTURE
    return changed;
}

void companion_pet_roll_day(companion_pet_t *pet, int32_t today, int gap_days)
{
    (void)gap_days; /* Age is derived from born_day, never incrementally added. */
    if (!pet || today <= 0 || companion_day_key_gap(today, today) != 0) return;
    if (pet->born_day == 0) {
        pet->born_day = today;
        pet->age_days = 0;
        update_stage(pet);
        return;
    }
    int age = companion_day_key_gap(pet->born_day, today);
    if (age < 0) return; /* Backward clock/day correction does not change age. */
    pet->age_days = age > 36500 ? 36500u : (uint32_t)age;
    update_stage(pet);
}

uint32_t companion_pet_age_days(const companion_pet_t *pet, int32_t today)
{
    if (!pet || pet->born_day == 0 || today <= 0) return 0;
    int gap = companion_day_key_gap(pet->born_day, today);
    return gap > 0 ? (uint32_t)gap : 0;
}

bool companion_pet_tick(companion_pet_t *pet, uint32_t elapsed_seconds, bool sleeping,
                        bool time_valid, int64_t now_epoch)
{
    if (!pet || elapsed_seconds == 0 || !time_valid ||
        !companion_time_plausible(now_epoch) || now_epoch > COMPANION_PET_EPOCH_MAX) return false;
    if (elapsed_seconds > COMPANION_PET_OFFLINE_MAX_SECONDS)
        elapsed_seconds = COMPANION_PET_OFFLINE_MAX_SECONDS;

    uint8_t satiety_before = pet->satiety;
    uint8_t health_before = pet->health;
    uint8_t mood_before = pet->mood;
    uint8_t hygiene_before = pet->hygiene;
    bool sick_before = pet->sick;
    uint8_t stage_before = pet->stage;
    int64_t hungry_since_before = pet->hungry_since_epoch;
    int64_t dirty_since_before = pet->dirty_since_epoch;

    /* Keep the old baseline during a backward clock correction; resume once it catches up. */
    if (pet->last_update_epoch > 0 && now_epoch < pet->last_update_epoch) {
        return false;
    }

    pet->satiety_carry_seconds += elapsed_seconds;
    uint32_t satiety_period = sleeping ? 1800u : 900u;
    bool reached_zero = false;
    uint32_t zero_remaining = 0;
    while (pet->satiety_carry_seconds >= satiety_period) {
        pet->satiety_carry_seconds -= satiety_period;
        if (pet->satiety > 0) {
            pet->satiety--;
            if (pet->satiety == 0) {
                reached_zero = true;
                zero_remaining = pet->satiety_carry_seconds;
            }
        }
    }
    if (pet->satiety == 0) {
        if (reached_zero || pet->hungry_since_epoch == 0) {
            uint32_t remainder = reached_zero ? zero_remaining
                : pet->satiety_carry_seconds % satiety_period;
            pet->hungry_since_epoch = now_epoch - (int64_t)remainder;
        }
    } else {
        pet->hungry_since_epoch = 0;
    }

    pet->health_carry_seconds += elapsed_seconds;
    while (pet->health_carry_seconds >= 3600u) {
        pet->health_carry_seconds -= 3600u;
        if (pet->satiety == 0 || pet->satiety < 20) {
            if (pet->health > 10) pet->health--;
        } else if (!pet->sick && pet->satiety > 40 && pet->health < 100) {
            pet->health++;
        }
    }
    if (pet->satiety == 0 && pet->hungry_since_epoch > 0 &&
        now_epoch >= pet->hungry_since_epoch &&
        (uint64_t)(now_epoch - pet->hungry_since_epoch) >= 6u * 60u * 60u)
        make_sick(pet, COMPANION_PET_SICK_STARVATION);

    pet->hygiene_carry_seconds += elapsed_seconds;
    while (pet->hygiene_carry_seconds >= 7200u) {
        pet->hygiene_carry_seconds -= 7200u;
        if (pet->hygiene > 0) pet->hygiene--;
    }
    if (pet->hygiene < 20) {
        if (pet->dirty_since_epoch == 0) pet->dirty_since_epoch = now_epoch;
        if (now_epoch >= pet->dirty_since_epoch &&
            (uint64_t)(now_epoch - pet->dirty_since_epoch) >= 12u * 60u * 60u)
            make_sick(pet, COMPANION_PET_SICK_DIRT);
    } else if (pet->hygiene >= 40) {
        pet->dirty_since_epoch = 0;
    }

    if ((pet->satiety < 30 || pet->sick) && pet->mood > 0) {
        pet->mood_carry_seconds += elapsed_seconds;
        while (pet->mood_carry_seconds >= 7200u) {
            pet->mood_carry_seconds -= 7200u;
            if (pet->mood > 0) pet->mood--;
        }
    } else {
        /* Do not carry leftover decay time into the next bad spell. */
        pet->mood_carry_seconds = 0;
    }
    pet->last_update_epoch = now_epoch;
    update_stage(pet);
    return satiety_before != pet->satiety || health_before != pet->health ||
           mood_before != pet->mood || hygiene_before != pet->hygiene ||
           sick_before != pet->sick || stage_before != pet->stage ||
           hungry_since_before != pet->hungry_since_epoch ||
           dirty_since_before != pet->dirty_since_epoch;
}

static bool sleeping_at(int64_t epoch, bool fallback)
{
    time_t value = (time_t)epoch;
    struct tm local = {0};
    if (!localtime_r(&value, &local)) return fallback;
    return !companion_is_work_hours_minute(companion_local_minute_of_day(&local));
}

static int64_t next_sleep_boundary(int64_t epoch)
{
    time_t value = (time_t)epoch;
    struct tm local = {0};
    if (!localtime_r(&value, &local)) return epoch + 900;
    struct tm boundary = local;
    boundary.tm_min = 0;
    boundary.tm_sec = 0;
    boundary.tm_isdst = -1;
    if (local.tm_hour < 8) {
        boundary.tm_hour = 8;
    } else if (local.tm_hour >= 22) {
        boundary.tm_hour = 8;
        boundary.tm_mday += 1;
    } else {
        boundary.tm_hour = 22;
    }
    time_t target = mktime(&boundary);
    return target > value ? (int64_t)target : epoch + 900;
}

static void update_age_from_epoch(companion_pet_t *pet, int64_t now_epoch)
{
    time_t value = (time_t)now_epoch;
    struct tm local = {0};
    if (!localtime_r(&value, &local)) return;
    int32_t today = companion_local_day_key(&local);
    if (pet->born_day == 0) {
        companion_pet_roll_day(pet, today, 0);
        return;
    }
    int gap = companion_day_key_gap(pet->born_day, today);
    if (gap > 0) companion_pet_roll_day(pet, today, gap);
}

bool companion_pet_catch_up(companion_pet_t *pet, bool sleeping, bool time_valid,
                            int64_t now_epoch)
{
    if (!pet) return false;
    if (!time_valid || !companion_time_plausible(now_epoch) ||
        now_epoch > COMPANION_PET_EPOCH_MAX) return false;
    if (pet->last_update_epoch > 0 && now_epoch < pet->last_update_epoch) return false;
    if (pet->last_update_epoch == 0 || now_epoch == pet->last_update_epoch) {
        pet->last_update_epoch = now_epoch;
        if (pet->born_day != 0) return false;
        uint8_t stage_before = pet->stage;
        update_age_from_epoch(pet, now_epoch);
        return stage_before != pet->stage || pet->born_day != 0;
    }
    uint8_t stage_before = pet->stage;
    int32_t born_before = pet->born_day;
    if (pet->born_day == 0) update_age_from_epoch(pet, pet->last_update_epoch);
    uint64_t remaining = (uint64_t)(now_epoch - pet->last_update_epoch);
    if (remaining > COMPANION_PET_OFFLINE_MAX_SECONDS)
        remaining = COMPANION_PET_OFFLINE_MAX_SECONDS;
    bool changed = false;
    int64_t boundary = next_sleep_boundary(pet->last_update_epoch);
    while (remaining > 0) {
        uint32_t chunk = remaining > 900u ? 900u : (uint32_t)remaining;
        if (boundary > pet->last_update_epoch &&
            (uint64_t)(boundary - pet->last_update_epoch) < chunk)
            chunk = (uint32_t)(boundary - pet->last_update_epoch);
        int64_t chunk_epoch = pet->last_update_epoch + (int64_t)chunk;
        changed = companion_pet_tick(pet, chunk,
                                     sleeping_at(pet->last_update_epoch, sleeping),
                                     true, chunk_epoch) || changed;
        remaining -= chunk;
        if (chunk_epoch >= boundary) boundary = next_sleep_boundary(chunk_epoch);
    }
    update_age_from_epoch(pet, now_epoch);
    changed = changed || stage_before != pet->stage || born_before != pet->born_day;
    pet->last_update_epoch = now_epoch;
    return changed;
}

companion_pet_action_result_t companion_pet_feed(companion_pet_t *pet, bool time_valid,
                                                  int64_t now_epoch)
{
    if (!pet) return COMPANION_PET_ACTION_NO_FOOD;
    if (!action_time_ready(pet, time_valid, now_epoch))
        return COMPANION_PET_ACTION_NO_TIME;
    if (pet->food == 0) return COMPANION_PET_ACTION_NO_FOOD;
    if (cooldown_active(pet->last_fed_epoch, now_epoch,
                        COMPANION_PET_FEED_COOLDOWN_SECONDS))
        return COMPANION_PET_ACTION_COOLDOWN;

    pet->food--;
    pet->last_fed_epoch = now_epoch;
    if (pet->satiety > 90) {
        if (pet->overfeed_window_epoch == 0 || now_epoch < pet->overfeed_window_epoch ||
            (uint64_t)(now_epoch - pet->overfeed_window_epoch) > 7200u) {
            pet->overfeed_window_epoch = now_epoch;
            pet->overfeed_count = 1;
        } else if (pet->overfeed_count < 10) {
            /* Keep the counter inside the range accepted by companion_pet_valid(). */
            pet->overfeed_count++;
        }
        if (pet->health > 10) pet->health -= 10;
        if (pet->mood > 5) pet->mood -= 5;
        if (pet->overfeed_count >= 2) make_sick(pet, COMPANION_PET_SICK_OVERFEED);
        return COMPANION_PET_ACTION_OVERFED;
    }

    pet->xp = clamp_xp(pet->xp + 5u);
    pet->satiety = clamp_u8((int)pet->satiety + 35);
    if (pet->mood < 100) pet->mood++;
    pet->hungry_since_epoch = 0;
    update_stage(pet);
    return COMPANION_PET_ACTION_OK;
}

companion_pet_action_result_t companion_pet_medicine(companion_pet_t *pet, bool time_valid,
                                                      int64_t now_epoch)
{
    if (!pet) return COMPANION_PET_ACTION_NO_MEDICINE;
    if (!action_time_ready(pet, time_valid, now_epoch))
        return COMPANION_PET_ACTION_NO_TIME;
    if (!pet->sick) return COMPANION_PET_ACTION_NOT_SICK;
    if (pet->medicine == 0) return COMPANION_PET_ACTION_NO_MEDICINE;
    if (cooldown_active(pet->last_medicine_epoch, now_epoch,
                        COMPANION_PET_MEDICINE_COOLDOWN_SECONDS))
        return COMPANION_PET_ACTION_COOLDOWN;

    pet->medicine--;
    pet->last_medicine_epoch = now_epoch;
    pet->health = clamp_u8((int)pet->health + 45);
    pet->sick = false;
    pet->sick_reason = COMPANION_PET_SICK_NONE;
    /*
     * Medicine does not erase the underlying hunger/dirt condition, but it
     * must restart the sickness clock. Keeping an expired timestamp would
     * make the next tick re-sicken the pet one second after treatment.
     */
    pet->hungry_since_epoch = pet->satiety == 0 ? now_epoch : 0;
    pet->dirty_since_epoch = pet->hygiene < 20 ? now_epoch : 0;
    if (pet->mood < 100) pet->mood += 5;
    pet->xp = clamp_xp(pet->xp + 10u);
    update_stage(pet);
    return COMPANION_PET_ACTION_OK;
}

companion_pet_action_result_t companion_pet_play(companion_pet_t *pet, bool time_valid,
                                                 int64_t now_epoch)
{
    if (!pet) return COMPANION_PET_ACTION_TOO_SICK;
    if (!action_time_ready(pet, time_valid, now_epoch))
        return COMPANION_PET_ACTION_NO_TIME;
    if (pet->sick) return COMPANION_PET_ACTION_TOO_SICK;
    if (cooldown_active(pet->last_played_epoch, now_epoch,
                        COMPANION_PET_PLAY_COOLDOWN_SECONDS))
        return COMPANION_PET_ACTION_COOLDOWN;
    pet->last_played_epoch = now_epoch;
    pet->mood = clamp_u8((int)pet->mood + 15);
    if (pet->satiety > 2) pet->satiety -= 2;
    pet->xp = clamp_xp(pet->xp + 8u);
    update_stage(pet);
    return COMPANION_PET_ACTION_OK;
}

companion_pet_action_result_t companion_pet_clean(companion_pet_t *pet, bool time_valid,
                                                  int64_t now_epoch)
{
    if (!pet) return COMPANION_PET_ACTION_COOLDOWN;
    if (!action_time_ready(pet, time_valid, now_epoch))
        return COMPANION_PET_ACTION_NO_TIME;
    if (cooldown_active(pet->last_cleaned_epoch, now_epoch,
                        COMPANION_PET_CLEAN_COOLDOWN_SECONDS))
        return COMPANION_PET_ACTION_COOLDOWN;
    pet->last_cleaned_epoch = now_epoch;
    pet->hygiene = clamp_u8((int)pet->hygiene + 50);
    pet->dirty_since_epoch = 0;
    if (pet->mood < 100) pet->mood += 3;
    /* A successful clean always cures dirt sickness, even from hygiene 0. */
    if (pet->sick && pet->sick_reason == COMPANION_PET_SICK_DIRT) {
        pet->sick = false;
        pet->sick_reason = COMPANION_PET_SICK_NONE;
    }
    pet->xp = clamp_xp(pet->xp + 5u);
    update_stage(pet);
    return COMPANION_PET_ACTION_OK;
}

void companion_pet_on_focus_completed(companion_pet_t *pet)
{
    if (!pet) return;
    if (pet->focus_count < COMPANION_PET_XP_MAX) pet->focus_count++;
    if (pet->food < COMPANION_PET_FOOD_MAX) pet->food++;
    if (pet->focus_count % 4u == 0 && pet->medicine < COMPANION_PET_MEDICINE_MAX)
        pet->medicine++;
    pet->xp = clamp_xp(pet->xp + 20u);
    update_stage(pet);
}

companion_vitality_t companion_pet_vitality(const companion_pet_t *pet)
{
    if (!pet) return COMPANION_VITALITY_HUNGRY;
    if (pet->sick) return COMPANION_VITALITY_SICK;
    if (pet->satiety < 30) return COMPANION_VITALITY_HUNGRY;
    return COMPANION_VITALITY_FULL;
}

const char *companion_pet_stage_name(companion_pet_stage_t stage)
{
    switch (stage) {
    case COMPANION_PET_STAGE_BABY: return "baby";
    case COMPANION_PET_STAGE_CHILD: return "child";
    case COMPANION_PET_STAGE_TEEN: return "teen";
    case COMPANION_PET_STAGE_ADULT: return "adult";
    default: return "baby";
    }
}

const char *companion_pet_stage_short_name(companion_pet_stage_t stage)
{
    switch (stage) {
    case COMPANION_PET_STAGE_BABY: return "BABY";
    case COMPANION_PET_STAGE_CHILD: return "CHILD";
    case COMPANION_PET_STAGE_TEEN: return "TEEN";
    case COMPANION_PET_STAGE_ADULT: return "ADULT";
    default: return "BABY";
    }
}

const char *companion_pet_sick_reason_name(companion_pet_sick_reason_t reason)
{
    switch (reason) {
    case COMPANION_PET_SICK_STARVATION: return "starvation";
    case COMPANION_PET_SICK_OVERFEED: return "overfeed";
    case COMPANION_PET_SICK_DIRT: return "dirt";
    default: return "none";
    }
}

const char *companion_pet_action_result_name(companion_pet_action_result_t result)
{
    switch (result) {
    case COMPANION_PET_ACTION_OK: return "ok";
    case COMPANION_PET_ACTION_COOLDOWN: return "cooldown";
    case COMPANION_PET_ACTION_NO_FOOD: return "no_food";
    case COMPANION_PET_ACTION_NO_MEDICINE: return "no_medicine";
    case COMPANION_PET_ACTION_NOT_SICK: return "not_sick";
    case COMPANION_PET_ACTION_TOO_SICK: return "too_sick";
    case COMPANION_PET_ACTION_OVERFED: return "overfed";
    case COMPANION_PET_ACTION_NO_TIME: return "no_time";
    default: return "invalid";
    }
}
