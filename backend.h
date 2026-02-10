#ifndef BACKEND_H
#define BACKEND_H

#include <sys/types.h>
#include "sds.h"

/* Terminal session info (generic across backends). */
typedef struct {
    char id[128];         /* macOS: window_id as string, tmux: pane_id (%0, %1, ...) */
    pid_t pid;            /* process PID */
    char name[128];       /* macOS: app name, tmux: session:window.pane */
    char title[256];      /* window/pane title or current command */
} TermInfo;

/* ============================================================================
 * Backend interface — implemented by backend_macos.c or backend_tmux.c
 * ========================================================================= */

/* List available terminal sessions. Fills TermList/TermCount. Returns count. */
int backend_list(void);

/* Free the terminal list. */
void backend_free_list(void);

/* Check if current connection is still alive. Returns 1 if yes. */
int backend_connected(void);

/* Capture visible text from connected terminal. Returns sds string or NULL. */
sds backend_capture_text(void);

/* Send keystrokes to connected terminal.
 * text: raw input with emoji modifiers.
 * Returns 0 on success, -1 on error. */
int backend_send_keys(const char *text);

/* ============================================================================
 * Shared state — defined in bot_common.c, used by backends
 * ========================================================================= */

extern TermInfo *TermList;
extern int TermCount;

extern int Connected;
extern char ConnectedId[128];   /* backend-specific ID (window_id or pane_id) */
extern pid_t ConnectedPid;
extern char ConnectedName[128];
extern char ConnectedTitle[256];

extern int DangerMode;

/* ============================================================================
 * Shared emoji parsing — defined in bot_common.c, used by backends
 * ========================================================================= */

int match_red_heart(const unsigned char *p, size_t remaining);
int match_colored_heart(const unsigned char *p, size_t remaining, char *heart);
int match_orange_heart(const unsigned char *p, size_t remaining);
int match_purple_heart(const unsigned char *p, size_t remaining);
int ends_with_purple_heart(const char *text);

#endif
