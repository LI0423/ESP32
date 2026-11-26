/*
 * LCD 驱动 —— Waveshare ESP32-S3-LCD-1.54 (ST7789, 240x240, SPI)
 * 参数参照 WeiYi/m1ai/m1esp32 的 bread-compact-wifi-lcd 配置:
 * spi_mode=0, 20MHz, invert_color=true, little-endian normal RGB565
 */
#include <string.h>
#include <assert.h>
#include <math.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_io_spi.h"
#include "lcd.h"
#include "font8x16.h"
#include "font8x16_sm.h"

#define TAG "lcd"

/* 用户实测接线表（GOOUU ESP32-S3-CAM 板） */
#define PIN_SCLK  21   /* SCL  */
#define PIN_MOSI  47   /* SDA  */
#define PIN_CS    41   /* CS   */
#define PIN_DC    40   /* DC   */
#define PIN_RST   45   /* RES  */
#define PIN_BL    42   /* BLK  */
#define LCD_SPI_HOST  SPI3_HOST   /* 与 m1esp32 bread-compact-wifi-lcd 配置一致 */
#define PCLK_HZ   (20 * 1000 * 1000)  /* 杜邦线/面包板上比 40MHz 更稳定 */
#define DMA_ROWS  20                 /* 与参考工程一致：每次只 DMA 20 行 */
#define BACKLIGHT_DUTY_MAX ((1U << LEDC_TIMER_10_BIT) - 1U)

/* Fallbacks keep this driver buildable before the project Kconfig is regenerated. */
#ifndef CONFIG_PAD_DEFAULT_BRIGHTNESS
#define CONFIG_PAD_DEFAULT_BRIGHTNESS 75
#endif
#ifndef CONFIG_PAD_CAL_R_GAIN
#define CONFIG_PAD_CAL_R_GAIN 100
#endif
#ifndef CONFIG_PAD_CAL_G_GAIN
#define CONFIG_PAD_CAL_G_GAIN 100
#endif
#ifndef CONFIG_PAD_CAL_B_GAIN
#define CONFIG_PAD_CAL_B_GAIN 100
#endif
#ifndef CONFIG_PAD_CAL_R_OFFSET
#define CONFIG_PAD_CAL_R_OFFSET 0
#endif
#ifndef CONFIG_PAD_CAL_G_OFFSET
#define CONFIG_PAD_CAL_G_OFFSET 0
#endif
#ifndef CONFIG_PAD_CAL_B_OFFSET
#define CONFIG_PAD_CAL_B_OFFSET 0
#endif

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_flush_done = NULL;
static bool s_backlight_ready = false;
static volatile uint8_t s_brightness_percent = CONFIG_PAD_DEFAULT_BRIGHTNESS;

/* Integer lookup tables avoid floating-point work for every pixel of every frame. */
typedef struct {
    uint16_t r[32];
    uint16_t g[64];
    uint16_t b[32];
    bool active;
} lcd_calibration_luts_t;

static lcd_calibration_luts_t s_calibration;
static portMUX_TYPE s_calibration_lock = portMUX_INITIALIZER_UNLOCKED;

static IRAM_ATTR bool lcd_color_trans_done(esp_lcd_panel_io_handle_t io,
                                           esp_lcd_panel_io_event_data_t *edata,
                                           void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t task_woken = pdFALSE;
    if (s_flush_done) xSemaphoreGiveFromISR(s_flush_done, &task_woken);
    portYIELD_FROM_ISR(task_woken);
    return true;
}

/* CPU 绘图帧放 PSRAM；DMA 只使用内部 RAM 中的 20 行 staging buffer。 */
static uint16_t *s_fb = NULL;
static uint16_t *s_dma_fb = NULL;
static int s_clip_x0 = 0, s_clip_x1 = LCD_WIDTH;   /* 字符水平裁剪 */

void lcd_set_clip(int x0, int x1)
{
    if (x0 < 0) x0 = 0;
    if (x1 > LCD_WIDTH) x1 = LCD_WIDTH;
    if (x1 < x0) x1 = x0;
    s_clip_x0 = x0;
    s_clip_x1 = x1;
}

