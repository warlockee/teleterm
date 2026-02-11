/*
 * backend_macos.c - macOS backend for teleterm
 *
 * Implements the 5 backend functions (backend.h) using the macOS
 * CoreGraphics / Accessibility APIs to list terminal windows,
 * capture their text, and inject keystrokes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <libproc.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>

#include "backend.h"
#include "botlib.h"

/* Private API to get CGWindowID from AXUIElement. */
extern AXError _AXUIElementGetWindow(AXUIElementRef element, CGWindowID *wid);

/* ============================================================================
 * Virtual keycodes and modifier flags
 * ========================================================================= */

#define kVK_Return    0x24
#define kVK_Tab       0x30
#define kVK_Escape    0x35

#define MOD_CTRL    (1<<0)
#define MOD_ALT     (1<<1)
#define MOD_CMD     (1<<2)

/* ============================================================================
 * Known terminal applications
 * ========================================================================= */

static const char *TerminalApps[] = {
    "Terminal", "iTerm2", "iTerm", "Ghostty", "kitty", "Alacritty",
    "Hyper", "Warp", "WezTerm", "Tabby", NULL
};

static int is_terminal_app(const char *name) {
    for (int i = 0; TerminalApps[i]; i++) {
        if (strcasestr(name, TerminalApps[i])) return 1;
    }
    return 0;
}

/* ============================================================================
 * backend_free_list  (adapted from free_window_list)
 * ========================================================================= */

void backend_free_list(void) {
    if (TermList) {
        free(TermList);
        TermList = NULL;
    }
    TermCount = 0;
}

/* ============================================================================
 * Process tree helpers — detect foreground command via libproc
 * ========================================================================= */

/* Walk to the deepest descendant of pid, return its process name. */
static int get_foreground_name(pid_t pid, char *out, size_t outsize) {
    out[0] = '\0';
    pid_t cur = pid;

    for (int depth = 0; depth < 20; depth++) {
        pid_t kids[128];
        int got = proc_listchildpids(cur, kids, sizeof(kids));
        int n = got;
        if (n <= 0) break;
        cur = kids[n - 1];  /* last child = most recently forked */
    }

    return proc_name(cur, out, (uint32_t)outsize) > 0 ? 1 : 0;
}

/* Detect foreground command for a terminal window.
 * Walks child process trees and tries to match leaf names to window title. */
static void detect_window_command(pid_t app_pid, const char *title,
                                   char *out, size_t outsize) {
    out[0] = '\0';

    pid_t children[128];
    int got = proc_listchildpids(app_pid, children, sizeof(children));
    int nch = got;
    if (nch <= 0) return;

    char first_leaf[128] = "";

    for (int i = 0; i < nch; i++) {
        char leaf[128];
        if (!get_foreground_name(children[i], leaf, sizeof(leaf))) continue;

        /* If this leaf name appears in the window title, it's our match. */
        if (title[0] && strstr(title, leaf)) {
            strncpy(out, leaf, outsize - 1);
            out[outsize - 1] = '\0';
            return;
        }

        if (!first_leaf[0])
            strncpy(first_leaf, leaf, sizeof(first_leaf) - 1);
    }

    /* Fallback: use first leaf found. */
    if (first_leaf[0]) {
        strncpy(out, first_leaf, outsize - 1);
        out[outsize - 1] = '\0';
    }
}

/* ============================================================================
 * Text prompt detection — check if terminal shows an input prompt
 * ========================================================================= */

/* Forward declarations for AX text capture. */
static sds ax_read_value(AXUIElementRef element);
static sds ax_get_text_content(AXUIElementRef element);

/* Trim trailing whitespace including non-breaking spaces (iTerm2 pads
 * terminal lines with U+00A0 = 0xC2 0xA0 to the full column width). */
static size_t trim_trailing(const char *s, size_t len) {
    while (len > 0) {
        unsigned char c = (unsigned char)s[len - 1];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { len--; continue; }
        /* UTF-8 non-breaking space: C2 A0 */
        if (c == 0xA0 && len >= 2 && (unsigned char)s[len - 2] == 0xC2)
            { len -= 2; continue; }
        break;
    }
    return len;
}

