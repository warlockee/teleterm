/*
 * emoji.c - Emoji parsing for teleterm
 *
 * Shared between bot_common.c and teleterm_ctl.c.
 * Parses UTF-8 emoji modifiers used for keystroke control.
 */

#include <string.h>
#include "backend.h"

/* Match red heart (E2 9D A4, optionally followed by EF B8 8F). */
int match_red_heart(const unsigned char *p, size_t remaining) {
    if (remaining >= 3 && p[0] == 0xE2 && p[1] == 0x9D && p[2] == 0xA4) {
        if (remaining >= 6 && p[3] == 0xEF && p[4] == 0xB8 && p[5] == 0x8F)
            return 6;
        return 3;
    }
    return 0;
}

/* Match colored hearts: blue (F0 9F 92 99), green (9A), yellow (9B). */
int match_colored_heart(const unsigned char *p, size_t remaining, char *heart) {
    if (remaining >= 4 && p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0x92) {
        if (p[3] == 0x99) { *heart = 'B'; return 4; }  /* Blue = Alt */
        if (p[3] == 0x9A) { *heart = 'G'; return 4; }  /* Green = Cmd */
        if (p[3] == 0x9B) { *heart = 'Y'; return 4; }  /* Yellow = ESC */
    }
    return 0;
}

/* Match orange heart (F0 9F A7 A1) - sends Enter. */
int match_orange_heart(const unsigned char *p, size_t remaining) {
    if (remaining >= 4 && p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0xA7 && p[3] == 0xA1)
        return 4;
    return 0;
}

/* Match purple heart (F0 9F 92 9C) - used to suppress newline. */
int match_purple_heart(const unsigned char *p, size_t remaining) {
    if (remaining >= 4 && p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0x92 && p[3] == 0x9C)
        return 4;
    return 0;
}

/* Check if string ends with purple heart. */
int ends_with_purple_heart(const char *text) {
    size_t len = strlen(text);
    if (len >= 4) {
        const unsigned char *p = (const unsigned char *)text + len - 4;
        if (match_purple_heart(p, 4)) return 1;
    }
    return 0;
}