void lcd_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_brightness_percent = percent;
    if (!s_backlight_ready) return;

    uint32_t duty = ((uint32_t)percent * BACKLIGHT_DUTY_MAX + 50U) / 100U;
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (err == ESP_OK) err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (err != ESP_OK) ESP_LOGE(TAG, "set brightness %u%% failed: %s", percent, esp_err_to_name(err));
}

uint8_t lcd_get_brightness(void)
{
    return s_brightness_percent;
}

static uint16_t calibrated_channel(int value, int maximum, uint8_t gain, int16_t offset)
{
    int adjusted = (value * gain + 50) / 100 + offset;
    if (adjusted < 0) adjusted = 0;
    if (adjusted > maximum) adjusted = maximum;
    return (uint16_t)adjusted;
}

void lcd_set_calibration(uint8_t r_gain_percent, uint8_t g_gain_percent, uint8_t b_gain_percent,
                         int16_t r_offset, int16_t g_offset, int16_t b_offset)
{
    lcd_calibration_luts_t next;
    for (int i = 0; i < 32; i++) {
        next.r[i] = (uint16_t)(calibrated_channel(i, 31, r_gain_percent, r_offset) << 11);
        next.b[i] = calibrated_channel(i, 31, b_gain_percent, b_offset);
    }
    for (int i = 0; i < 64; i++) {
        next.g[i] = (uint16_t)(calibrated_channel(i, 63, g_gain_percent, g_offset) << 5);
    }
    next.active = r_gain_percent != 100 || g_gain_percent != 100 || b_gain_percent != 100 ||
                  r_offset != 0 || g_offset != 0 || b_offset != 0;

    portENTER_CRITICAL(&s_calibration_lock);
    s_calibration = next;
    portEXIT_CRITICAL(&s_calibration_lock);
}

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_BL,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    s_backlight_ready = true;
    /* 默认 CONFIG_PAD_DEFAULT_BRIGHTNESS（75% -> 10-bit duty 767）。 */
    lcd_set_brightness(s_brightness_percent);
}

void lcd_init(void)
{
    if (s_brightness_percent > 100) s_brightness_percent = 100;
    lcd_set_calibration(CONFIG_PAD_CAL_R_GAIN, CONFIG_PAD_CAL_G_GAIN, CONFIG_PAD_CAL_B_GAIN,
                        CONFIG_PAD_CAL_R_OFFSET, CONFIG_PAD_CAL_G_OFFSET, CONFIG_PAD_CAL_B_OFFSET);

    /* 完整绘图帧放 PSRAM；SPI DMA 只读取内部 RAM 的小块 staging buffer。 */
    s_fb = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dma_fb = (uint16_t *)heap_caps_malloc(LCD_WIDTH * DMA_ROWS * sizeof(uint16_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    assert(s_fb && s_dma_fb);
    ESP_LOGI(TAG, "framebuffer @ %p (PSRAM), DMA staging @ %p (internal)", s_fb, s_dma_fb);
    memset(s_fb, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
    memset(s_dma_fb, 0, LCD_WIDTH * DMA_ROWS * sizeof(uint16_t));

    s_flush_done = xSemaphoreCreateBinary();
    assert(s_flush_done);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,   /* 纯色诊断验证过能显示的配置 */
        .pclk_hz = PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = lcd_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* 本面板调用 invert_color(true) 后实测为正常颜色映射。 */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    backlight_init();
    ESP_LOGI(TAG, "ST7789 240x240 init done (SPI3 @20MHz, mode0, 20-row DMA)");
}

uint16_t lcd_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return LCD_RGB(r, g, b);
}

uint16_t lcd_blend(uint16_t bg, uint16_t fg, uint8_t alpha)
{
    int r1 = (bg >> 11) & 0x1F, g1 = (bg >> 5) & 0x3F, b1 = bg & 0x1F;
    int r2 = (fg >> 11) & 0x1F, g2 = (fg >> 5) & 0x3F, b2 = fg & 0x1F;
    int inv = 255 - alpha;
    int r = (r1 * inv + r2 * alpha + 127) / 255;
    int g = (g1 * inv + g2 * alpha + 127) / 255;
    int bl = (b1 * inv + b2 * alpha + 127) / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

void lcd_clear(uint16_t color)
{
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) s_fb[i] = color;
}

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            s_fb[yy * LCD_WIDTH + xx] = color;
}

void lcd_set_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
    s_fb[y * LCD_WIDTH + x] = color;
}

