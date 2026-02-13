/*
 * bot_common.c - Shared logic for teleterm Telegram bot
 *
 * Platform-independent code: TOTP auth, emoji parsing, command handling,
 * text formatting, and main(). Delegates to backend_*.c via backend.h
 * for terminal listing, text capture, and keystroke delivery.
 *
 * Commands:
 *   .list    - List available terminal sessions
 *   .1 .2 .. - Connect to session by number
 *   .help    - Show help
 *
 * Once connected, any text is sent as keystrokes (newline auto-added).
 * End with a purple heart to suppress the automatic newline.
 * Emoji modifiers: red heart (Ctrl), blue heart (Alt), green heart (Cmd),
 *                  yellow heart (ESC), orange heart (Enter)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>

#include "backend.h"
#include "botlib.h"
#include "sha1.h"
#include "qrcodegen.h"

/* ============================================================================
 * Shared state (declared extern in backend.h)
 * ========================================================================= */

TermInfo *TermList = NULL;
int TermCount = 0;
int Connected = 0;
char ConnectedId[128] = {0};
pid_t ConnectedPid = 0;
char ConnectedName[128] = {0};
char ConnectedTitle[256] = {0};
int DangerMode = 0;

/* ============================================================================
 * Internal state
 * ========================================================================= */

static pthread_mutex_t RequestLock = PTHREAD_MUTEX_INITIALIZER;
static int WeakSecurity = 0;          /* If 1, skip all OTP logic. */
static int Authenticated = 0;         /* Whether OTP has been verified. */
static time_t LastActivity = 0;       /* Last time owner sent a valid command. */
static int OtpTimeout = 300;          /* Timeout in seconds (default 5 min). */

#define MAX_TRACKED_MSGS 16
static int64_t TrackedMsgIds[MAX_TRACKED_MSGS];
static int TrackedMsgCount = 0;

/* ============================================================================
 * TOTP Authentication
 * ========================================================================= */

/* Encode raw bytes to Base32 string (RFC 4648). Returns static buffer. */
static const char *base32_encode(const unsigned char *data, size_t len) {
    static char out[128];
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    int i = 0, j = 0;
    uint64_t buf = 0;
    int bits = 0;

    for (i = 0; i < (int)len; i++) {
        buf = (buf << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out[j++] = alphabet[(buf >> bits) & 0x1f];
        }
    }
    if (bits > 0) {
        out[j++] = alphabet[(buf << (5 - bits)) & 0x1f];
    }
    out[j] = '\0';
    return out;
}

/* Compute 6-digit TOTP code from raw secret and time step. */
static uint32_t totp_code(const unsigned char *secret, size_t secret_len,
                          uint64_t time_step)
{
    unsigned char msg[8];
    for (int i = 7; i >= 0; i--) {
        msg[i] = (unsigned char)(time_step & 0xff);
        time_step >>= 8;
    }

    unsigned char hash[SHA1_DIGEST_SIZE];
    hmac_sha1(secret, secret_len, msg, 8, hash);

    int offset = hash[19] & 0x0f;
    uint32_t code = ((uint32_t)(hash[offset] & 0x7f) << 24)
                  | ((uint32_t)hash[offset+1] << 16)
                  | ((uint32_t)hash[offset+2] << 8)
                  | (uint32_t)hash[offset+3];
    return code % 1000000;
}

/* Print QR code as compact ASCII art using half-block characters.
 * Each output line encodes two QR rows using upper/lower half blocks. */