/* Skip leading whitespace including NBSP. Returns new start and updates len. */
static const unsigned char *skip_leading(const unsigned char *s, size_t *len) {
    while (*len > 0) {
        if (*s == ' ') { s++; (*len)--; continue; }
        if (*len >= 2 && s[0] == 0xC2 && s[1] == 0xA0) { s += 2; *len -= 2; continue; }
        break;
    }
    return s;
}

/* Check if terminal text has an input prompt in the last few non-empty lines.
 * Catches shell prompts ($, #, %, >, ❯, ➜) at line end OR start.
 * Needs a generous budget because TUIs (Claude Code) and tmux add status
 * bars, separators, etc. below the actual prompt line. */
static int text_shows_prompt(const char *text) {
    size_t len = strlen(text);
    if (len == 0) return 0;

    int lines_checked = 0;
    size_t pos = trim_trailing(text, len);

    while (pos > 0 && lines_checked < 10) {
        size_t end = pos;
        while (pos > 0 && text[pos-1] != '\n') pos--;

        const char *line = text + pos;
        size_t line_len = trim_trailing(line, end - pos);

        if (line_len == 0) {
            if (pos > 0) pos--;
            continue;
        }

        /* Check prompt char at END of line. */
        char last = line[line_len - 1];
        if (last == '$' || last == '#' || last == '%' || last == '>') return 1;

        /* ❯ (E2 9D AF) or ➜ (E2 9E 9C) at end. */
        if (line_len >= 3) {
            const unsigned char *p = (const unsigned char *)line + line_len - 3;
            if ((p[0] == 0xE2 && p[1] == 0x9D && p[2] == 0xAF) ||
                (p[0] == 0xE2 && p[1] == 0x9E && p[2] == 0x9C)) return 1;
        }

        /* Check prompt char at START of line (skip leading whitespace).
         * Catches TUI prompts like Claude Code's "> " or "❯ ". */
        const unsigned char *ls = (const unsigned char *)line;
        size_t lr = line_len;
        ls = skip_leading(ls, &lr);
        if (lr >= 1) {
            char first = (char)ls[0];
            if ((first == '>' || first == '$' || first == '#' || first == '%') &&
                (lr == 1 || ls[1] == ' ' ||
                 (lr >= 3 && ls[1] == 0xC2 && ls[2] == 0xA0))) return 1;
        }
        /* ❯ or ➜ at start. */
        if (lr >= 3) {
            if ((ls[0] == 0xE2 && ls[1] == 0x9D && ls[2] == 0xAF) ||
                (ls[0] == 0xE2 && ls[1] == 0x9E && ls[2] == 0x9C)) {
                if (lr == 3 || ls[3] == ' ' ||
                    (lr >= 5 && ls[3] == 0xC2 && ls[4] == 0xA0)) return 1;
            }
        }

        lines_checked++;
        if (pos > 0) pos--;
    }

    return 0;
}

/* ============================================================================
 * backend_list  (adapted from refresh_window_list)
 * ========================================================================= */