void lcd_hline(int x, int y, int w, uint16_t color)
{
    if (w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y < 0 || y >= LCD_HEIGHT) return;
    uint16_t *p = s_fb + y * LCD_WIDTH + x;
    while (w-- > 0) *p++ = color;
}

void lcd_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (1) {
        lcd_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void lcd_thick_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    if (thickness <= 1) {
        lcd_line(x0, y0, x1, y1, color);
        return;
    }
    int r = thickness / 2;
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx);
    int ady = (dy < 0 ? -dy : dy);
    if (ady > steps) steps = ady;
    if (steps == 0) {
        lcd_fill_ellipse(x0, y0, r, r, color);
        return;
    }
    for (int i = 0; i <= steps; i++) {
        int px = x0 + (dx * i) / steps;
        int py = y0 + (dy * i) / steps;
        lcd_fill_ellipse(px, py, r, r, color);
    }
}

/* 扫描线三角形填充（用于倾斜眼睑遮罩与几何表情） */
void lcd_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    if (y0 > y1) { int tx = x0, ty = y0; x0 = x1; y0 = y1; x1 = tx; y1 = ty; }
    if (y1 > y2) { int tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }
    if (y0 > y1) { int tx = x0, ty = y0; x0 = x1; y0 = y1; x1 = tx; y1 = ty; }
    if (y0 == y2) return;

    int total_h = y2 - y0;
    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= LCD_HEIGHT) continue;
        int second_half = (y > y1) || (y1 == y0);
        int seg_h = second_half ? (y2 - y1) : (y1 - y0);
        if (seg_h == 0) seg_h = 1;
        float alpha = (float)(y - y0) / (float)total_h;
        float beta  = (float)(y - (second_half ? y1 : y0)) / (float)seg_h;
        int ax = x0 + (int)((x2 - x0) * alpha + 0.5f);
        int bx = second_half
                 ? x1 + (int)((x2 - x1) * beta + 0.5f)
                 : x0 + (int)((x1 - x0) * beta + 0.5f);
        if (ax > bx) { int t = ax; ax = bx; bx = t; }
        lcd_hline(ax, y, bx - ax + 1, color);
    }
}

/* 圆角矩形填充（垂直渐变）：逐行精确计算圆角水平半宽 */
void lcd_fill_roundrect_grad(int x, int y, int w, int h, int r,
                             uint16_t c_top, uint16_t c_bot)
{
    if (w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (int row = 0; row < h; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= LCD_HEIGHT) continue;

        int inset = 0;
        if (r > 0 && row < r) {
            double ry = (double)r - (double)row - 0.5;
            double dx = sqrt((double)r * (double)r - ry * ry);
            inset = r - (int)(dx + 0.5);
            if (inset < 0) inset = 0;
        } else if (r > 0 && row >= h - r) {
            double ry = (double)(row - (h - r)) + 0.5;
            double dx = sqrt((double)r * (double)r - ry * ry);
            inset = r - (int)(dx + 0.5);
            if (inset < 0) inset = 0;
        }

        uint16_t col = c_top;
        if (c_top != c_bot && h > 1) {
            uint8_t a = (uint8_t)((row * 255) / (h - 1));
            col = lcd_blend(c_top, c_bot, a);
        }
        lcd_hline(x + inset, yy, w - 2 * inset, col);
    }
}

/* 圆角矩形填充 */
void lcd_fill_roundrect(int x, int y, int w, int h, int r, uint16_t color)
{
    lcd_fill_roundrect_grad(x, y, w, h, r, color, color);
}

