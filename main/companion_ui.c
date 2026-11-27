#include "companion_ui.h"

#include <stdio.h>
#include <string.h>

#include "companion_progress.h"
#include "companion_pet.h"
#include "companion_timer.h"
#include "deskpal_font.h"
#include "deskpal_sprites.h"
#include "esp_timer.h"
#include "lcd.h"

static uint16_t hud_text_color(void)
{
    /* Approved HUD color #FFECD6. Stage, time, today, and streak share it. */
    return lcd_rgb(255, 236, 214);
}

static uint16_t phase_background(companion_phase_t phase)
{
    switch (phase) {
    case COMPANION_PHASE_FOCUS: return DESKPAL_BG_FOCUS;
    case COMPANION_PHASE_BREAK: return DESKPAL_BG_BREAK;
    case COMPANION_PHASE_CELEBRATE: return DESKPAL_BG_COMPLETE;
    case COMPANION_PHASE_STANDBY: return DESKPAL_BG_STANDBY;
    default: return DESKPAL_BG_FOCUS;
    }
}

static deskpal_sprite_id_t phase_sprite(companion_phase_t phase)
{
    switch (phase) {
    case COMPANION_PHASE_FOCUS: return DESKPAL_SPRITE_FOCUS;
    case COMPANION_PHASE_BREAK: return DESKPAL_SPRITE_BREAK;
    case COMPANION_PHASE_CELEBRATE: return DESKPAL_SPRITE_COMPLETE;
    case COMPANION_PHASE_STANDBY: return DESKPAL_SPRITE_STANDBY;
    default: return DESKPAL_SPRITE_FOCUS;
    }
}

static deskpal_sprite_id_t stage_sprite(uint8_t stage)
{
    switch ((companion_pet_stage_t)stage) {
    case COMPANION_PET_STAGE_CHILD: return DESKPAL_SPRITE_CHILD;
    case COMPANION_PET_STAGE_TEEN: return DESKPAL_SPRITE_TEEN;
    case COMPANION_PET_STAGE_ADULT: return DESKPAL_SPRITE_ADULT;
    default: return DESKPAL_SPRITE_BABY;
    }
}

static deskpal_sprite_id_t pet_action_sprite(companion_pet_action_t action)
{
    switch (action) {
    case COMPANION_PET_ACTION_FEED: return DESKPAL_SPRITE_ACTION_FEED;
    case COMPANION_PET_ACTION_MEDICINE: return DESKPAL_SPRITE_ACTION_MEDICINE;
    case COMPANION_PET_ACTION_PLAY: return DESKPAL_SPRITE_ACTION_PLAY;
    case COMPANION_PET_ACTION_CLEAN: return DESKPAL_SPRITE_ACTION_CLEAN;
    default: return DESKPAL_SPRITE_BABY;
    }
}

static void draw_pixel_char(int x, int y, char ch, int scale, uint16_t color)
{
    const uint8_t *glyph = deskpal_font_glyph(ch);
    for (int row = 0; row < DESKPAL_FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < DESKPAL_FONT_W; col++) {
            if ((bits & (0x80u >> col)) == 0) continue;
            int px = x + col * scale;
            int py = y + row * scale;
            if (scale == 1) {
                lcd_set_pixel(px, py, color);
            } else {
                lcd_fill_rect(px, py, scale, scale, color);
            }
        }
    }
}

static void draw_pixel_text(int x, int y, const char *value, int scale, uint16_t color)
{
    if (!value) return;
    int advance = DESKPAL_FONT_W * scale;
    for (int i = 0; value[i]; i++) {
        draw_pixel_char(x + i * advance, y, value[i], scale, color);
    }
}

static void draw_centered_pixel_text(int cx, int cy, const char *value, int scale, uint16_t color)
{
    if (!value) return;
    int w = (int)strlen(value) * DESKPAL_FONT_W * scale;
    int h = DESKPAL_FONT_H * scale;
    draw_pixel_text(cx - w / 2, cy - h / 2, value, scale, color);
}