int backend_list(void) {
    backend_free_list();

    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID
    );
    if (!list) return 0;

    CFIndex count = CFArrayGetCount(list);

    /* Allocate maximum possible size. */
    TermList = malloc(count * sizeof(TermInfo));
    if (!TermList) {
        CFRelease(list);
        return 0;
    }

    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = CFArrayGetValueAtIndex(list, i);

        /* Get owner name. */
        CFStringRef owner_ref = CFDictionaryGetValue(info, kCGWindowOwnerName);
        if (!owner_ref) continue;

        char owner[128];
        if (!CFStringGetCString(owner_ref, owner, sizeof(owner), kCFStringEncodingUTF8))
            continue;

        /* Filter to terminals only unless in danger mode. */
        if (!DangerMode && !is_terminal_app(owner)) continue;

        /* Get window ID and PID. */
        CFNumberRef wid_ref = CFDictionaryGetValue(info, kCGWindowNumber);
        CFNumberRef pid_ref = CFDictionaryGetValue(info, kCGWindowOwnerPID);
        if (!wid_ref || !pid_ref) continue;

        CGWindowID wid;
        pid_t pid;
        CFNumberGetValue(wid_ref, kCGWindowIDCFNumberType, &wid);
        CFNumberGetValue(pid_ref, kCFNumberIntType, &pid);

        /* Only layer 0. */
        CFNumberRef layer_ref = CFDictionaryGetValue(info, kCGWindowLayer);
        int layer = 0;
        if (layer_ref) CFNumberGetValue(layer_ref, kCFNumberIntType, &layer);
        if (layer != 0) continue;

        /* Must have reasonable size. */
        CFDictionaryRef bounds_dict = CFDictionaryGetValue(info, kCGWindowBounds);
        if (!bounds_dict) continue;

        CGRect bounds;
        CGRectMakeWithDictionaryRepresentation(bounds_dict, &bounds);
        if (bounds.size.width <= 50 || bounds.size.height <= 50) continue;

        /* Get window title. */
        CFStringRef title_ref = CFDictionaryGetValue(info, kCGWindowName);
        char title[256] = "";
        if (title_ref)
            CFStringGetCString(title_ref, title, sizeof(title), kCFStringEncodingUTF8);

        /* Add to list — convert window_id to string for TermInfo.id,
         * use owner for TermInfo.name. */
        TermInfo *t = &TermList[TermCount++];
        snprintf(t->id, sizeof(t->id), "%u", (unsigned)wid);
        t->pid = pid;
        strncpy(t->name, owner, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
        strncpy(t->title, title, sizeof(t->title) - 1);
        t->title[sizeof(t->title) - 1] = '\0';
        t->command[0] = '\0';
    }

    CFRelease(list);

    /* Fill in titles, detect command status via AX text + process tree. */
    for (int i = 0; i < TermCount; i++) {
        int prompt_found = 0;

        AXUIElementRef app = AXUIElementCreateApplication(TermList[i].pid);
        if (!app) goto do_proc;
        CFArrayRef axwins = NULL;
        AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&axwins);
        if (axwins) {
            CGWindowID target_wid = (CGWindowID)atoi(TermList[i].id);
            CFIndex n = CFArrayGetCount(axwins);
            for (CFIndex j = 0; j < n; j++) {
                AXUIElementRef win = (AXUIElementRef)CFArrayGetValueAtIndex(axwins, j);
                CGWindowID wid = 0;
                if (_AXUIElementGetWindow(win, &wid) == kAXErrorSuccess &&
                    wid == target_wid) {
                    /* Get title if missing. */
                    if (!TermList[i].title[0]) {
                        CFStringRef title = NULL;
                        AXUIElementCopyAttributeValue(win, kAXTitleAttribute, (CFTypeRef *)&title);
                        if (title) {
                            CFStringGetCString(title, TermList[i].title,
                                sizeof(TermList[i].title), kCFStringEncodingUTF8);
                            CFRelease(title);
                        }
                    }
                    /* Check terminal text for input prompt. */
                    sds text = ax_get_text_content(win);
                    if (text) {
                        prompt_found = text_shows_prompt(text);
                        sdsfree(text);
                    }
                    break;
                }
            }
            CFRelease(axwins);
        }
        CFRelease(app);

do_proc:
        if (prompt_found) {
            /* Text shows a prompt → terminal is idle/waiting for input. */
            strncpy(TermList[i].command, "shell",
                sizeof(TermList[i].command) - 1);
            TermList[i].command[sizeof(TermList[i].command) - 1] = '\0';
        } else {
            /* No prompt visible → use process tree for command name. */
            detect_window_command(TermList[i].pid, TermList[i].title,
                TermList[i].command, sizeof(TermList[i].command));
        }
    }

    return TermCount;
}

/* ============================================================================
 * backend_connected  (adapted from connected_window_exists)
 * ========================================================================= */

