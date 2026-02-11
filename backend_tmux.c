/*
 * backend_tmux.c - Linux tmux backend for teleterm
 *
 * Implements the backend interface (backend.h) using tmux CLI commands.
 * All terminal operations go through popen() calls to the tmux binary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"

/* ============================================================================
 * Helper: run a shell command and capture output as sds string
 * ========================================================================= */

static sds run_cmd(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    sds out = sdsempty();
    char buf[4096];
    size_t n;

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        out = sdscatlen(out, buf, n);
    }

    int status = pclose(fp);
    if (status != 0) {
        sdsfree(out);
        return NULL;
    }

    return out;
}

/* ============================================================================
 * Shell escaping helper
 * ========================================================================= */

/* Build a single-quoted shell string from raw input.
 * Any embedded single quote is escaped as: '\'' */
static sds shell_escape(const char *s) {
    sds out = sdsnew("'");
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            out = sdscat(out, "'\\''");
        } else {
            out = sdscatlen(out, p, 1);
        }
    }
    out = sdscat(out, "'");
    return out;
}

/* ============================================================================
 * backend_list — enumerate tmux panes
 * ========================================================================= */

int backend_list(void) {
    backend_free_list();

    sds output = run_cmd(
        "tmux list-panes -a -F "
        "'#{pane_id}\t#{session_name}:#{window_index}.#{pane_index}"
        "\t#{pane_pid}\t#{pane_title}'"
    );
    if (!output) return 0;

    if (sdslen(output) == 0) {
        sdsfree(output);
        return 0;
    }

    /* Count lines to pre-allocate. */
    int lines = 0;
    for (size_t i = 0; i < sdslen(output); i++) {
        if (output[i] == '\n') lines++;
    }
    /* If the output does not end with newline, count the last line too. */
    if (sdslen(output) > 0 && output[sdslen(output) - 1] != '\n') lines++;

    if (lines == 0) {
        sdsfree(output);
        return 0;
    }

    TermList = malloc(lines * sizeof(TermInfo));
    if (!TermList) {
        sdsfree(output);
        return 0;
    }

    /* Parse each line: pane_id \t name \t pid \t title */
    int count;
    sds *rows = sdssplitlen(output, sdslen(output), "\n", 1, &count);
    sdsfree(output);

    if (!rows) return 0;

    for (int i = 0; i < count; i++) {
        if (sdslen(rows[i]) == 0) continue;

        int ncols;
        sds *cols = sdssplitlen(rows[i], sdslen(rows[i]), "\t", 1, &ncols);
        if (!cols) continue;

        if (ncols >= 4) {
            TermInfo *t = &TermList[TermCount];

            strncpy(t->id, cols[0], sizeof(t->id) - 1);
            t->id[sizeof(t->id) - 1] = '\0';

            strncpy(t->name, cols[1], sizeof(t->name) - 1);
            t->name[sizeof(t->name) - 1] = '\0';

            t->pid = (pid_t)atoi(cols[2]);

            strncpy(t->title, cols[3], sizeof(t->title) - 1);
            t->title[sizeof(t->title) - 1] = '\0';

            TermCount++;
        }

        sdsfreesplitres(cols, ncols);
    }

    sdsfreesplitres(rows, count);
    return TermCount;
}

/* ============================================================================
 * backend_free_list — free enumerated pane list
 * ========================================================================= */

void backend_free_list(void) {
    if (TermList) {
        free(TermList);
        TermList = NULL;
    }
    TermCount = 0;
}

/* ============================================================================
 * backend_connected — check if connected pane is still alive
 * ========================================================================= */

int backend_connected(void) {
    if (!Connected) return 0;

    sds escaped_id = shell_escape(ConnectedId);
    sds cmd = sdscatprintf(sdsempty(),
        "tmux display-message -t %s -p '#{pane_id}'", escaped_id);
    sdsfree(escaped_id);

    sds result = run_cmd(cmd);
    sdsfree(cmd);

    if (!result) return 0;

    /* Trim trailing whitespace. */
    sdstrim(result, " \t\r\n");

    /* If we got output matching the pane id pattern, it's alive. */
    int alive = (sdslen(result) > 0 && result[0] == '%');
    sdsfree(result);

    return alive ? 1 : 0;
}