static void print_qr_ascii(const char *text) {
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempbuf[qrcodegen_BUFFER_LEN_MAX];

    if (!qrcodegen_encodeText(text, tempbuf, qrcode,
            qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
            qrcodegen_Mask_AUTO, true)) {
        printf("Failed to generate QR code.\n");
        return;
    }

    int size = qrcodegen_getSize(qrcode);
    int lo = -1, hi = size + 1; /* 1-module quiet zone. */

    for (int y = lo; y < hi; y += 2) {
        for (int x = lo; x < hi; x++) {
            int top = (x >= 0 && x < size && y >= 0 && y < size &&
                       qrcodegen_getModule(qrcode, x, y));
            int bot = (x >= 0 && x < size && y+1 >= 0 && y+1 < size &&
                       qrcodegen_getModule(qrcode, x, y+1));
            if (top && bot)       printf("\xe2\x96\x88"); /* full block */
            else if (top && !bot) printf("\xe2\x96\x80"); /* upper half */
            else if (!top && bot) printf("\xe2\x96\x84"); /* lower half */
            else                  printf(" ");
        }
        printf("\n");
    }
}

/* Convert hex string to raw bytes. Returns number of bytes written. */
static int hex_to_bytes(const char *hex, unsigned char *out, int max) {
    int len = 0;
    while (*hex && *(hex+1) && len < max) {
        unsigned int byte;
        if (sscanf(hex, "%2x", &byte) != 1) break;
        out[len++] = (unsigned char)byte;
        hex += 2;
    }
    return len;
}

/* Convert raw bytes to hex string. Returns static buffer. */
static const char *bytes_to_hex(const unsigned char *data, int len) {
    static char hex[128];
    for (int i = 0; i < len && i < 63; i++) {
        sprintf(hex + i*2, "%02x", data[i]);
    }
    hex[len*2] = '\0';
    return hex;
}

/* Setup TOTP: check for existing secret, generate if needed, display QR.
 * The db_path is the SQLite database file path.
 * Returns the secret length in bytes, or 0 on error/weak-security. */
static int totp_setup(const char *db_path) {
    if (WeakSecurity) return 0;

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database for TOTP setup.\n");
        return 0;
    }
    /* Ensure KV table exists. */
    sqlite3_exec(db, TB_CREATE_KV_STORE, 0, 0, NULL);

    /* Check for existing secret. */
    sds existing = kvGet(db, "totp_secret");
    if (existing) {
        sdsfree(existing);
        /* Load stored timeout if present. */
        sds timeout_str = kvGet(db, "otp_timeout");
        if (timeout_str) {
            int t = atoi(timeout_str);
            if (t >= 30 && t <= 28800) OtpTimeout = t;
            sdsfree(timeout_str);
        }
        sqlite3_close(db);
        return 1; /* Secret already exists. */
    }

    /* Generate 20 random bytes. */
    unsigned char secret[20];
    FILE *f = fopen("/dev/urandom", "r");
    if (!f || fread(secret, 1, 20, f) != 20) {
        fprintf(stderr, "Failed to read /dev/urandom, aborting: "
                        "can't proceed without TOTP secret generation.\n");
        exit(1);
    }
    fclose(f);

    /* Store as hex in KV. */
    kvSet(db, "totp_secret", bytes_to_hex(secret, 20), 0);
    sqlite3_close(db);

    /* Build otpauth URI and display QR code. */
    const char *b32 = base32_encode(secret, 20);
    char uri[256];
    snprintf(uri, sizeof(uri),
             "otpauth://totp/tgterm?secret=%s&issuer=tgterm", b32);

    printf("\n=== TOTP Setup ===\n");
    printf("Scan this QR code with Google Authenticator:\n\n");
    print_qr_ascii(uri);
    printf("\nOr enter this secret manually: %s\n", b32);
    printf("==================\n\n");
    fflush(stdout);

    return 1;
}

/* Check if the given code matches the current TOTP (with +/-1 window). */
static int totp_verify(sqlite3 *db, const char *code_str) {
    sds hex = kvGet(db, "totp_secret");
    if (!hex) return 0;

    unsigned char secret[20];
    int slen = hex_to_bytes(hex, secret, 20);
    sdsfree(hex);
    if (slen != 20) return 0;

    uint64_t now = (uint64_t)time(NULL) / 30;
    uint32_t input_code = (uint32_t)atoi(code_str);

    for (int i = -1; i <= 1; i++) {
        if (totp_code(secret, 20, now + i) == input_code)
            return 1;
    }
    return 0;
}

