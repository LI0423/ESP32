#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "companion_pet.h"

static struct tm make_local(int year, int month, int day, int hour, int minute)
{
    struct tm local = {0};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_isdst = -1;
    return local;
}

static int64_t epoch_of(struct tm local)
{
    time_t value = mktime(&local);
    assert(value > 0);
    return (int64_t)value;
}

static void test_defaults_and_validation(void)
{
    companion_pet_t pet;
    companion_pet_reset(&pet);
    assert(pet.satiety == 80 && pet.health == 100 && pet.mood == 80);
    assert(pet.hygiene == 100 && pet.food == 5 && pet.medicine == 2);
    assert(pet.stage == COMPANION_PET_STAGE_BABY);
    assert(companion_pet_valid(&pet));

    pet.satiety = 101;
    assert(!companion_pet_valid(&pet));
    companion_pet_sanitize(&pet);
    assert(pet.satiety == 100 && companion_pet_valid(&pet));

    pet.health = 0;
    assert(!companion_pet_valid(&pet));
    companion_pet_sanitize(&pet);
    assert(pet.health == 10 && companion_pet_valid(&pet));

    pet.stage = COMPANION_PET_STAGE_ADULT;
    assert(!companion_pet_valid(&pet));
    companion_pet_sanitize(&pet);
    assert(pet.stage == COMPANION_PET_STAGE_BABY);
}

static void test_time_decay_and_sleep(void)
{
    int64_t base = epoch_of(make_local(2026, 11, 25, 10, 0));
    companion_pet_t pet;
    companion_pet_reset(&pet);
    pet.last_update_epoch = base;

    assert(companion_pet_tick(&pet, 3600, false, true, base + 3600));
    assert(pet.satiety == 76); /* -4 per awake hour */
    assert(pet.health == 100 && pet.hygiene == 100 && pet.mood == 80);
    assert(pet.last_update_epoch == base + 3600);
    assert(companion_pet_valid(&pet));

    companion_pet_reset(&pet);
    pet.last_update_epoch = base;
    assert(companion_pet_tick(&pet, 3600, true, true, base + 3600));
    assert(pet.satiety == 78); /* -2 per sleeping hour */
    assert(companion_pet_valid(&pet));

    companion_pet_reset(&pet);
    pet.satiety = 29;
    pet.last_update_epoch = base;
    companion_pet_tick(&pet, 7200, false, true, base + 7200);
    assert(pet.satiety == 21 && pet.mood == 79 && pet.hygiene == 99);

    companion_pet_reset(&pet);
    pet.last_update_epoch = base;
    assert(!companion_pet_tick(&pet, 3600, false, false, base + 3600));
    assert(pet.satiety == 80 && pet.last_update_epoch == base);
}

static void test_starvation_medicine_and_health_floor(void)
{
    int64_t base = epoch_of(make_local(2026, 11, 25, 10, 0));
    companion_pet_t pet;
    companion_pet_reset(&pet);
    pet.satiety = 1;
    pet.last_update_epoch = base;

    companion_pet_tick(&pet, 900, false, true, base + 900);
    assert(pet.satiety == 0 && pet.hungry_since_epoch == base + 900);
    assert(!pet.sick);

    companion_pet_catch_up(&pet, false, true, base + 6 * 3600 + 900);
    assert(pet.sick && pet.sick_reason == COMPANION_PET_SICK_STARVATION);
    assert(pet.health == 10); /* sickness and the health floor are non-lethal */

    companion_pet_catch_up(&pet, false, true, base + 30 * 3600);
    assert(pet.health == 10 && pet.sick);
    assert(companion_pet_vitality(&pet) == COMPANION_VITALITY_SICK);

    int64_t treat = base + 30 * 3600;
    assert(companion_pet_medicine(&pet, true, treat) == COMPANION_PET_ACTION_OK);
    assert(!pet.sick && pet.health == 55 && pet.medicine == 1 && pet.xp == 10);
    assert(pet.hungry_since_epoch == treat); /* fresh window, not the old one */

    /* A treated pet must stay healthy on the very next 1 Hz tick. */
    companion_pet_tick(&pet, 1, false, true, treat + 1);
    assert(!pet.sick && pet.health == 55);

    pet.sick = true;
    pet.sick_reason = COMPANION_PET_SICK_OVERFEED;
    assert(companion_pet_medicine(&pet, true, treat + 60) == COMPANION_PET_ACTION_COOLDOWN);
    assert(pet.medicine == 1);
}