static void draw_character(const companion_state_t *state)
{
    if (!state) return;
    /*
     * Care actions have their own feedback sprite. Hungry/sick keep the shared
     * vitality faces; break/standby reveal the current growth-stage body.
     * Focus and celebration retain the original timer expressions.
     */
    deskpal_sprite_id_t id = phase_sprite(state->phase);
    if (state->pet_action_seconds > 0 &&
        state->pet_action_result != COMPANION_PET_ACTION_OVERFED) {
        id = pet_action_sprite(state->pet_action);
    } else if (state->vitality == COMPANION_VITALITY_HUNGRY ||
               state->pet_action_result == COMPANION_PET_ACTION_OVERFED) {
        id = DESKPAL_SPRITE_HUNGRY;
    } else if (state->vitality == COMPANION_VITALITY_SICK) {
        id = DESKPAL_SPRITE_SICK;
    } else if (state->phase == COMPANION_PHASE_BREAK ||
               state->phase == COMPANION_PHASE_STANDBY) {
        id = stage_sprite(state->pet.stage);
    }

    const uint8_t *sprite = deskpal_sprite_data(id);
    const uint16_t *palette = deskpal_sprite_palette(id);
    if (!sprite || !palette) return;
    lcd_blit_indexed(DESKPAL_CHAR_X, DESKPAL_CHAR_Y,
                     DESKPAL_CHAR_W, DESKPAL_CHAR_H,
                     sprite, palette, DESKPAL_PALETTE_LEN,
                     DESKPAL_TRANSPARENT_INDEX);
}

/*
 * The pill badge means "medicine can cure this sickness". Dirt sickness is
 * cleaned instead, and starvation needs feeding first; showing a pill there
 * would suggest the wrong action. No badge without stock or without a clock.
 */
static bool medicine_can_cure(const companion_state_t *state)
{
    if (!state->time_valid || !state->pet.sick || state->pet.medicine == 0) return false;
    switch ((companion_pet_sick_reason_t)state->pet.sick_reason) {
    case COMPANION_PET_SICK_OVERFEED:
        return true;
    case COMPANION_PET_SICK_STARVATION:
        return state->pet.satiety > 0;
    case COMPANION_PET_SICK_DIRT:
    default:
        return false;
    }
}

/* One badge at a time. Medicine-curable sickness outranks plain hunger. */
static bool hunger_badge_needed(const companion_state_t *state)
{
    if (!state->time_valid) return false;
    if (state->pet.satiety < 30) return true;
    return state->pet.sick &&
           state->pet.sick_reason == COMPANION_PET_SICK_STARVATION;
}

static void draw_status_badge(const companion_state_t *state)
{
    uint32_t phase = (uint32_t)(esp_timer_get_time() / 1000 / DESKPAL_PILL_BLINK_MS);
    if (phase & 1u) return; /* 1 Hz blink: 500 ms on, 500 ms off */

    if (medicine_can_cure(state)) {
        lcd_blit_indexed(DESKPAL_PILL_X, DESKPAL_PILL_Y,
                         DESKPAL_PILL_SIZE, DESKPAL_PILL_SIZE,
                         deskpal_sick_pill, deskpal_palette,
                         DESKPAL_PALETTE_LEN, DESKPAL_TRANSPARENT_INDEX);
    } else if (hunger_badge_needed(state)) {
        lcd_blit_indexed(DESKPAL_HUNGER_X, DESKPAL_HUNGER_Y,
                         DESKPAL_HUNGER_SIZE, DESKPAL_HUNGER_SIZE,
                         deskpal_hungry_rice, deskpal_palette,
                         DESKPAL_PALETTE_LEN, DESKPAL_TRANSPARENT_INDEX);
    }
}