/* ============================================================================
 * UTF-8 Emoji Parsing
 * ========================================================================= */

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

/* ============================================================================
 * Connection Management
 * ========================================================================= */

/* Disconnect from current terminal session. */
void disconnect(void) {
    Connected = 0;
    ConnectedId[0] = '\0';
    ConnectedPid = 0;
    ConnectedName[0] = '\0';
    ConnectedTitle[0] = '\0';
    TrackedMsgCount = 0;
}

/* ============================================================================
 * Bot Command Handlers
 * ========================================================================= */

/* Build the .list response. */
sds build_list_message(void) {
    backend_list();

    sds msg = sdsempty();
    if (TermCount == 0) {
        msg = sdscat(msg, "No terminal sessions found.");
        return msg;
    }

    msg = sdscat(msg, "Terminal windows:\n");
    for (int i = 0; i < TermCount; i++) {
        TermInfo *t = &TermList[i];
        char line[512];
        if (t->title[0]) {
            snprintf(line, sizeof(line), ".%d %s - %s\n",
                     i + 1, t->name, t->title);
        } else {
            snprintf(line, sizeof(line), ".%d %s\n",
                     i + 1, t->name);
        }
        msg = sdscat(msg, line);
    }
    return msg;
}

sds build_help_message(void) {
    return sdsnew(
        "Commands:\n"
        ".list - Show terminal windows\n"
        ".1 .2 ... - Connect to window\n"
        ".help - This help\n\n"
        "Once connected, text is sent as keystrokes.\n"
        "Newline is auto-added; end with `\xf0\x9f\x92\x9c` to suppress it.\n\n"
        "Modifiers (tap to copy, then paste + key):\n"
        "`\xe2\x9d\xa4\xef\xb8\x8f` Ctrl  `\xf0\x9f\x92\x99` Alt  "
        "`\xf0\x9f\x92\x9a` Cmd  `\xf0\x9f\x92\x9b` ESC  "
        "`\xf0\x9f\xa7\xa1` Enter\n\n"
        "Escape sequences: \\n=Enter \\t=Tab\n\n"
        "`.otptimeout <seconds>` - Set OTP timeout (30-28800)"
    );
}

/* ============================================================================
 * Telegram Bot Callbacks
 * ========================================================================= */

#define MAX_MSG_LEN 4085  /* 4096 - strlen("<pre></pre>") */
#define OWNER_KEY "owner_id"
#define REFRESH_BTN "\xf0\x9f\x94\x84 Refresh"
#define REFRESH_DATA "refresh"

/* Get visible lines from TELETERM_VISIBLE_LINES env var, defaulting to 40. */
static int get_visible_lines(void) {
    const char *env = getenv("TELETERM_VISIBLE_LINES");
    if (env) {
        int v = atoi(env);
        if (v > 0) return v;
    }
    return 40;
}

/* Check if multi-message splitting is enabled (default: off = truncate). */
static int get_split_messages(void) {
    const char *env = getenv("TELETERM_SPLIT_MESSAGES");
    if (env && (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0))
        return 1;
    return 0;
}

/* Send a plain HTML message (no inline keyboard). Returns message_id or 0. */
static int64_t send_html_message(int64_t target, sds text) {
    char *options[6];
    options[0] = "chat_id";
    options[1] = sdsfromlonglong(target);
    options[2] = "text";
    options[3] = text;
    options[4] = "parse_mode";
    options[5] = "HTML";
    int res;
    sds body = makeGETBotRequest("sendMessage", &res, options, 3);

    int64_t mid = 0;
    if (res && body) {
        cJSON *json = cJSON_Parse(body);
        cJSON *m = cJSON_Select(json, ".result.message_id:n");
        if (m) mid = (int64_t)m->valuedouble;
        cJSON_Delete(json);
    }

    sdsfree(body);
    sdsfree(options[1]);
    return mid;
}