/* 圆角矩形描边：用四个小圆弧 + 四条直线，避免浮点 */
void lcd_draw_roundrect(int x, int y, int w, int h, int r, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    lcd_hline(x + r, y, w - 2 * r, color);
    lcd_hline(x + r, y + h - 1, w - 2 * r, color);
    for (int i = 0; i < h - 2 * r; i++) {
        lcd_set_pixel(x, y + r + i, color);
        lcd_set_pixel(x + w - 1, y + r + i, color);
    }
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int d = dx * dx + dy * dy;
            if (d >= (r - 1) * (r - 1) && d <= r * r) {
                lcd_set_pixel(x + r - 1 - dx, y + r - 1 - dy, color);
                lcd_set_pixel(x + w - r + dx, y + r - 1 - dy, color);
                lcd_set_pixel(x + r - 1 - dx, y + h - r + dy, color);
                lcd_set_pixel(x + w - r + dx, y + h - r + dy, color);
            }
        }
    }
}

void lcd_fill_ellipse(int cx, int cy, int rx, int ry, uint16_t color)
{
    if (rx < 1 || ry < 1) return;
    for (int dy = -ry; dy <= ry; dy++) {
        double t = 1.0 - (double)(dy * dy) / (ry * ry);
        if (t < 0) continue;
        int dx = (int)(rx * sqrt(t));
        lcd_hline(cx - dx, cy + dy, 2 * dx + 1, color);
    }
}

/* 整角度圆弧：逐度采样，去抖（每度间隔小，点会重叠，视觉平滑） */
void lcd_draw_arc(int cx, int cy, int r, int a0, int a1, uint16_t color)
{
    for (int a = a0; a <= a1; a++) {
        double rad = a * 3.14159265 / 180.0;
        int px = cx + (int)(r * cos(rad));
        int py = cy + (int)(r * sin(rad));
        lcd_set_pixel(px, py, color);
        lcd_set_pixel(px + 1, py, color);
    }
}

void lcd_draw_char(int x, int y, char ch, uint16_t color)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) return;
    const uint16_t *g = font_bits[ch - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++) {
        uint16_t bits = g[row];
        int yy = y + row;
        if (yy < 0 || yy >= LCD_HEIGHT) continue;
        for (int col = 0; col < FONT_W; col++) {
            if (!(bits & (1 << (FONT_W - 1 - col)))) continue;
            int xx = x + col;
            if (xx >= 0 && xx < LCD_WIDTH && xx >= s_clip_x0 && xx < s_clip_x1)
                s_fb[yy * LCD_WIDTH + xx] = color;
        }
    }
}

void lcd_draw_text_len(int x, int y, const char *str, int len, uint16_t color)
{
    for (int i = 0; i < len && str[i]; i++) {
        lcd_draw_char(x + i * FONT_W, y, str[i], color);
    }
}

void lcd_draw_text(int x, int y, const char *str, uint16_t color)
{
    lcd_draw_text_len(x, y, str, 0x7fffffff, color);
}

void lcd_draw_char_sm(int x, int y, char ch, uint16_t color)
{
    if (ch < FONT_SM_FIRST || ch > FONT_SM_LAST) return;
    const uint8_t *g = font_sm_bits[ch - FONT_SM_FIRST];
    for (int row = 0; row < FONT_SM_H; row++) {
        uint8_t bits = g[row];
        if (!bits) continue;
        int yy = y + row;
        if (yy < 0 || yy >= LCD_HEIGHT) continue;
        for (int col = 0; col < FONT_SM_W; col++) {
            if (!(bits & (1u << (7 - col)))) continue;
            int xx = x + col;
            if (xx >= 0 && xx < LCD_WIDTH && xx >= s_clip_x0 && xx < s_clip_x1)
                s_fb[yy * LCD_WIDTH + xx] = color;
        }
    }
}

void lcd_draw_text_sm_len(int x, int y, const char *str, int len, uint16_t color)
{
    for (int i = 0; i < len && str[i]; i++) {
        lcd_draw_char_sm(x + i * FONT_SM_W, y, str[i], color);
    }
}

void lcd_draw_text_sm(int x, int y, const char *str, uint16_t color)
{
    lcd_draw_text_sm_len(x, y, str, 0x7fffffff, color);
}

