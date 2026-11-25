#pragma once
#include <stdint.h>

#define LCD_WIDTH  240
#define LCD_HEIGHT 240

/* RGB888 -> RGB565。本面板调用 invert_color(true) 后实测为正常颜色映射。 */
#define RGB565(r, g, b) ((((uint16_t)((r) & 0xF8)) << 8) | (((uint16_t)((g) & 0xFC)) << 3) | ((uint16_t)(b) >> 3))
#define LCD_RGB(r, g, b) RGB565((r), (g), (b))

#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_BLUE    0x001F
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_MAGENTA 0xF81F
#define C_GRAY    0x8410
#define C_DARK    0x1082

/* 设计系统专用色 */
#define C_PAPER   0xF7BE   /* #F7F5F0 暖白纸底 */
#define C_NAVY    0x1948   /* #1A2A45 深navy */
#define C_CORAL   0xE268   /* #E04F46 珊瑚红 */
#define C_TEAL    0x24ED   /* #239E6B 青绿 */
#define C_SOFT    0xEF7E   /* #E9ECF2 浅灰蓝 */
#define C_MUTED   0xCE7A   /* #C9CFD6 中灰 */
#define C_PINK    0xF5D6   /* #F2B8B5 腮红粉 */

/* 霓虹舱配色 */
#define C_VOID     0x0884   /* #0A1024 深空底 */
#define C_GRID     0x1108   /* #162240 暗网格 */
#define C_PANEL    0x10E7   /* #131D38 玻璃面板 */
#define C_EDGE     0x29EE   /* #2A3F72 面板描边 */
#define C_NEON     0x4F5F   /* #4DE8FF 电光青 */
#define C_VIOLET   0xA45F   /* #A78BFA 思考紫 */
#define C_MINT     0x5F96   /* #5EF2B0 完成绿 */
#define C_AMBER    0xFE4D   /* #FFC96B 待机黄 */
#define C_ROSE     0xFB51   /* #FF6B8A 警示粉 */
#define C_ICE      0xEFBF   /* #E8F4FF 冷白 */
#define C_GLOW     0x1A6D   /* #1B4E6B 内层辉光 */
#define C_DIMGLOW  0x11CA   /* #123A55 外层辉光 */

uint16_t lcd_rgb(uint8_t r, uint8_t g, uint8_t b);
uint16_t lcd_blend(uint16_t bg, uint16_t fg, uint8_t alpha);

void lcd_init(void);

/* Backlight API. percent is clamped to [0, 100]; default comes from Kconfig. */
void lcd_set_brightness(uint8_t percent);
uint8_t lcd_get_brightness(void);

/*
 * RGB565 channel calibration applied to the DMA staging copy in lcd_flush().
 * Gains are percentages (0..255); offsets are signed 5/6/5 channel units.
 * The default is 100/100/100 percent with zero offsets.
 */
void lcd_set_calibration(uint8_t r_gain_percent, uint8_t g_gain_percent, uint8_t b_gain_percent,
                         int16_t r_offset, int16_t g_offset, int16_t b_offset);

void lcd_clear(uint16_t color);
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);
void lcd_set_pixel(int x, int y, uint16_t color);
void lcd_hline(int x, int y, int w, uint16_t color);
void lcd_line(int x0, int y0, int x1, int y1, uint16_t color);
void lcd_thick_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color);
void lcd_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
void lcd_fill_roundrect(int x, int y, int w, int h, int r, uint16_t color);
void lcd_fill_roundrect_grad(int x, int y, int w, int h, int r, uint16_t c_top, uint16_t c_bot);
void lcd_draw_roundrect(int x, int y, int w, int h, int r, uint16_t color);
void lcd_fill_ellipse(int cx, int cy, int rx, int ry, uint16_t color);
void lcd_draw_arc(int cx, int cy, int r, int a0, int a1, uint16_t color);
void lcd_set_clip(int x0, int x1);   /* 字符水平裁剪范围 [x0,x1)，默认全屏；跑马灯用 */
void lcd_draw_char(int x, int y, char ch, uint16_t color);
void lcd_draw_text(int x, int y, const char *str, uint16_t color);
void lcd_draw_text_len(int x, int y, const char *str, int len, uint16_t color);
void lcd_draw_char_sm(int x, int y, char ch, uint16_t color);
void lcd_draw_text_sm(int x, int y, const char *str, uint16_t color);
void lcd_draw_text_sm_len(int x, int y, const char *str, int len, uint16_t color);
void lcd_draw_text_sm_wrap(int x, int y, int max_w, int max_lines, int line_h,
                            const char *str, uint16_t color);
void lcd_flush(void);
