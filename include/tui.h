/*
 * tui.h - Terminal User Interface for Linux Hotspot Enabler
 *
 * Responsive ncurses-based TUI with dashboard, config, clients,
 * and log screens with keyboard navigation.
 */

#ifndef TUI_H
#define TUI_H

#include <ncurses.h>
#include "hotspot.h"

#define MAX_LOG_LINES  200
#define MAX_LOG_LEN    256

/* ── Color Pairs ─────────────────────────────────────────────────────── */

enum {
    CP_NORMAL = 1,
    CP_HEADER,
    CP_STATUS_OK,
    CP_STATUS_WARN,
    CP_STATUS_ERR,
    CP_STATUS_OFF,
    CP_HIGHLIGHT,
    CP_BORDER,
    CP_INPUT,
    CP_HOTKEY,
    CP_TITLE,
    CP_CLIENT,
    CP_LOG_INFO,
    CP_LOG_WARN,
    CP_LOG_ERR,
    CP_BANNER
};

/* ── Screens ─────────────────────────────────────────────────────────── */

typedef enum {
    SCREEN_DASHBOARD,
    SCREEN_CONFIG,
    SCREEN_CLIENTS,
    SCREEN_LOG,
    SCREEN_COUNT
} TuiScreen;

/* ── Config Editor Fields ────────────────────────────────────────────── */

typedef enum {
    CFG_SSID,
    CFG_PASSWORD,
    CFG_CHANNEL,
    CFG_BAND_INFO,       /* Read-only: auto-detected from client channel */
    CFG_MAX_CLIENTS,
    CFG_HIDDEN,
    CFG_FIELD_COUNT
} ConfigField;

/* ── Log Entry ───────────────────────────────────────────────────────── */

typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_SUCCESS
} LogLevel;

typedef struct {
    char      message[MAX_LOG_LEN];
    LogLevel  level;
    time_t    timestamp;
} LogEntry;

/* ── TUI State ───────────────────────────────────────────────────────── */

typedef struct {
    TuiScreen      current_screen;
    HotspotStatus *hs_status;
    int            term_rows;
    int            term_cols;
    bool           running;
    bool           editing;
    ConfigField    selected_field;
    char           edit_buffer[MAX_SSID_LEN];
    int            edit_cursor;
    LogEntry       logs[MAX_LOG_LINES];
    int            log_count;
    int            log_scroll;
    int            client_scroll;
} TuiState;

/* ── Functions ───────────────────────────────────────────────────────── */

/* Initialize ncurses and the TUI state */
void tui_init(TuiState *tui, HotspotStatus *hs_status);

/* Main event loop — blocks until user quits */
void tui_run(TuiState *tui);

/* Cleanup ncurses */
void tui_cleanup(TuiState *tui);

/* Add a log message */
void tui_log(TuiState *tui, LogLevel level, const char *fmt, ...);

/* Force a redraw (e.g., after terminal resize) */
void tui_redraw(TuiState *tui);

#endif /* TUI_H */