/* ============================================================================
 * backend_capture_text — capture visible pane content
 * ========================================================================= */

sds backend_capture_text(void) {
    if (!Connected) return NULL;

    sds escaped_id = shell_escape(ConnectedId);
    sds cmd = sdscatprintf(sdsempty(),
        "tmux capture-pane -t %s -p", escaped_id);
    sdsfree(escaped_id);

    /* Use popen directly so we can capture output even on "failure"
     * (capture-pane returns the text on stdout). */
    FILE *fp = popen(cmd, "r");
    sdsfree(cmd);
    if (!fp) return NULL;

    sds text = sdsempty();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        text = sdscatlen(text, buf, n);
    }
    pclose(fp);

    if (sdslen(text) == 0) {
        sdsfree(text);
        return NULL;
    }

    /* Strip trailing blank lines. */
    size_t len = sdslen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == ' ')) {
        len--;
    }
    /* Keep one trailing newline for readability. */
    if (len < sdslen(text)) {
        text[len] = '\0';
        sdssetlen(text, len);
    }

    return sdslen(text) > 0 ? text : (sdsfree(text), (sds)NULL);
}

/* ============================================================================
 * backend_send_keys — send keystrokes to connected tmux pane
 * ========================================================================= */

/* Send a single tmux send-keys command (non-literal mode) for special keys. */
static int tmux_send_key(const char *pane_id, const char *key) {
    sds escaped_id = shell_escape(pane_id);
    sds escaped_key = shell_escape(key);
    sds cmd = sdscatprintf(sdsempty(),
        "tmux send-keys -t %s %s", escaped_id, escaped_key);
    sdsfree(escaped_id);
    sdsfree(escaped_key);

    sds result = run_cmd(cmd);
    sdsfree(cmd);

    if (result) {
        sdsfree(result);
        return 0;
    }
    return -1;
}

/* Send literal text to tmux pane (using -l flag). */
static int tmux_send_literal(const char *pane_id, const char *text, size_t len) {
    if (len == 0) return 0;

    /* Build the literal string to send. */
    sds literal = sdsnewlen(text, len);
    sds escaped_id = shell_escape(pane_id);
    sds escaped_text = shell_escape(literal);
    sdsfree(literal);

    sds cmd = sdscatprintf(sdsempty(),
        "tmux send-keys -t %s -l %s", escaped_id, escaped_text);
    sdsfree(escaped_id);
    sdsfree(escaped_text);

    sds result = run_cmd(cmd);
    sdsfree(cmd);

    if (result) {
        sdsfree(result);
        return 0;
    }
    return -1;
}