int backend_connected(void) {
    if (!Connected) return 0;

    CGWindowID connected_wid = (CGWindowID)atoi(ConnectedId);

    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID
    );
    if (!list) return 0;

    int found = 0;
    CGWindowID fallback_wid = 0;
    CFIndex count = CFArrayGetCount(list);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = CFArrayGetValueAtIndex(list, i);
        CFNumberRef wid_ref = CFDictionaryGetValue(info, kCGWindowNumber);
        CFNumberRef pid_ref = CFDictionaryGetValue(info, kCGWindowOwnerPID);
        if (!wid_ref || !pid_ref) continue;

        CGWindowID wid;
        pid_t pid;
        CFNumberGetValue(wid_ref, kCGWindowIDCFNumberType, &wid);
        CFNumberGetValue(pid_ref, kCFNumberIntType, &pid);

        if (wid == connected_wid) {
            found = 1;
            break;
        }

        /* Track a fallback: another on-screen window from the same PID. */
        if (pid == ConnectedPid && !fallback_wid) {
            CFNumberRef layer_ref = CFDictionaryGetValue(info, kCGWindowLayer);
            int layer = 0;
            if (layer_ref) CFNumberGetValue(layer_ref, kCFNumberIntType, &layer);
            if (layer == 0) fallback_wid = wid;
        }
    }

    /* Window gone but same app has another window — likely a tab switch. */
    if (!found && fallback_wid) {
        snprintf(ConnectedId, sizeof(ConnectedId), "%u", (unsigned)fallback_wid);
        found = 1;
    }

    CFRelease(list);
    return found;
}

/* ============================================================================
 * Accessibility helpers for text capture  (static)
 * ========================================================================= */

/* Read AXValue text from an element, stripping embedded null bytes
 * that iTerm2 uses to pad empty terminal cells. */
static sds ax_read_value(AXUIElementRef element) {
    CFStringRef value = NULL;
    AXUIElementCopyAttributeValue(element, kAXValueAttribute, (CFTypeRef *)&value);
    if (!value) return NULL;

    CFIndex len = CFStringGetLength(value);
    CFIndex bufsize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    char *buf = malloc(bufsize);
    if (!buf) { CFRelease(value); return NULL; }

    /* Get the actual UTF-8 byte count. */
    CFIndex used = 0;
    CFStringGetBytes(value, CFRangeMake(0, len), kCFStringEncodingUTF8,
                     '?', false, (UInt8 *)buf, bufsize - 1, &used);
    CFRelease(value);

    /* Strip null bytes. */
    sds result = sdsempty();
    for (CFIndex i = 0; i < used; i++) {
        if (buf[i] != '\0')
            result = sdscatlen(result, buf + i, 1);
    }
    free(buf);
    return sdslen(result) > 0 ? result : (sdsfree(result), (sds)NULL);
}

/* Recursively search AX hierarchy for a text area and return its text. */
static sds ax_get_text_content(AXUIElementRef element) {
    CFStringRef role = NULL;
    AXUIElementCopyAttributeValue(element, kAXRoleAttribute, (CFTypeRef *)&role);
    if (role) {
        if (CFStringCompare(role, CFSTR("AXTextArea"), 0) == kCFCompareEqualTo ||
            CFStringCompare(role, CFSTR("AXStaticText"), 0) == kCFCompareEqualTo ||
            CFStringCompare(role, CFSTR("AXWebArea"), 0) == kCFCompareEqualTo) {
            CFRelease(role);
            return ax_read_value(element);
        }
        CFRelease(role);
    }

    CFArrayRef children = NULL;
    AXUIElementCopyAttributeValue(element, kAXChildrenAttribute, (CFTypeRef *)&children);
    if (children) {
        CFIndex count = CFArrayGetCount(children);
        for (CFIndex i = 0; i < count; i++) {
            AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
            sds text = ax_get_text_content(child);
            if (text) {
                CFRelease(children);
                return text;
            }
        }
        CFRelease(children);
    }
    return NULL;
}

/* ============================================================================
 * backend_capture_text  (adapted from capture_terminal_text)
 * ========================================================================= */

