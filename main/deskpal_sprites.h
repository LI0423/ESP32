#pragma once

#include <stdint.h>

enum {
    DESKPAL_CHAR_W = 128,
    DESKPAL_CHAR_H = 128,
    DESKPAL_CHAR_X = 56,
    DESKPAL_CHAR_Y = 43,
    DESKPAL_STAGE_CX = 120,
    DESKPAL_STAGE_CY = 20,
    DESKPAL_TIME_CX = 120,
    DESKPAL_TIME_CY = 198,
    DESKPAL_TODAY_CX = 120,
    DESKPAL_TODAY_CY = 219,
    DESKPAL_DOT_CX = 222,
    DESKPAL_DOT_CY = 18,
    DESKPAL_DOT_R = 5,
    DESKPAL_PALETTE_LEN = 32,
    DESKPAL_TRANSPARENT_INDEX = 0,
};

/* RGB565 */
enum {
    DESKPAL_BG_FOCUS = 0x4942,
    DESKPAL_BG_BREAK = 0x438B,
    DESKPAL_BG_COMPLETE = 0xE505,
    DESKPAL_BG_STANDBY = 0x08A5,
    DESKPAL_DOT_ONLINE = 0x2EAD,
};

typedef enum {
    DESKPAL_SPRITE_FOCUS = 0,
    DESKPAL_SPRITE_BREAK,
    DESKPAL_SPRITE_COMPLETE,
    DESKPAL_SPRITE_STANDBY,
    DESKPAL_SPRITE_COUNT
} deskpal_sprite_id_t;

#ifdef __cplusplus
extern "C" {
#endif

extern const uint16_t deskpal_palette[DESKPAL_PALETTE_LEN];
extern const uint8_t deskpal_sprite_focus[DESKPAL_CHAR_W * DESKPAL_CHAR_H];
extern const uint8_t deskpal_sprite_break[DESKPAL_CHAR_W * DESKPAL_CHAR_H];
extern const uint8_t deskpal_sprite_complete[DESKPAL_CHAR_W * DESKPAL_CHAR_H];
extern const uint8_t deskpal_sprite_standby[DESKPAL_CHAR_W * DESKPAL_CHAR_H];
const uint8_t *deskpal_sprite_data(deskpal_sprite_id_t id);

#ifdef __cplusplus
}
#endif