int backend_send_keys(const char *text) {
    if (!Connected) return -1;

    /* Check if we should suppress trailing newline. */
    int add_newline = !ends_with_purple_heart(text);

    const unsigned char *p = (const unsigned char *)text;
    size_t len = strlen(text);

    /* If ends with purple heart, reduce length to skip it. */
    if (!add_newline && len >= 4) {
        len -= 4;
    }

    int mods = 0;          /* Accumulated modifiers. */
    int consumed;
    char heart;
    int keycount = 0;       /* Number of actual keystrokes sent. */
    int had_mods = 0;       /* True if any keystroke used modifiers. */
    int last_was_nl = 0;    /* True if last keystroke was Enter. */

    /* Buffer for accumulating literal text (no modifiers). */
    const unsigned char *literal_start = NULL;
    size_t literal_len = 0;

    /* Flush any accumulated literal text. */
    #define FLUSH_LITERAL() do { \
        if (literal_len > 0) { \
            tmux_send_literal(ConnectedId, (const char *)literal_start, literal_len); \
            literal_start = NULL; \
            literal_len = 0; \
        } \
    } while (0)

    while (len > 0) {
        /* Red heart: Ctrl modifier. */
        if ((consumed = match_red_heart(p, len)) > 0) {
            FLUSH_LITERAL();
            mods |= 1; /* Ctrl */
            p += consumed; len -= consumed;
            continue;
        }

        /* Orange heart: Enter key. */
        if ((consumed = match_orange_heart(p, len)) > 0) {
            FLUSH_LITERAL();
            if (mods) {
                /* Ctrl+Enter or other modified Enter — though unusual. */
                sds key = sdsempty();
                if (mods & 1) key = sdscat(key, "C-");
                if (mods & 2) key = sdscat(key, "M-");
                key = sdscat(key, "Enter");
                tmux_send_key(ConnectedId, key);
                sdsfree(key);
                had_mods = 1;
            } else {
                tmux_send_key(ConnectedId, "Enter");
            }
            keycount++; last_was_nl = 1; mods = 0;
            p += consumed; len -= consumed;
            continue;
        }

        /* Colored hearts: 💙=Alt, 💚=Cmd (skip on Linux), 💛=ESC. */
        if ((consumed = match_colored_heart(p, len, &heart)) > 0) {
            FLUSH_LITERAL();
            if (heart == 'Y') {
                /* Yellow heart: send Escape immediately. */
                tmux_send_key(ConnectedId, "Escape");
                keycount++; had_mods = 1; last_was_nl = 0;
                mods = 0;
            } else if (heart == 'B') {
                /* Blue heart: Alt/Meta modifier. */
                mods |= 2; /* Alt */
            } else if (heart == 'G') {
                /* Green heart: Cmd — not meaningful on Linux, skip. */
            }
            p += consumed; len -= consumed;
            continue;
        }

        /* Backslash escape sequences. */
        last_was_nl = 0;
        if (*p == '\\' && len > 1) {
            if (p[1] == 'n') {
                FLUSH_LITERAL();
                if (mods) {
                    sds key = sdsempty();
                    if (mods & 1) key = sdscat(key, "C-");
                    if (mods & 2) key = sdscat(key, "M-");
                    key = sdscat(key, "Enter");
                    tmux_send_key(ConnectedId, key);
                    sdsfree(key);
                    had_mods = 1;
                } else {
                    tmux_send_key(ConnectedId, "Enter");
                }
                keycount++; last_was_nl = 1; mods = 0;
                p += 2; len -= 2;
                continue;
            } else if (p[1] == 't') {
                FLUSH_LITERAL();
                if (mods) {
                    sds key = sdsempty();
                    if (mods & 1) key = sdscat(key, "C-");
                    if (mods & 2) key = sdscat(key, "M-");
                    key = sdscat(key, "Tab");
                    tmux_send_key(ConnectedId, key);
                    sdsfree(key);
                    had_mods = 1;
                } else {
                    tmux_send_key(ConnectedId, "Tab");
                }
                keycount++; mods = 0;
                p += 2; len -= 2;
                continue;
            } else if (p[1] == '\\') {
                /* Literal backslash. */
                FLUSH_LITERAL();
                if (mods) {
                    sds key = sdsempty();
                    if (mods & 1) key = sdscat(key, "C-");
                    if (mods & 2) key = sdscat(key, "M-");
                    key = sdscat(key, "\\");
                    tmux_send_key(ConnectedId, key);
                    sdsfree(key);
                    had_mods = 1;
                } else {
                    tmux_send_literal(ConnectedId, "\\", 1);
                }
                keycount++; mods = 0;
                p += 2; len -= 2;
                continue;
            }
        }

        /* Regular character. */
        if (mods) {
            /* Modified key: send via send-keys (non-literal). */
            FLUSH_LITERAL();
            sds key = sdsempty();
            if (mods & 1) key = sdscat(key, "C-");
            if (mods & 2) key = sdscat(key, "M-");
            key = sdscatlen(key, (const char *)p, 1);
            tmux_send_key(ConnectedId, key);
            sdsfree(key);
            had_mods = 1;
            keycount++; mods = 0;
            p++; len--;
        } else {
            /* Unmodified: accumulate into literal buffer. */
            if (literal_len == 0) {
                literal_start = p;
            }
            literal_len++;
            keycount++;
            p++; len--;
        }
    }

    /* Flush remaining literal text. */
    FLUSH_LITERAL();

    #undef FLUSH_LITERAL

    /* Add newline unless:
     * - Suppressed by purple heart
     * - Single modified keystroke (like Ctrl+C) or bare ESC
     * - Last explicit keystroke was already a newline */
    if (add_newline && !(keycount == 1 && had_mods) && !last_was_nl) {
        tmux_send_key(ConnectedId, "Enter");
    }

    return 0;
}