sds backend_capture_text(void) {
    if (!Connected) return NULL;

    CGWindowID connected_wid = (CGWindowID)atoi(ConnectedId);

    AXUIElementRef app = AXUIElementCreateApplication(ConnectedPid);
    if (!app) return NULL;

    sds text = NULL;
    CFArrayRef windows = NULL;
    AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&windows);
    if (!windows) { CFRelease(app); return NULL; }

    CFIndex count = CFArrayGetCount(windows);
    for (CFIndex i = 0; i < count; i++) {
        AXUIElementRef win = (AXUIElementRef)CFArrayGetValueAtIndex(windows, i);
        CGWindowID wid = 0;
        if (_AXUIElementGetWindow(win, &wid) == kAXErrorSuccess && wid == connected_wid) {
            text = ax_get_text_content(win);
            break;
        }
    }

    CFRelease(windows);
    CFRelease(app);
    return text;
}

/* ============================================================================
 * Keystroke helpers  (static)
 * ========================================================================= */

/* Bring app to front. */
static int bring_to_front(pid_t pid) {
    ProcessSerialNumber psn;
    if (GetProcessForPID(pid, &psn) != noErr) return -1;
    if (SetFrontProcessWithOptions(&psn, kSetFrontProcessFrontWindowOnly) != noErr) return -1;
    usleep(100000);
    return 0;
}

/* Raise the specific window by matching CGWindowID via Accessibility API. */
static int raise_window_by_id(pid_t pid, CGWindowID target_wid) {
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app) return -1;

    CFArrayRef windows = NULL;
    AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&windows);
    CFRelease(app);

    if (!windows) return -1;

    int found = 0;
    CFIndex count = CFArrayGetCount(windows);
    for (CFIndex i = 0; i < count; i++) {
        AXUIElementRef win = (AXUIElementRef)CFArrayGetValueAtIndex(windows, i);

        CGWindowID wid = 0;
        if (_AXUIElementGetWindow(win, &wid) == kAXErrorSuccess) {
            if (wid == target_wid) {
                AXUIElementPerformAction(win, kAXRaiseAction);
                found = 1;
                break;
            }
        }
    }

    CFRelease(windows);

    /* Also bring the app to front. */
    bring_to_front(pid);
    return found ? 0 : -1;
}

/* Map ASCII character to macOS virtual keycode (US keyboard layout). */
static CGKeyCode keycode_for_char(char c) {
    /* Letters a-z (same codes for upper/lowercase). */
    static const CGKeyCode letter_map[26] = {
        0x00,0x0B,0x08,0x02,0x0E,0x03,0x05,0x04,0x22,0x26, /* a-j */
        0x28,0x25,0x2E,0x2D,0x1F,0x23,0x0C,0x0F,0x01,0x11, /* k-t */
        0x20,0x09,0x0D,0x07,0x10,0x06                       /* u-z */
    };
    /* Digits 0-9. */
    static const CGKeyCode digit_map[10] = {
        0x1D,0x12,0x13,0x14,0x15,0x17,0x16,0x1A,0x1C,0x19  /* 0-9 */
    };
    /* Punctuation / symbols. */
    if (c >= 'a' && c <= 'z') return letter_map[c - 'a'];
    if (c >= 'A' && c <= 'Z') return letter_map[c - 'A'];
    if (c >= '0' && c <= '9') return digit_map[c - '0'];
    switch (c) {
        case '-':  return 0x1B;  case '=':  return 0x18;
        case '[':  return 0x21;  case ']':  return 0x1E;
        case '\\': return 0x2A;  case ';':  return 0x29;
        case '\'': return 0x27;  case ',':  return 0x2B;
        case '.':  return 0x2F;  case '/':  return 0x2C;
        case '`':  return 0x32;  case ' ':  return 0x31;
    }
    return 0xFFFF; /* Unknown. */
}

