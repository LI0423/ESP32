#include "companion_ui.h"

#include <stdio.h>
#include <string.h>
#include "lcd.h"

#define UI_FRAME_MS 100u
#define TEXT_W 12
#define SMALL_W 8

static uint32_t s_tick;

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return lcd_rgb(r, g, b);
}

static void text(int x, int y, const char *value, uint16_t color)
{
    lcd_draw_text(x, y, value, color);
}

static void small(int x, int y, const char *value, uint16_t color)
{
    lcd_draw_text_sm(x, y, value, color);
}

static void centered_text(int cx, int y, const char *value, uint16_t color)
{
    text(cx - (int)strlen(value) * TEXT_W / 2, y, value, color);
}

static void centered_small(int cx, int y, const char *value, uint16_t color)
{
    small(cx - (int)strlen(value) * SMALL_W / 2, y, value, color);
}

static uint16_t phase_color(companion_phase_t phase, bool online)
{
    if (!online) return rgb(72, 88, 112);
    switch (phase) {
    case COMPANION_PHASE_FOCUS: return rgb(77, 232, 255);
    case COMPANION_PHASE_BREAK: return rgb(94, 245, 184);
    case COMPANION_PHASE_CELEBRATE: return rgb(255, 201, 107);
    default: return rgb(77, 232, 255);
    }
}

static const char *phase_label(companion_phase_t phase)
{
    switch (phase) {
    case COMPANION_PHASE_FOCUS: return "FOCUS";
    case COMPANION_PHASE_BREAK: return "BREAK";
    case COMPANION_PHASE_CELEBRATE: return "COMPLETE";
    default: return "FOCUS";
    }
}

static const char *phase_phrase(companion_phase_t phase, bool online)
{
    if (!online) return "Waiting for Wi-Fi";
    switch (phase) {
    case COMPANION_PHASE_FOCUS: return "Stay with the moment";
    case COMPANION_PHASE_BREAK: return "Rest your eyes";
    case COMPANION_PHASE_CELEBRATE: return "One session complete";
    default: return "Here with you";
    }
}

static void draw_header(const companion_state_t *state)
{
    text(10, 5, state->clock, rgb(232, 244, 255));
    small(96, 10, state->date, rgb(159, 177, 201));
    uint16_t dot = state->online ? rgb(94, 245, 184) : rgb(72, 88, 112);
    lcd_fill_ellipse(222, 14, 4, 4, rgb(24, 37, 61));
    lcd_fill_ellipse(222, 14, 2, 2, dot);
    lcd_fill_rect(10, 34, 220, 1, rgb(42, 63, 114));
}

static void draw_face(companion_phase_t phase, bool online, int cx, int cy, uint16_t accent)
{
    uint16_t body = online ? rgb(18, 31, 52) : rgb(15, 22, 34);
    uint16_t face = online ? rgb(232, 244, 255) : rgb(92, 108, 132);
    lcd_fill_ellipse(cx, cy, 55, 53, body);
    lcd_draw_arc(cx, cy, 55, -90, 270, accent);

    bool blink = (s_tick / 5u) % 24u < 2u;
    if (phase == COMPANION_PHASE_BREAK || !online) {
        lcd_line(cx - 30, cy - 7, cx - 14, cy - 7, face);
        lcd_line(cx + 14, cy - 7, cx + 30, cy - 7, face);
    } else if (blink) {
        lcd_line(cx - 30, cy - 7, cx - 14, cy - 7, face);
        lcd_line(cx + 14, cy - 7, cx + 30, cy - 7, face);
    } else {
        lcd_fill_ellipse(cx - 22, cy - 8, 6, 9, face);
        lcd_fill_ellipse(cx + 22, cy - 8, 6, 9, face);
    }

    if (phase == COMPANION_PHASE_CELEBRATE) {
        lcd_fill_ellipse(cx, cy + 18, 13, 9, accent);
        lcd_fill_ellipse(cx, cy + 14, 10, 5, body);
    } else if (phase == COMPANION_PHASE_BREAK || !online) {
        lcd_draw_arc(cx, cy + 8, 16, 20, 160, face);
    } else {
        lcd_line(cx - 12, cy + 18, cx + 12, cy + 18, face);
    }

    lcd_fill_ellipse(cx - 37, cy + 14, 5, 3, rgb(245, 143, 168));
    lcd_fill_ellipse(cx + 37, cy + 14, 5, 3, rgb(245, 143, 168));
}

void companion_ui_init(void)
{
    s_tick = 0;
}

void companion_ui_render(const companion_state_t *state)
{
    if (!state) return;
    s_tick++;
    lcd_clear(rgb(5, 9, 20));
    draw_header(state);

    uint16_t accent = phase_color(state->phase, state->online);
    static const int8_t breath[] = {0, 1, 2, 1};
    int cy = 108 + breath[(s_tick / 3u) % 4u];

    uint32_t total = state->total_seconds ? state->total_seconds : 1;
    uint32_t done = total > state->remaining_seconds ? total - state->remaining_seconds : 0;
    int percent = total > 0x7fffffffu ? 0 : (int)(done * 100u / total);
    lcd_draw_arc(120, cy, 72, -90, 270, rgb(30, 45, 70));
    if (percent > 0) lcd_draw_arc(120, cy, 72, -90, -90 + percent * 360 / 100, accent);

    draw_face(state->phase, state->online, 120, cy, accent);

    centered_small(120, 184, phase_label(state->phase), accent);
    char value[16];
    snprintf(value, sizeof(value), "%02lu:%02lu",
             (unsigned long)(state->remaining_seconds / 60u),
             (unsigned long)(state->remaining_seconds % 60u));
    centered_text(120, 198, value, rgb(236, 244, 255));
    centered_small(120, 222, phase_phrase(state->phase, state->online), rgb(159, 177, 201));
    lcd_flush();
}
