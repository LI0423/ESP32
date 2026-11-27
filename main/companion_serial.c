#include "companion_serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "companion_progress.h"

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *skip_space(const char *p)
{
    while (*p && is_space(*p)) p++;
    return p;
}

static bool word_is(const char *word, size_t length, const char *expected)
{
    size_t expected_length = strlen(expected);
    return length == expected_length && strncmp(word, expected, length) == 0;
}

/* Strict decimal: digits only, trailing spaces allowed, range checked by caller. */
static bool parse_uint(const char *text, uint32_t *out)
{
    const char *p = skip_space(text);
    if (*p < '0' || *p > '9') return false;
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10u + (uint32_t)(*p - '0');
        if (value > 1000u) return false;
        p++;
    }
    if (*skip_space(p) != '\0') return false;
    *out = value;
    return true;
}

void companion_serial_parse(const char *line, companion_serial_result_t *out)
{
    if (!out) return;
    out->command = COMPANION_SERIAL_CMD_UNKNOWN;
    out->goal = 0;
    if (!line) return;

    const char *p = skip_space(line);
    const char *word = p;
    while (*p && !is_space(*p)) p++;
    size_t word_length = (size_t)(p - word);
    const char *rest = skip_space(p);
    bool no_args = (*rest == '\0');

    if (word_length == 0) return; /* blank line: ignore */

    if (word_is(word, word_length, "token")) {
        out->command = no_args ? COMPANION_SERIAL_CMD_TOKEN : COMPANION_SERIAL_CMD_INVALID;
        return;
    }
    if (word_is(word, word_length, "status")) {
        out->command = no_args ? COMPANION_SERIAL_CMD_STATUS : COMPANION_SERIAL_CMD_INVALID;
        return;
    }
    if (word_is(word, word_length, "focus")) {
        out->command = no_args ? COMPANION_SERIAL_CMD_FOCUS : COMPANION_SERIAL_CMD_INVALID;
        return;
    }
    if (word_is(word, word_length, "standby")) {
        out->command = no_args ? COMPANION_SERIAL_CMD_STANDBY : COMPANION_SERIAL_CMD_INVALID;
        return;
    }
    if (word_is(word, word_length, "feed")) {
        out->command = no_args ? COMPANION_SERIAL_CMD_FEED : COMPANION_SERIAL_CMD_INVALID;
        return;
    }
    if (word_is(word, word_length, "medicine")) {
        out->command = no_args ? COMPANION_SERIAL_CMD_MEDICINE : COMPANION_SERIAL_CMD_INVALID;
        return;
    }
    if (word_is(word, word_length, "play")) {
        out->command = no_args ? COMPANION_SERIAL_CMD_PLAY : COMPANION_SERIAL_CMD_INVALID;
        return;
    }
    if (word_is(word, word_length, "clean")) {
        out->command = no_args ? COMPANION_SERIAL_CMD_CLEAN : COMPANION_SERIAL_CMD_INVALID;
        return;
    }
    if (word_is(word, word_length, "goal")) {
        uint32_t goal = 0;
        if (!parse_uint(rest, &goal) ||
            goal < COMPANION_DAILY_GOAL_MIN || goal > COMPANION_DAILY_GOAL_MAX) {
            out->command = COMPANION_SERIAL_CMD_INVALID;
            return;
        }
        out->command = COMPANION_SERIAL_CMD_GOAL;
        out->goal = goal;
    }
}