void lcd_blit_indexed(int dst_x, int dst_y, int w, int h,
                      const uint8_t *indices,
                      const uint16_t *palette, int palette_len,
                      uint8_t transparent_index)
{
    if (!s_fb || !indices || !palette || w <= 0 || h <= 0 || palette_len <= 0) return;
    for (int row = 0; row < h; row++) {
        int y = dst_y + row;
        if (y < 0 || y >= LCD_HEIGHT) continue;
        const uint8_t *src = indices + (size_t)row * (size_t)w;
        uint16_t *dst_row = s_fb + y * LCD_WIDTH;
        for (int col = 0; col < w; col++) {
            int x = dst_x + col;
            if (x < 0 || x >= LCD_WIDTH) continue;
            uint8_t idx = src[col];
            if (idx == transparent_index || (int)idx >= palette_len) continue;
            dst_row[x] = palette[idx];
        }
    }
}

/* 自动按词换行渲染 8x16 文本，超出 max_lines 时末尾加省略号 */
void lcd_draw_text_sm_wrap(int x, int y, int max_w, int max_lines, int line_h,
                           const char *str, uint16_t color)
{
    if (!str || max_w < FONT_SM_W || max_lines <= 0) return;
    int cols = max_w / FONT_SM_W;
    const char *p = str;

    for (int line = 0; line < max_lines && *p; line++) {
        while (*p == ' ') p++;
        if (!*p) break;

        int rem = (int)strlen(p);
        int take = rem;
        int truncated = 0;

        if (rem > cols) {
            if (line == max_lines - 1) {
                take = cols > 3 ? cols - 3 : cols;
                truncated = 1;
            } else {
                take = cols;
                int brk = -1;
                for (int i = cols; i > 0; i--) {
                    if (p[i] == ' ') { brk = i; break; }
                }
                if (brk > 0) take = brk;
            }
        }

        lcd_draw_text_sm_len(x, y + line * line_h, p, take, color);
        if (truncated && cols >= 3) {
            lcd_draw_text_sm(x + take * FONT_SM_W, y + line * line_h, "...", color);
        }
        p += take;
    }
}

static void lcd_apply_calibration(uint16_t *pixels, size_t count,
                                  const lcd_calibration_luts_t *calibration)
{
    for (size_t i = 0; i < count; i++) {
        uint16_t pixel = pixels[i];
        pixels[i] = calibration->r[(pixel >> 11) & 0x1F] |
                    calibration->g[(pixel >> 5) & 0x3F] |
                    calibration->b[pixel & 0x1F];
    }
}

void lcd_flush(void)
{
    /* One coherent calibration snapshot for the entire frame. */
    lcd_calibration_luts_t calibration;
    portENTER_CRITICAL(&s_calibration_lock);
    calibration = s_calibration;
    portEXIT_CRITICAL(&s_calibration_lock);

    /* 参考工程的分块刷屏方式：每次只发送 20 行，并等待该块 DMA 完成，
     * 然后才改写 staging buffer。这样不会混合前后两帧，也不会出现整屏跑马。 */
    for (int y = 0; y < LCD_HEIGHT; y += DMA_ROWS) {
        int rows = DMA_ROWS;
        if (y + rows > LCD_HEIGHT) rows = LCD_HEIGHT - y;

        size_t pixels = (size_t)rows * LCD_WIDTH;
        memcpy(s_dma_fb, s_fb + y * LCD_WIDTH, pixels * sizeof(uint16_t));
        /* Calibrate only the staging copy, never the reusable PSRAM framebuffer. */
        if (calibration.active) lcd_apply_calibration(s_dma_fb, pixels, &calibration);
        xSemaphoreTake(s_flush_done, 0);  /* 清掉残留通知 */

        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + rows, s_dma_fb);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "draw_bitmap y=%d err=0x%x", y, err);
            return;
        }
        if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGE(TAG, "flush timeout y=%d", y);
            /* 排空 SPI 队列，避免下一帧覆盖仍在传输的 staging buffer。 */
            esp_lcd_panel_io_tx_param(s_io, -1, NULL, 0);
            xSemaphoreTake(s_flush_done, 0);
            return;
        }
    }
}
