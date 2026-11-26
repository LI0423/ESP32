#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest line the USB serial command reader accepts, including the NUL. */
#define COMPANION_SERIAL_LINE_MAX 64

typedef enum {
    COMPANION_SERIAL_CMD_UNKNOWN = 0, /* ignore silently */
    COMPANION_SERIAL_CMD_TOKEN,
    COMPANION_SERIAL_CMD_STATUS,
    COMPANION_SERIAL_CMD_GOAL,
    COMPANION_SERIAL_CMD_FOCUS,
    COMPANION_SERIAL_CMD_STANDBY,
    COMPANION_SERIAL_CMD_INVALID,     /* recognised command, bad arguments */
} companion_serial_command_t;

typedef struct {
    companion_serial_command_t command;
    uint32_t goal; /* valid only for COMPANION_SERIAL_CMD_GOAL */
} companion_serial_result_t;

/*
 * Parse one NUL-terminated line (CR/LF already stripped or not; both work).
 * No ESP-IDF, NVS or model dependency so it can be host-tested.
 */
void companion_serial_parse(const char *line, companion_serial_result_t *out);

#ifdef __cplusplus
}
#endif
