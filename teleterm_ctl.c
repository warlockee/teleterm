/*
 * teleterm_ctl.c - CLI tool for teleterm terminal operations
 *
 * Provides a command-line interface to backend operations.
 * Each invocation is a separate process with its own global state,
 * avoiding conflicts with the main teleterm bot.
 *
 * Commands:
 *   teleterm-ctl list              - List terminals as JSON
 *   teleterm-ctl capture <id>      - Capture terminal text
 *   teleterm-ctl send <id> <keys>  - Send keystrokes
 *   teleterm-ctl status <id>       - Check if terminal is alive
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "sds.h"
#include "cJSON.h"

/* xmalloc/xrealloc/xfree — needed by sds.c, normally in botlib.c. */
void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) { fprintf(stderr, "Out of memory\n"); exit(1); }
    return p;
}
void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p) { fprintf(stderr, "Out of memory\n"); exit(1); }
    return p;
}
void xfree(void *ptr) { free(ptr); }

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
 * Helper: look up a terminal by ID and set connection globals
 * ========================================================================= */

static int connect_to_id(const char *id) {
    backend_list();

    for (int i = 0; i < TermCount; i++) {
        if (strcmp(TermList[i].id, id) == 0) {
            Connected = 1;
            strncpy(ConnectedId, TermList[i].id, sizeof(ConnectedId) - 1);
            ConnectedId[sizeof(ConnectedId) - 1] = '\0';
            ConnectedPid = TermList[i].pid;
            strncpy(ConnectedName, TermList[i].name, sizeof(ConnectedName) - 1);
            ConnectedName[sizeof(ConnectedName) - 1] = '\0';
            strncpy(ConnectedTitle, TermList[i].title, sizeof(ConnectedTitle) - 1);
            ConnectedTitle[sizeof(ConnectedTitle) - 1] = '\0';
            return 0;
        }
    }

    fprintf(stderr, "Terminal '%s' not found.\n", id);
    return -1;
}

/* ============================================================================
 * Commands
 * ========================================================================= */

static int cmd_list(void) {
    backend_list();

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < TermCount; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "index", i + 1);
        cJSON_AddStringToObject(obj, "id", TermList[i].id);
        cJSON_AddNumberToObject(obj, "pid", (double)TermList[i].pid);
        cJSON_AddStringToObject(obj, "name", TermList[i].name);
        cJSON_AddStringToObject(obj, "title", TermList[i].title);
        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_PrintUnformatted(arr);
    printf("%s\n", json);
    free(json);
    cJSON_Delete(arr);

    backend_free_list();
    return 0;
}

static int cmd_capture(const char *id) {
    if (connect_to_id(id) != 0) return 1;

    sds text = backend_capture_text();
    if (text) {
        printf("%s\n", text);
        sdsfree(text);
    } else {
        fprintf(stderr, "Could not capture text from '%s'.\n", id);
        backend_free_list();
        return 1;
    }

    backend_free_list();
    return 0;
}

static int cmd_send(const char *id, const char *keys) {
    if (connect_to_id(id) != 0) return 1;

    int rc = backend_send_keys(keys);
    backend_free_list();

    if (rc != 0) {
        fprintf(stderr, "Failed to send keys to '%s'.\n", id);
        return 1;
    }

    return 0;
}

static int cmd_status(const char *id) {
    if (connect_to_id(id) != 0) {
        printf("dead\n");
        return 1;
    }

    int alive = backend_connected();
    printf("%s\n", alive ? "alive" : "dead");

    backend_free_list();
    return alive ? 0 : 1;
}

/* ============================================================================
 * Usage and main
 * ========================================================================= */

static void usage(void) {
    fprintf(stderr,
        "Usage: teleterm-ctl <command> [args...]\n"
        "\n"
        "Commands:\n"
        "  list              List terminals as JSON\n"
        "  capture <id>      Capture terminal text\n"
        "  send <id> <keys>  Send keystrokes\n"
        "  status <id>       Check if terminal is alive\n"
        "\n"
        "Options:\n"
        "  --danger          Include non-terminal windows (macOS)\n"
    );
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    /* Parse global options. */
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--danger") == 0) {
            DangerMode = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }

    if (argi >= argc) {
        usage();
        return 1;
    }

    const char *cmd = argv[argi];

    if (strcmp(cmd, "list") == 0) {
        return cmd_list();
    } else if (strcmp(cmd, "capture") == 0) {
        if (argi + 1 >= argc) {
            fprintf(stderr, "Usage: teleterm-ctl capture <id>\n");
            return 1;
        }
        return cmd_capture(argv[argi + 1]);
    } else if (strcmp(cmd, "send") == 0) {
        if (argi + 2 >= argc) {
            fprintf(stderr, "Usage: teleterm-ctl send <id> <keys>\n");
            return 1;
        }
        return cmd_send(argv[argi + 1], argv[argi + 2]);
    } else if (strcmp(cmd, "status") == 0) {
        if (argi + 1 >= argc) {
            fprintf(stderr, "Usage: teleterm-ctl status <id>\n");
            return 1;
        }
        return cmd_status(argv[argi + 1]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage();
        return 1;
    }
}