static void test_overfeeding_is_deterministic(void)
{
    int64_t base = epoch_of(make_local(2026, 11, 25, 10, 0));
    companion_pet_t pet;
    companion_pet_reset(&pet);
    pet.satiety = 95;

    assert(companion_pet_feed(&pet, true, base) == COMPANION_PET_ACTION_OVERFED);
    assert(pet.food == 4 && pet.health == 90 && pet.mood == 75 && !pet.sick);
    assert(companion_pet_feed(&pet, true, base + 60) == COMPANION_PET_ACTION_COOLDOWN);
    assert(pet.food == 4);

    assert(companion_pet_feed(&pet, true, base + 600) == COMPANION_PET_ACTION_OVERFED);
    assert(pet.food == 3 && pet.sick && pet.sick_reason == COMPANION_PET_SICK_OVERFEED);
    assert(pet.health == 10);

    /* Repeated overfeeding inside one 2h window must not corrupt the record. */
    companion_pet_t repeat;
    companion_pet_reset(&repeat);
    repeat.satiety = 95;
    repeat.food = 20;
    for (int i = 0; i < 12; i++) {
        assert(companion_pet_feed(&repeat, true, base + i * 600) ==
               COMPANION_PET_ACTION_OVERFED);
        assert(repeat.overfeed_count <= 10);
        assert(companion_pet_valid(&repeat));
    }
    assert(repeat.overfeed_count == 10);
}

static void test_dirt_sickness_and_cleaning(void)
{
    int64_t base = epoch_of(make_local(2026, 11, 25, 10, 0));
    companion_pet_t pet;
    companion_pet_reset(&pet);
    pet.hygiene = 20;
    pet.last_update_epoch = base;

    companion_pet_tick(&pet, 7200, false, true, base + 7200);
    assert(pet.hygiene == 19 && pet.dirty_since_epoch == base + 7200 && !pet.sick);
    companion_pet_catch_up(&pet, false, true, base + 12 * 3600 + 7200);
    assert(pet.sick && pet.sick_reason == COMPANION_PET_SICK_DIRT);

    int64_t clean_at = base + 12 * 3600 + 7200;
    assert(companion_pet_clean(&pet, true, clean_at) == COMPANION_PET_ACTION_OK);
    assert(pet.hygiene == 63 && !pet.sick && pet.dirty_since_epoch == 0);
    assert(pet.xp == 5);
    assert(companion_pet_clean(&pet, true, clean_at + 60) == COMPANION_PET_ACTION_COOLDOWN);

    /* Even hygiene 0 is cured by one successful clean. */
    companion_pet_t low;
    companion_pet_reset(&low);
    low.hygiene = 5;
    low.sick = true;
    low.sick_reason = COMPANION_PET_SICK_DIRT;
    assert(companion_pet_clean(&low, true, base) == COMPANION_PET_ACTION_OK);
    assert(low.hygiene == 55 && !low.sick &&
           low.sick_reason == COMPANION_PET_SICK_NONE);
}