static void draw_online_dot(bool online)
{
    uint16_t color = online ? DESKPAL_DOT_ONLINE : lcd_rgb(72, 88, 112);
    lcd_fill_ellipse(DESKPAL_DOT_CX, DESKPAL_DOT_CY, DESKPAL_DOT_R, DESKPAL_DOT_R, color);
}

static uint16_t stat_color(uint8_t value)
{
    if (value < 30) return lcd_rgb(255, 112, 112);
    if (value < 60) return lcd_rgb(255, 201, 107);
    return lcd_rgb(94, 242, 176);
}

static const char *pet_action_label(const companion_state_t *state)
{
    if (state->pet_action_result == COMPANION_PET_ACTION_OVERFED) return "TOO MUCH";
    switch (state->pet_action) {
    case COMPANION_PET_ACTION_FEED: return "YUM!";
    case COMPANION_PET_ACTION_MEDICINE: return "MEDICINE";
    case COMPANION_PET_ACTION_PLAY: return "PLAY!";
    case COMPANION_PET_ACTION_CLEAN: return "CLEAN!";
    default: return "";
    }
}

void companion_ui_init(void)
{
}

void companion_ui_render(const companion_state_t *state)
{
    if (!state) return;

    lcd_clear(phase_background(state->phase));
    draw_character(state);
    draw_status_badge(state);

    uint16_t text = hud_text_color();
    char streak[8];
    companion_progress_format_streak(streak, sizeof(streak), state->streak);
    /* Left-aligned on the stage-label row. 8px glyph → top y = cy - 4. */
    draw_pixel_text(DESKPAL_STREAK_X, DESKPAL_STREAK_CY - DESKPAL_FONT_H / 2,
                    streak, 1, text);
    draw_centered_pixel_text(DESKPAL_STAGE_CX, DESKPAL_STAGE_CY,
                             companion_pet_stage_short_name((companion_pet_stage_t)state->pet.stage),
                             1, text);

    char clock[16];
    if (state->phase == COMPANION_PHASE_STANDBY) {
        snprintf(clock, sizeof(clock), "--:--");
    } else {
        snprintf(clock, sizeof(clock), "%02lu:%02lu",
                 (unsigned long)(state->remaining_seconds / 60u),
                 (unsigned long)(state->remaining_seconds % 60u));
    }
    draw_centered_pixel_text(DESKPAL_TIME_CX, DESKPAL_TIME_CY, clock, 2, text);

    /* Care stats occupy the narrow row between the character and timer. */
    if (state->pet_action_seconds > 0) {
        draw_centered_pixel_text(DESKPAL_TIME_CX, 179, pet_action_label(state), 1, text);
    } else {
        char stats[24];
        snprintf(stats, sizeof(stats), "S%u H%u M%u C%u", state->pet.satiety,
                 state->pet.health, state->pet.mood, state->pet.hygiene);
        int width = (int)strlen(stats) * DESKPAL_FONT_W;
        int x = DESKPAL_TIME_CX - width / 2;
        const uint8_t values[] = {state->pet.satiety, state->pet.health,
                                  state->pet.mood, state->pet.hygiene};
        const char labels[] = {'S', 'H', 'M', 'C'};
        for (size_t i = 0; i < 4; i++) {
            char value[8];
            snprintf(value, sizeof(value), "%u", values[i]);
            draw_pixel_char(x, 177, labels[i], 1, stat_color(values[i]));
            x += DESKPAL_FONT_W;
            draw_pixel_text(x, 177, value, 1, stat_color(values[i]));
            x += (int)strlen(value) * DESKPAL_FONT_W;
            if (i + 1 < 4) x += DESKPAL_FONT_W;
        }
    }

    char today[24];
    companion_progress_format_today(today, sizeof(today),
                                    state->completed_sessions, state->daily_goal);
    draw_centered_pixel_text(DESKPAL_TODAY_CX, DESKPAL_TODAY_CY, today, 1, text);

    /* Offline only affects the network dot — never the character sprite. */
    draw_online_dot(state->online);
    lcd_flush();
}