static void send_key(pid_t pid, CGKeyCode keycode, UniChar ch, int mods) {
    /* When modifiers are active and we have a character, use the
     * correct virtual keycode so the system sends the right combo. */
    int mapped_keycode = 0;
    if (ch && mods) {
        CGKeyCode mapped = keycode_for_char((char)ch);
        if (mapped != 0xFFFF) {
            keycode = mapped;
            mapped_keycode = 1;
        }
    }

    CGEventRef down = CGEventCreateKeyboardEvent(NULL, keycode, true);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, keycode, false);
    if (!down || !up) {
        if (down) CFRelease(down);
        if (up) CFRelease(up);
        return;
    }

    CGEventFlags flags = 0;
    if (mods & MOD_CTRL) flags |= kCGEventFlagMaskControl;
    if (mods & MOD_ALT)  flags |= kCGEventFlagMaskAlternate;
    if (mods & MOD_CMD)  flags |= kCGEventFlagMaskCommand;

    if (flags) {
        CGEventSetFlags(down, flags);
        CGEventSetFlags(up, flags);
    }

    /* When we have a mapped keycode with modifiers, let the system
     * derive the character from keycode + flags. Otherwise set it. */
    if (ch && !mapped_keycode) {
        CGEventKeyboardSetUnicodeString(down, 1, &ch);
        CGEventKeyboardSetUnicodeString(up, 1, &ch);
    }

    CGEventPostToPid(pid, down);
    usleep(1000);
    CGEventPostToPid(pid, up);
    usleep(5000);

    CFRelease(down);
    CFRelease(up);
}

/* ============================================================================
 * backend_send_keys  (adapted from send_keys)
 * ========================================================================= */

int backend_send_keys(const char *text) {
    if (!Connected) return -1;

    CGWindowID connected_wid = (CGWindowID)atoi(ConnectedId);

    raise_window_by_id(ConnectedPid, connected_wid);

    /* Check if we should suppress trailing newline. */
    int add_newline = !ends_with_purple_heart(text);

    const unsigned char *p = (const unsigned char *)text;
    size_t len = strlen(text);

    /* If ends with purple heart, reduce length to skip it. */
    if (!add_newline && len >= 4) {
        len -= 4;
    }

    int mods = 0;
    int consumed;
    char heart;
    int keycount = 0;       /* Number of actual keystrokes sent. */
    int had_mods = 0;       /* True if any keystroke used modifiers. */
    int last_was_nl = 0;    /* True if last keystroke was Enter. */

    while (len > 0) {
        if ((consumed = match_red_heart(p, len)) > 0) {
            mods |= MOD_CTRL;
            p += consumed; len -= consumed;
            continue;
        }

        if ((consumed = match_orange_heart(p, len)) > 0) {
            send_key(ConnectedPid, kVK_Return, 0, mods);
            if (mods) had_mods = 1;
            keycount++; last_was_nl = 1; mods = 0;
            p += consumed; len -= consumed;
            continue;
        }

        if ((consumed = match_colored_heart(p, len, &heart)) > 0) {
            if (heart == 'Y') {
                send_key(ConnectedPid, kVK_Escape, 0, 0);
                keycount++; had_mods = 1; last_was_nl = 0;
                mods = 0;
            } else if (heart == 'B') {
                mods |= MOD_ALT;
            } else if (heart == 'G') {
                mods |= MOD_CMD;
            }
            p += consumed; len -= consumed;
            continue;
        }

        last_was_nl = 0;
        if (*p == '\\' && len > 1) {
            if (p[1] == 'n') {
                send_key(ConnectedPid, kVK_Return, 0, mods);
                if (mods) had_mods = 1;
                keycount++; last_was_nl = 1; mods = 0;
                p += 2; len -= 2;
                continue;
            } else if (p[1] == 't') {
                send_key(ConnectedPid, kVK_Tab, 0, mods);
                if (mods) had_mods = 1;
                keycount++; mods = 0; p += 2; len -= 2;
                continue;
            } else if (p[1] == '\\') {
                send_key(ConnectedPid, 0, '\\', mods);
                if (mods) had_mods = 1;
                keycount++; mods = 0; p += 2; len -= 2;
                continue;
            }
        }

        send_key(ConnectedPid, 0, (UniChar)*p, mods);
        if (mods) had_mods = 1;
        keycount++; mods = 0;
        p++; len--;
    }

    /* Add newline unless:
     * - Suppressed by purple heart
     * - Single modified keystroke (like Ctrl+C) or bare ESC
     * - Last explicit keystroke was already a newline */
    if (add_newline && !(keycount == 1 && had_mods) && !last_was_nl) {
        usleep(50000);
        send_key(ConnectedPid, kVK_Return, 0, 0);
    }

    return 0;
}