static void test_care_rewards_and_cooldowns(void)
{
    int64_t base = epoch_of(make_local(2026, 11, 25, 10, 0));
    companion_pet_t pet;
    companion_pet_reset(&pet);

    assert(companion_pet_feed(&pet, true, base) == COMPANION_PET_ACTION_OK);
    assert(pet.satiety == 100 && pet.food == 4 && pet.xp == 5 && pet.mood == 81);
    assert(companion_pet_play(&pet, true, base + 1) == COMPANION_PET_ACTION_OK);
    assert(pet.mood == 96 && pet.satiety == 98 && pet.xp == 13);
    assert(companion_pet_clean(&pet, true, base + 2) == COMPANION_PET_ACTION_OK);
    assert(pet.xp == 18 && pet.mood == 99);
    assert(companion_pet_play(&pet, true, base + 3) == COMPANION_PET_ACTION_COOLDOWN);
    assert(companion_pet_feed(&pet, true, base + 4) == COMPANION_PET_ACTION_COOLDOWN);

    pet.food = 0;
    pet.last_fed_epoch = 0;
    assert(companion_pet_feed(&pet, true, base + 5) == COMPANION_PET_ACTION_NO_FOOD);
    assert(companion_pet_medicine(&pet, true, base + 5) == COMPANION_PET_ACTION_NOT_SICK);
    pet.sick = true;
    pet.sick_reason = COMPANION_PET_SICK_OVERFEED;
    pet.medicine = 0;
    assert(companion_pet_medicine(&pet, true, base + 5) == COMPANION_PET_ACTION_NO_MEDICINE);
}

static void test_focus_economy(void)
{
    companion_pet_t pet;
    companion_pet_reset(&pet);
    for (unsigned i = 0; i < 4; i++) companion_pet_on_focus_completed(&pet);
    assert(pet.focus_count == 4 && pet.food == 9 && pet.medicine == 3);
    assert(pet.xp == 80 && companion_pet_valid(&pet));
}

static void test_growth_requires_age_and_xp(void)
{
    companion_pet_t pet;
    companion_pet_reset(&pet);
    companion_pet_roll_day(&pet, 20261125, 0);
    assert(pet.born_day == 20261125 && pet.age_days == 0);

    int64_t born_epoch = epoch_of(make_local(2026, 11, 25, 10, 0));
    pet.last_update_epoch = born_epoch;
    companion_pet_catch_up(&pet, false, true, born_epoch + 3 * 86400);
    assert(pet.age_days == 3);
    companion_pet_roll_day(&pet, 20261128, 3);
    assert(pet.age_days == 3); /* catch-up and timer rollover cannot double-count */
    companion_pet_roll_day(&pet, 20261124, -1);
    assert(pet.age_days == 3 && pet.born_day == 20261125);

    pet.xp = 100;
    companion_pet_roll_day(&pet, 20261128, 0);
    assert(pet.stage == COMPANION_PET_STAGE_CHILD);
    companion_pet_roll_day(&pet, 20261126, 1);
    assert(pet.stage == COMPANION_PET_STAGE_CHILD);

    pet.xp = 300;
    assert(pet.stage == COMPANION_PET_STAGE_CHILD);
    companion_pet_roll_day(&pet, 20261128, 3);
    assert(pet.stage == COMPANION_PET_STAGE_TEEN);

    pet.xp = 600;
    assert(pet.stage == COMPANION_PET_STAGE_TEEN);
    companion_pet_roll_day(&pet, 20261202, 7);
    assert(pet.stage == COMPANION_PET_STAGE_ADULT);
    assert(companion_pet_age_days(&pet, 20261203) == 8);
    assert(companion_pet_valid(&pet));

    companion_pet_reset(&pet);
    pet.born_day = 20261125;
    pet.age_days = 30;
    companion_pet_sanitize(&pet);
    assert(pet.stage == COMPANION_PET_STAGE_BABY);
}

static void test_offline_cap_and_night_sleep(void)
{
    int64_t base = epoch_of(make_local(2026, 11, 25, 10, 0));
    companion_pet_t short_gap;
    companion_pet_t long_gap;
    companion_pet_reset(&short_gap);
    companion_pet_reset(&long_gap);
    short_gap.last_update_epoch = base;
    long_gap.last_update_epoch = base;

    companion_pet_catch_up(&short_gap, false, true, base + 48 * 3600);
    companion_pet_catch_up(&long_gap, false, true, base + 72 * 3600);
    assert(short_gap.satiety == long_gap.satiety);
    assert(short_gap.hygiene == long_gap.hygiene);
    assert(short_gap.mood == long_gap.mood);
    assert(long_gap.age_days == 3 && short_gap.age_days == 2);
    assert(short_gap.sick && short_gap.health == 10); /* neglected, never dead */

    companion_pet_t night;
    companion_pet_reset(&night);
    night.last_update_epoch = epoch_of(make_local(2026, 11, 25, 21, 0));
    companion_pet_catch_up(&night, false, true, night.last_update_epoch + 2 * 3600);
    assert(night.satiety == 74); /* local 21:00–23:00: one awake + one sleep hour */
}