/* Delete all tracked terminal messages, then reset tracking. */
static void delete_terminal_messages(int64_t chat_id) {
    for (int i = TrackedMsgCount - 1; i >= 0; i--) {
        char *options[4];
        options[0] = "chat_id";
        options[1] = sdsfromlonglong(chat_id);
        options[2] = "message_id";
        options[3] = sdsfromlonglong(TrackedMsgIds[i]);
        int res;
        sds body = makeGETBotRequest("deleteMessage", &res, options, 2);
        sdsfree(body);
        sdsfree(options[1]);
        sdsfree(options[3]);
    }
    TrackedMsgCount = 0;
}

/* Escape text for Telegram HTML parse mode. */
static sds html_escape(const char *text) {
    sds out = sdsempty();
    for (const char *p = text; *p; p++) {
        switch (*p) {
            case '<': out = sdscat(out, "&lt;"); break;
            case '>': out = sdscat(out, "&gt;"); break;
            case '&': out = sdscat(out, "&amp;"); break;
            default:  out = sdscatlen(out, p, 1); break;
        }
    }
    return out;
}

/* Get the last N lines from text. Returns pointer into the string. */
static const char *last_n_lines(const char *text, int n) {
    const char *end = text + strlen(text);
    const char *p = end;
    int count = 0;
    while (p > text) {
        p--;
        if (*p == '\n') {
            count++;
            if (count >= n) { p++; break; }
        }
    }
    return p;
}

/* Format terminal text into one or more HTML <pre> messages.
 * When TELETERM_SPLIT_MESSAGES is enabled, splits on line boundaries when
 * content exceeds Telegram's 4096 char limit. Otherwise truncates to fit
 * a single message (keeping the tail end of the output).
 * Caller must sdsfree each element and xfree the array. */
static sds *format_terminal_messages(sds raw, int *count) {
    int visible_lines = get_visible_lines();
    const char *tail = last_n_lines(raw, visible_lines);
    sds escaped = html_escape(tail);

    sds *msgs = NULL;
    int n = 0;
    int split = get_split_messages();

    if (!split) {
        /* Truncate mode: keep the tail that fits in one message. */
        if (sdslen(escaped) > MAX_MSG_LEN) {
            /* Find a newline near the cut point to avoid breaking a line. */
            const char *start = escaped + sdslen(escaped) - MAX_MSG_LEN;
            const char *nl = strchr(start, '\n');
            if (nl && (size_t)(nl - escaped) < sdslen(escaped))
                start = nl + 1;
            sds trimmed = sdsnew(start);
            sdsfree(escaped);
            escaped = trimmed;
        }
        msgs = xrealloc(msgs, sizeof(sds) * 1);
        msgs[n++] = sdscatprintf(sdsempty(), "<pre>%s</pre>", escaped);
    } else {
        /* Split mode: break into multiple messages. */
        while (sdslen(escaped) > 0) {
            if (sdslen(escaped) <= MAX_MSG_LEN) {
                msgs = xrealloc(msgs, sizeof(sds) * (n + 1));
                msgs[n++] = sdscatprintf(sdsempty(), "<pre>%s</pre>", escaped);
                break;
            }

            /* Find last newline within MAX_MSG_LEN to split on a line boundary. */
            char *cut = NULL;
            for (size_t i = MAX_MSG_LEN; i > 0; i--) {
                if (escaped[i - 1] == '\n') {
                    cut = escaped + i - 1;
                    break;
                }
            }

            if (!cut) {
                /* No newline found; hard-cut at MAX_MSG_LEN. */
                sds chunk = sdsnewlen(escaped, MAX_MSG_LEN);
                msgs = xrealloc(msgs, sizeof(sds) * (n + 1));
                msgs[n++] = sdscatprintf(sdsempty(), "<pre>%s</pre>", chunk);
                sdsfree(chunk);
                sdsrange(escaped, MAX_MSG_LEN, -1);
            } else {
                size_t chunk_len = cut - escaped;
                sds chunk = sdsnewlen(escaped, chunk_len);
                msgs = xrealloc(msgs, sizeof(sds) * (n + 1));
                msgs[n++] = sdscatprintf(sdsempty(), "<pre>%s</pre>", chunk);
                sdsfree(chunk);
                sdsrange(escaped, chunk_len + 1, -1); /* skip past newline */
            }
        }
    }

    sdsfree(escaped);

    if (n == 0) {
        msgs = xrealloc(msgs, sizeof(sds));
        msgs[0] = sdsnew("<pre></pre>");
        n = 1;
    }

    *count = n;
    return msgs;
}

