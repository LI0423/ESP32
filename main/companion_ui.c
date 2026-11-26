#include "companion_ui.h"

#include <stdio.h>
#include <string.h>

#include "companion_progress.h"
#include "companion_timer.h"
#include "deskpal_font.h"
#include "deskpal_sprites.h"
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

static void draw_character(companion_phase_t phase)
{
    const uint8_t *sprite = deskpal_sprite_data(phase_sprite(phase));
    if (!sprite) return;
    lcd_blit_indexed(DESKPAL_CHAR_X, DESKPAL_CHAR_Y,
                     DESKPAL_CHAR_W, DESKPAL_CHAR_H,
                     sprite, deskpal_palette, DESKPAL_PALETTE_LEN,
                     DESKPAL_TRANSPARENT_INDEX);
}

static void draw_online_dot(bool online)
{
    uint16_t color = online ? DESKPAL_DOT_ONLINE : lcd_rgb(72, 88, 112);
    lcd_fill_ellipse(DESKPAL_DOT_CX, DESKPAL_DOT_CY, DESKPAL_DOT_R, DESKPAL_DOT_R, color);
}

void companion_ui_init(void)
{
}

void companion_ui_render(const companion_state_t *state)
{
    if (!state) return;

    lcd_clear(phase_background(state->phase));
    draw_character(state->phase);

    uint16_t text = hud_text_color();
    char streak[8];
    companion_progress_format_streak(streak, sizeof(streak), state->streak);
    /* Left-aligned on the stage-label row. 8px glyph → top y = cy - 4. */
    draw_pixel_text(DESKPAL_STREAK_X, DESKPAL_STREAK_CY - DESKPAL_FONT_H / 2,
                    streak, 1, text);
    draw_centered_pixel_text(DESKPAL_STAGE_CX, DESKPAL_STAGE_CY,
                             companion_phase_label(state->phase), 1, text);

    char clock[16];
    if (state->phase == COMPANION_PHASE_STANDBY) {
        snprintf(clock, sizeof(clock), "--:--");
    } else {
        snprintf(clock, sizeof(clock), "%02lu:%02lu",
                 (unsigned long)(state->remaining_seconds / 60u),
                 (unsigned long)(state->remaining_seconds % 60u));
    }
    draw_centered_pixel_text(DESKPAL_TIME_CX, DESKPAL_TIME_CY, clock, 2, text);

    char today[24];
    companion_progress_format_today(today, sizeof(today),
                                    state->completed_sessions, state->daily_goal);
    draw_centered_pixel_text(DESKPAL_TODAY_CX, DESKPAL_TODAY_CY, today, 1, text);

    /* Offline only affects the network dot — never the character sprite. */
    draw_online_dot(state->online);
    lcd_flush();
}