static void test_actions_require_valid_time(void)
{
    companion_pet_t pet;
    companion_pet_reset(&pet);
    assert(companion_pet_feed(&pet, false, 0) == COMPANION_PET_ACTION_NO_TIME);
    assert(companion_pet_medicine(&pet, false, 0) == COMPANION_PET_ACTION_NO_TIME);
    assert(companion_pet_play(&pet, false, 0) == COMPANION_PET_ACTION_NO_TIME);
    assert(companion_pet_clean(&pet, false, 0) == COMPANION_PET_ACTION_NO_TIME);
    assert(pet.food == 5 && pet.medicine == 2 && pet.xp == 0);

    int64_t base = epoch_of(make_local(2026, 11, 25, 10, 0));
    pet.last_update_epoch = base + 3600;
    assert(companion_pet_feed(&pet, true, base) == COMPANION_PET_ACTION_NO_TIME);
    assert(!companion_pet_catch_up(&pet, false, true, base));
    assert(pet.last_update_epoch == base + 3600); /* rewind does not move baseline */
    assert(companion_pet_feed(&pet, true, base + 3601) == COMPANION_PET_ACTION_OK);
}

static void test_sickness_timestamps_and_mood_carry(void)
{
    int64_t base = epoch_of(make_local(2026, 11, 25, 10, 0));
    companion_pet_t pet;
    companion_pet_reset(&pet);
    pet.satiety = 1;
    pet.last_update_epoch = base;

    /* Setting the starvation timestamp alone is a state change worth saving. */
    assert(companion_pet_tick(&pet, 900, false, true, base + 900));
    assert(pet.satiety == 0 && pet.hungry_since_epoch == base + 900);

    /* Leftover mood decay time must not follow into the next bad spell. */
    companion_pet_reset(&pet);
    pet.last_update_epoch = base;
    pet.mood_carry_seconds = 7000;
    companion_pet_tick(&pet, 3600, false, true, base + 3600);
    assert(pet.mood == 80 && pet.mood_carry_seconds == 0);
    pet.satiety = 29;
    companion_pet_tick(&pet, 200, false, true, base + 3800);
    assert(pet.mood == 80);
}

static void test_rebase_future_timestamps(void)
{
    int64_t base = epoch_of(make_local(2026, 11, 25, 10, 0));
    companion_pet_t pet;
    companion_pet_reset(&pet);
    pet.last_update_epoch = base + 3600;
    pet.last_fed_epoch = base + 7200;
    pet.hungry_since_epoch = base + 100;

    assert(companion_pet_rebase_future(&pet, base));
    assert(pet.last_update_epoch == base);
    assert(pet.last_fed_epoch == base);
    assert(pet.hungry_since_epoch == base);
    assert(!companion_pet_rebase_future(&pet, base));
    assert(!companion_pet_rebase_future(&pet, 0));
}

int main(void)
{
    test_defaults_and_validation();
    test_time_decay_and_sleep();
    test_starvation_medicine_and_health_floor();
    test_overfeeding_is_deterministic();
    test_dirt_sickness_and_cleaning();
    test_care_rewards_and_cooldowns();
    test_focus_economy();
    test_growth_requires_age_and_xp();
    test_offline_cap_and_night_sleep();
    test_actions_require_valid_time();
    test_sickness_timestamps_and_mood_carry();
    test_rebase_future_timestamps();
    puts("companion pet tests passed");
    return 0;
}