/* Send terminal text with refresh button (splits into multiple messages if needed).
 * Deletes previously tracked messages first to create a "live terminal view". */
void send_terminal_text(int64_t chat_id) {
    sds raw = backend_capture_text();
    if (!raw) {
        botSendMessage(chat_id, "Could not read terminal text.", 0);
        return;
    }

    delete_terminal_messages(chat_id);

    int count;
    sds *msgs = format_terminal_messages(raw, &count);
    sdsfree(raw);

    for (int i = 0; i < count - 1; i++) {
        int64_t mid = send_html_message(chat_id, msgs[i]);
        if (mid && TrackedMsgCount < MAX_TRACKED_MSGS)
            TrackedMsgIds[TrackedMsgCount++] = mid;
        sdsfree(msgs[i]);
    }

    int64_t last_mid = 0;
    botSendMessageWithKeyboard(chat_id, msgs[count - 1], "HTML",
                               REFRESH_BTN, REFRESH_DATA, &last_mid);
    if (last_mid && TrackedMsgCount < MAX_TRACKED_MSGS)
        TrackedMsgIds[TrackedMsgCount++] = last_mid;

    sdsfree(msgs[count - 1]);
    xfree(msgs);
}

void handle_request(sqlite3 *db, BotRequest *br) {
    pthread_mutex_lock(&RequestLock);

    /* Check owner. First user to message becomes owner. */
    sds owner_str = kvGet(db, OWNER_KEY);
    int64_t owner_id = 0;

    if (owner_str) {
        owner_id = strtoll(owner_str, NULL, 10);
        sdsfree(owner_str);
    }

    if (owner_id == 0) {
        /* Register first user as owner. */
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)br->from);
        kvSet(db, OWNER_KEY, buf, 0);
        owner_id = br->from;
        printf("Registered owner: %lld (%s)\n", (long long)owner_id, br->from_username);
    }

    if (br->from != owner_id) {
        printf("Ignoring message from non-owner %lld\n", (long long)br->from);
        goto done;
    }

    /* TOTP authentication check (applies to both messages and callbacks). */
    if (!WeakSecurity) {
        if (!Authenticated || time(NULL) - LastActivity > OtpTimeout) {
            Authenticated = 0;
            if (br->is_callback) {
                botAnswerCallbackQuery(br->callback_id);
                goto done;
            }
            char *req = br->request;
            /* Check if message is a 6-digit OTP code. */
            int is_otp = (strlen(req) == 6);
            for (int i = 0; is_otp && i < 6; i++) {
                if (!isdigit((unsigned char)req[i])) is_otp = 0;
            }
            if (is_otp && totp_verify(db, req)) {
                Authenticated = 1;
                LastActivity = time(NULL);
                botSendMessage(br->target, "Authenticated.", 0);
            } else {
                botSendMessage(br->target, "Enter OTP code.", 0);
            }
            goto done;
        }
        LastActivity = time(NULL);
    }

    /* Handle callback query (button press). */
    if (br->is_callback) {
        botAnswerCallbackQuery(br->callback_id);
        if (strcmp(br->callback_data, REFRESH_DATA) == 0 && Connected) {
            send_terminal_text(br->target);
        }
        goto done;
    }

    char *req = br->request;

    /* Handle .list command. */
    if (strcasecmp(req, ".list") == 0) {
        disconnect();
        sds msg = build_list_message();
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Handle .help command. */
    if (strcasecmp(req, ".help") == 0) {
        sds msg = build_help_message();
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Handle .otptimeout command. */
    if (strncasecmp(req, ".otptimeout", 11) == 0) {
        char *arg = req + 11;
        while (*arg == ' ') arg++;
        int secs = atoi(arg);
        if (secs < 30) secs = 30;
        if (secs > 28800) secs = 28800;
        OtpTimeout = secs;
        char buf[64];
        snprintf(buf, sizeof(buf), "%d", secs);
        kvSet(db, "otp_timeout", buf, 0);
        sds msg = sdscatprintf(sdsempty(), "OTP timeout set to %d seconds.", secs);
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Handle .N to connect to terminal session N. */
    if (req[0] == '.' && isdigit(req[1])) {
        int n = atoi(req + 1);
        backend_list();

        if (n < 1 || n > TermCount) {
            botSendMessage(br->target, "Invalid window number.", 0);
            goto done;
        }

        /* Store connection info directly. */
        TermInfo *t = &TermList[n - 1];
        Connected = 1;
        strncpy(ConnectedId, t->id, sizeof(ConnectedId) - 1);
        ConnectedId[sizeof(ConnectedId) - 1] = '\0';
        ConnectedPid = t->pid;
        strncpy(ConnectedName, t->name, sizeof(ConnectedName) - 1);
        ConnectedName[sizeof(ConnectedName) - 1] = '\0';
        strncpy(ConnectedTitle, t->title, sizeof(ConnectedTitle) - 1);
        ConnectedTitle[sizeof(ConnectedTitle) - 1] = '\0';

        sds msg = sdsnew("Connected to ");
        msg = sdscat(msg, ConnectedName);
        if (ConnectedTitle[0]) {
            msg = sdscat(msg, " - ");
            msg = sdscat(msg, ConnectedTitle);
        }
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);

        /* Send terminal text. */
        send_terminal_text(br->target);
        goto done;
    }

    /* Not a command - send as keystrokes if connected. */
    if (!Connected) {
        sds msg = build_list_message();
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Check terminal session still exists. */
    if (!backend_connected()) {
        disconnect();
        sds msg = sdsnew("Window closed.\n\n");
        sds list = build_list_message();
        msg = sdscatsds(msg, list);
        sdsfree(list);
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Send keystrokes. */
    backend_send_keys(req);

    /* Wait a bit for the terminal to react, then re-check the session
     * (keystrokes may switch panes/tabs, changing the active ID). */
    sleep(2);
    backend_connected();
    send_terminal_text(br->target);

done:
    pthread_mutex_unlock(&RequestLock);
}

void cron_callback(sqlite3 *db) {
    UNUSED(db);
}

/* ============================================================================
 * Main
 * ========================================================================= */

int main(int argc, char **argv) {
    /* Parse our custom flags. */
    const char *dbfile = "./mybot.sqlite";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dangerously-attach-to-any-window") == 0) {
            DangerMode = 1;
            printf("DANGER MODE: All windows will be visible.\n");
        } else if (strcmp(argv[i], "--use-weak-security") == 0) {
            WeakSecurity = 1;
            printf("WARNING: OTP authentication disabled.\n");
        } else if (strcmp(argv[i], "--dbfile") == 0 && i+1 < argc) {
            dbfile = argv[i+1];
        }
    }

    /* TOTP setup: check/generate secret before starting the bot. */
    totp_setup(dbfile);

    /* Triggers: respond to all private messages. */
    static char *triggers[] = { "*", NULL };

    startBot(TB_CREATE_KV_STORE, argc, argv, TB_FLAGS_IGNORE_BAD_ARG,
             handle_request, cron_callback, triggers);
    return 0;
}
