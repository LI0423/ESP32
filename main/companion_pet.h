#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "companion_progress.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMPANION_PET_FOOD_MAX 99u
#define COMPANION_PET_MEDICINE_MAX 99u
#define COMPANION_PET_XP_MAX 1000000u
#define COMPANION_PET_EPOCH_MAX 0xffffffffLL

#define COMPANION_PET_FEED_COOLDOWN_SECONDS (10u * 60u)
#define COMPANION_PET_MEDICINE_COOLDOWN_SECONDS (6u * 60u * 60u)
#define COMPANION_PET_PLAY_COOLDOWN_SECONDS (30u * 60u)
#define COMPANION_PET_CLEAN_COOLDOWN_SECONDS (60u * 60u)
#define COMPANION_PET_OFFLINE_MAX_SECONDS (48u * 60u * 60u)

typedef enum {
    COMPANION_PET_STAGE_BABY = 0,
    COMPANION_PET_STAGE_CHILD,
    COMPANION_PET_STAGE_TEEN,
    COMPANION_PET_STAGE_ADULT,
} companion_pet_stage_t;

typedef enum {
    COMPANION_PET_SICK_NONE = 0,
    COMPANION_PET_SICK_STARVATION,
    COMPANION_PET_SICK_OVERFEED,
    COMPANION_PET_SICK_DIRT,
} companion_pet_sick_reason_t;

typedef enum {
    COMPANION_PET_ACTION_NONE = 0,
    COMPANION_PET_ACTION_FEED,
    COMPANION_PET_ACTION_MEDICINE,
    COMPANION_PET_ACTION_PLAY,
    COMPANION_PET_ACTION_CLEAN,
} companion_pet_action_t;

typedef enum {
    COMPANION_PET_ACTION_OK = 0,
    COMPANION_PET_ACTION_COOLDOWN,
    COMPANION_PET_ACTION_NO_FOOD,
    COMPANION_PET_ACTION_NO_MEDICINE,
    COMPANION_PET_ACTION_NOT_SICK,
    COMPANION_PET_ACTION_TOO_SICK,
    COMPANION_PET_ACTION_OVERFED,
    COMPANION_PET_ACTION_NO_TIME,
} companion_pet_action_result_t;

typedef struct {
    uint8_t satiety;
    uint8_t health;
    uint8_t mood;
    uint8_t hygiene;
    uint8_t food;
    uint8_t medicine;
    uint32_t xp;
    uint32_t focus_count;
    int32_t born_day;
    uint32_t age_days;
    uint8_t stage;
    bool sick;
    uint8_t sick_reason;
    int64_t last_fed_epoch;
    int64_t last_medicine_epoch;
    int64_t last_played_epoch;
    int64_t last_cleaned_epoch;
    int64_t last_update_epoch;
    int64_t hungry_since_epoch;
    int64_t dirty_since_epoch;
    int64_t overfeed_window_epoch;
    uint8_t overfeed_count;
    uint32_t satiety_carry_seconds;
    uint32_t health_carry_seconds;
    uint32_t hygiene_carry_seconds;
    uint32_t mood_carry_seconds;
} companion_pet_t;

void companion_pet_reset(companion_pet_t *pet);
bool companion_pet_valid(const companion_pet_t *pet);
void companion_pet_sanitize(companion_pet_t *pet);
/* Rebase persisted timestamps that are ahead of the clock after an RTC reset. */
bool companion_pet_rebase_future(companion_pet_t *pet, int64_t now_epoch);

void companion_pet_roll_day(companion_pet_t *pet, int32_t today, int gap_days);
uint32_t companion_pet_age_days(const companion_pet_t *pet, int32_t today);
bool companion_pet_tick(companion_pet_t *pet, uint32_t elapsed_seconds, bool sleeping,
                        bool time_valid, int64_t now_epoch);
bool companion_pet_catch_up(companion_pet_t *pet, bool sleeping, bool time_valid,
                            int64_t now_epoch);

companion_pet_action_result_t companion_pet_feed(companion_pet_t *pet, bool time_valid,
                                                  int64_t now_epoch);
companion_pet_action_result_t companion_pet_medicine(companion_pet_t *pet, bool time_valid,
                                                      int64_t now_epoch);
companion_pet_action_result_t companion_pet_play(companion_pet_t *pet, bool time_valid,
                                                 int64_t now_epoch);
companion_pet_action_result_t companion_pet_clean(companion_pet_t *pet, bool time_valid,
                                                  int64_t now_epoch);
void companion_pet_on_focus_completed(companion_pet_t *pet);

companion_vitality_t companion_pet_vitality(const companion_pet_t *pet);
const char *companion_pet_stage_name(companion_pet_stage_t stage);
const char *companion_pet_stage_short_name(companion_pet_stage_t stage);
const char *companion_pet_sick_reason_name(companion_pet_sick_reason_t reason);
const char *companion_pet_action_result_name(companion_pet_action_result_t result);

#ifdef __cplusplus
}
#endif
