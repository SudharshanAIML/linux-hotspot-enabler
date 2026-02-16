/*
 * tui.c - Terminal User Interface for Linux Hotspot Enabler
 *
 * Responsive ncurses-based TUI with multiple screens:
 *  - Dashboard: WiFi + Hotspot status at a glance
 *  - Config: Edit SSID, password, channel, band
 *  - Clients: Connected devices list
 *  - Log: Scrollable event/error log
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <locale.h>

#include "tui.h"
#include "hotspot.h"

/* ── Globals for resize handler ──────────────────────────────────────── */

static volatile sig_atomic_t g_resize = 0;

static void handle_resize(int sig)
{
    (void)sig;
    g_resize = 1;
}

/* ── ncurses Init ────────────────────────────────────────────────────── */

void tui_init(TuiState *tui, HotspotStatus *hs_status)
{
    setlocale(LC_ALL, "");

    memset(tui, 0, sizeof(TuiState));
    tui->hs_status     = hs_status;
    tui->current_screen = SCREEN_DASHBOARD;
    tui->running        = true;
    tui->editing        = false;
    tui->selected_field = CFG_SSID;
    tui->log_count      = 0;
    tui->log_scroll     = 0;
    tui->client_scroll  = 0;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);  /* non-blocking input */
    timeout(500);           /* refresh every 500ms */

    /* Setup signal handler for resize */
    struct sigaction sa;
    sa.sa_handler = handle_resize;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);

    /* Init colors */
    if (has_colors()) {
        start_color();
        use_default_colors();

        init_pair(CP_NORMAL,     COLOR_WHITE,   -1);
        init_pair(CP_HEADER,     COLOR_BLACK,   COLOR_CYAN);
        init_pair(CP_STATUS_OK,  COLOR_GREEN,   -1);
        init_pair(CP_STATUS_WARN, COLOR_YELLOW, -1);
        init_pair(CP_STATUS_ERR, COLOR_RED,     -1);
        init_pair(CP_STATUS_OFF, COLOR_WHITE,   -1);
        init_pair(CP_HIGHLIGHT,  COLOR_BLACK,   COLOR_WHITE);
        init_pair(CP_BORDER,     COLOR_CYAN,    -1);
        init_pair(CP_INPUT,      COLOR_WHITE,   COLOR_BLUE);
        init_pair(CP_HOTKEY,     COLOR_YELLOW,  -1);
        init_pair(CP_TITLE,      COLOR_CYAN,    -1);
        init_pair(CP_CLIENT,     COLOR_WHITE,   -1);
        init_pair(CP_LOG_INFO,   COLOR_CYAN,    -1);
        init_pair(CP_LOG_WARN,   COLOR_YELLOW,  -1);
        init_pair(CP_LOG_ERR,    COLOR_RED,     -1);
        init_pair(CP_BANNER,     COLOR_CYAN,    -1);
    }

    getmaxyx(stdscr, tui->term_rows, tui->term_cols);

    tui_log(tui, LOG_INFO, "Linux Hotspot Enabler started.");

    /* Log distro info */
    DistroInfo di = net_detect_distro();
    tui_log(tui, LOG_INFO, "Detected OS: %s", di.name);

    /* Log WiFi info */
    if (hs_status->wifi.name[0]) {
        tui_log(tui, LOG_INFO, "WiFi interface: %s", hs_status->wifi.name);
        if (hs_status->wifi.connected) {
            tui_log(tui, LOG_SUCCESS, "Connected to: %s", hs_status->wifi.ssid);
        }
        if (hs_status->wifi.supports_ap) {
            tui_log(tui, LOG_SUCCESS, "AP/STA concurrency supported!");
        } else {
            tui_log(tui, LOG_WARN, "AP/STA concurrency may not be supported.");
        }
    } else {
        tui_log(tui, LOG_ERROR, "No WiFi interface detected!");
    }
}

void tui_cleanup(TuiState *tui)
{
    tui->running = false;
    endwin();
}

/* ── Logging ─────────────────────────────────────────────────────────── */

void tui_log(TuiState *tui, LogLevel level, const char *fmt, ...)
{
    if (tui->log_count >= MAX_LOG_LINES) {
        /* Shift logs up */
        memmove(&tui->logs[0], &tui->logs[1],
                sizeof(LogEntry) * (MAX_LOG_LINES - 1));
        tui->log_count = MAX_LOG_LINES - 1;
    }

    LogEntry *entry = &tui->logs[tui->log_count];
    entry->level     = level;
    entry->timestamp = time(NULL);

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->message, MAX_LOG_LEN, fmt, args);
    va_end(args);

    tui->log_count++;
}

/* ── Drawing Helpers ─────────────────────────────────────────────────── */

static void draw_hline(int y, int x, int width, chtype ch)
{
    mvhline(y, x, ch, width);
}

static void draw_box_title(int y, int x, int w, int h, const char *title, int cp)
{
    attron(COLOR_PAIR(cp));

    /* Top border */
    mvaddch(y, x, ACS_ULCORNER);
    for (int i = 1; i < w - 1; i++) mvaddch(y, x + i, ACS_HLINE);
    mvaddch(y, x + w - 1, ACS_URCORNER);

    /* Side borders */
    for (int i = 1; i < h - 1; i++) {
        mvaddch(y + i, x, ACS_VLINE);
        mvaddch(y + i, x + w - 1, ACS_VLINE);
    }

    /* Bottom border */
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    for (int i = 1; i < w - 1; i++) mvaddch(y + h - 1, x + i, ACS_HLINE);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);

    /* Title */
    if (title && strlen(title) > 0) {
        int title_x = x + 2;
        mvprintw(y, title_x, " %s ", title);
    }

    attroff(COLOR_PAIR(cp));
}

static void draw_label_value(int y, int x, int label_w, const char *label,
                             const char *value, int value_cp)
{
    attron(COLOR_PAIR(CP_NORMAL) | A_DIM);
    mvprintw(y, x, "%-*s", label_w, label);
    attroff(COLOR_PAIR(CP_NORMAL) | A_DIM);

    attron(COLOR_PAIR(value_cp) | A_BOLD);
    printw(" %s", value);
    attroff(COLOR_PAIR(value_cp) | A_BOLD);
}

static const char *state_str(HotspotState state)
{
    switch (state) {
        case HS_STATE_STOPPED:  return "STOPPED";
        case HS_STATE_STARTING: return "STARTING...";
        case HS_STATE_RUNNING:  return "RUNNING";
        case HS_STATE_ERROR:    return "ERROR";
        case HS_STATE_STOPPING: return "STOPPING...";
        default:                return "UNKNOWN";
    }
}

static int state_color(HotspotState state)
{
    switch (state) {
        case HS_STATE_RUNNING:  return CP_STATUS_OK;
        case HS_STATE_STARTING:
        case HS_STATE_STOPPING: return CP_STATUS_WARN;
        case HS_STATE_ERROR:    return CP_STATUS_ERR;
        default:                return CP_STATUS_OFF;
    }
}

/* ── Draw Header ─────────────────────────────────────────────────────── */

static void draw_header(TuiState *tui)
{
    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvhline(0, 0, ' ', tui->term_cols);

    const char *title = "  LINUX HOTSPOT ENABLER  ";
    mvprintw(0, 0, "%s", title);

    /* Current time on the right */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestr[32];
    strftime(timestr, sizeof(timestr), "%H:%M:%S", tm);
    mvprintw(0, tui->term_cols - (int)strlen(timestr) - 2, "%s  ", timestr);

    attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
}

/* ── Draw Tab Bar ────────────────────────────────────────────────────── */

static void draw_tabs(TuiState *tui)
{
    int y = 1;
    const char *tabs[] = { "F1:Dashboard", "F2:Config", "F3:Clients", "F4:Log" };
    int tab_count = 4;

    mvhline(y, 0, ' ', tui->term_cols);

    int x = 1;
    for (int i = 0; i < tab_count; i++) {
        if (i == (int)tui->current_screen) {
            attron(COLOR_PAIR(CP_HIGHLIGHT) | A_BOLD);
            mvprintw(y, x, " %s ", tabs[i]);
            attroff(COLOR_PAIR(CP_HIGHLIGHT) | A_BOLD);
        } else {
            attron(COLOR_PAIR(CP_HOTKEY));
            mvprintw(y, x, " %s ", tabs[i]);
            attroff(COLOR_PAIR(CP_HOTKEY));
        }
        x += strlen(tabs[i]) + 3;
    }

    attron(COLOR_PAIR(CP_BORDER));
    draw_hline(2, 0, tui->term_cols, ACS_HLINE);
    attroff(COLOR_PAIR(CP_BORDER));
}

/* ── Draw Footer ─────────────────────────────────────────────────────── */

static void draw_footer(TuiState *tui)
{
    int y = tui->term_rows - 1;
    attron(COLOR_PAIR(CP_HEADER));
    mvhline(y, 0, ' ', tui->term_cols);

    const char *hint;
    if (tui->current_screen == SCREEN_CONFIG && tui->editing) {
        hint = " [Enter] Save  [Esc] Cancel";
    } else if (tui->current_screen == SCREEN_DASHBOARD) {
        hint = " [Enter] Start/Stop  [Tab/Shift+Tab] Switch screens  [F1-F4] Screens  [q] Quit";
    } else if (tui->current_screen == SCREEN_CONFIG) {
        hint = " [Up/Down] Select  [Enter] Edit  [Tab/Shift+Tab] Switch screens  [F1-F4] Screens  [q] Quit";
    } else {
        hint = " [Up/Down] Scroll  [Tab/Shift+Tab] Switch screens  [F1-F4] Screens  [q] Quit";
    }

    mvprintw(y, 1, "%s", hint);
    attroff(COLOR_PAIR(CP_HEADER));
}

/* ── Dashboard Screen ────────────────────────────────────────────────── */

static void draw_dashboard(TuiState *tui)
{
    HotspotStatus *hs = tui->hs_status;
    int start_y = 3;
    int half_w = tui->term_cols / 2;
    int box_h = 10;

    /* Clamp box height if terminal is small */
    if (box_h + start_y + 3 > tui->term_rows) {
        box_h = tui->term_rows - start_y - 3;
        if (box_h < 6) box_h = 6;
    }

    /* ── WiFi Status Panel (left) ────────────────────────────────────── */
    int lw = half_w - 1;
    draw_box_title(start_y, 0, lw, box_h, "WiFi Client", CP_BORDER);

    int y = start_y + 1;
    int lbl_w = 12;
    int pad = 2;

    if (hs->wifi.connected) {
        draw_label_value(y++, pad, lbl_w, "Status:",
                         "Connected", CP_STATUS_OK);
        draw_label_value(y++, pad, lbl_w, "SSID:",
                         hs->wifi.ssid, CP_NORMAL);
        draw_label_value(y++, pad, lbl_w, "IP:",
                         hs->wifi.ip[0] ? hs->wifi.ip : "N/A", CP_NORMAL);
        draw_label_value(y++, pad, lbl_w, "MAC:",
                         hs->wifi.mac[0] ? hs->wifi.mac : "N/A", CP_NORMAL);

        char ch_str[32];
        if (hs->wifi.channel > 0) {
            snprintf(ch_str, sizeof(ch_str), "%d (%s)",
                     hs->wifi.channel,
                     hs->wifi.channel >= 32 ? "5 GHz" : "2.4 GHz");
        } else {
            snprintf(ch_str, sizeof(ch_str), "N/A");
        }
        draw_label_value(y++, pad, lbl_w, "Channel:",
                         ch_str, CP_NORMAL);

        char sig_str[32];
        snprintf(sig_str, sizeof(sig_str), "%d dBm", hs->wifi.signal_dbm);
        int sig_cp = hs->wifi.signal_dbm > -50 ? CP_STATUS_OK :
                     hs->wifi.signal_dbm > -70 ? CP_STATUS_WARN : CP_STATUS_ERR;
        draw_label_value(y++, pad, lbl_w, "Signal:",
                         sig_str, sig_cp);
    } else {
        draw_label_value(y++, pad, lbl_w, "Status:",
                         "Disconnected", CP_STATUS_ERR);
        draw_label_value(y++, pad, lbl_w, "Interface:",
                         hs->wifi.name[0] ? hs->wifi.name : "None", CP_NORMAL);
    }

    /* ── Hotspot Status Panel (right) ────────────────────────────────── */
    int rx = half_w;
    int rw = tui->term_cols - half_w;
    draw_box_title(start_y, rx, rw, box_h, "Hotspot", CP_BORDER);

    y = start_y + 1;
    pad = rx + 2;

    draw_label_value(y++, pad, lbl_w, "Status:",
                     state_str(hs->state), state_color(hs->state));
    draw_label_value(y++, pad, lbl_w, "SSID:",
                     hs->config.ssid, CP_NORMAL);
    draw_label_value(y++, pad, lbl_w, "Interface:",
                     hs->ap_iface, CP_NORMAL);

    char client_str[16];
    snprintf(client_str, sizeof(client_str), "%d", hs->client_count);
    draw_label_value(y++, pad, lbl_w, "Clients:",
                     client_str, hs->client_count > 0 ? CP_STATUS_OK : CP_NORMAL);

    if (hs->state == HS_STATE_RUNNING) {
        char uptime[32];
        hotspot_get_uptime_str(hs, uptime, sizeof(uptime));
        draw_label_value(y++, pad, lbl_w, "Uptime:", uptime, CP_STATUS_OK);

        draw_label_value(y++, pad, lbl_w, "Gateway:",
                         AP_GATEWAY, CP_NORMAL);
    }

    if (hs->state == HS_STATE_ERROR && hs->error_msg[0]) {
        draw_label_value(y++, pad, lbl_w, "Error:", "", CP_STATUS_ERR);
        /* Wrap error message */
        int max_err_w = rw - 4;
        int err_len = strlen(hs->error_msg);
        int offset = 0;
        while (offset < err_len && y < start_y + box_h - 1) {
            attron(COLOR_PAIR(CP_STATUS_ERR));
            mvprintw(y++, pad, "%.*s", max_err_w, hs->error_msg + offset);
            attroff(COLOR_PAIR(CP_STATUS_ERR));
            offset += max_err_w;
        }
    }

    /* ── Action Button ───────────────────────────────────────────────── */
    int btn_y = start_y + box_h + 1;
    if (btn_y < tui->term_rows - 2) {
        int btn_cp;
        const char *btn_text;
        if (hs->state == HS_STATE_RUNNING) {
            btn_text = "  [ STOP HOTSPOT ]  ";
            btn_cp   = CP_STATUS_ERR;
        } else if (hs->state == HS_STATE_STOPPED || hs->state == HS_STATE_ERROR) {
            btn_text = "  [ START HOTSPOT ]  ";
            btn_cp   = CP_STATUS_OK;
        } else {
            btn_text = "  [ PLEASE WAIT... ]  ";
            btn_cp   = CP_STATUS_WARN;
        }

        int btn_x = (tui->term_cols - (int)strlen(btn_text)) / 2;
        attron(COLOR_PAIR(btn_cp) | A_BOLD | A_REVERSE);
        mvprintw(btn_y, btn_x, "%s", btn_text);
        attroff(COLOR_PAIR(btn_cp) | A_BOLD | A_REVERSE);

        /* AP support warning */
        if (!hs->wifi.supports_ap && hs->wifi.name[0]) {
            int warn_y = btn_y + 2;
            if (warn_y < tui->term_rows - 2) {
                attron(COLOR_PAIR(CP_STATUS_WARN));
                mvprintw(warn_y, 2,
                    "WARNING: Your WiFi adapter may not support AP+STA concurrency.");
                mvprintw(warn_y + 1, 2,
                    "Check with: iw list | grep -A5 \"valid interface combinations\"");
                attroff(COLOR_PAIR(CP_STATUS_WARN));
            }
        }
    }
}

/* ── Config Screen ───────────────────────────────────────────────────── */

static void draw_config(TuiState *tui)
{
    HotspotConfig *cfg = &tui->hs_status->config;
    int start_y = 4;
    int label_w = 16;
    int field_x = label_w + 6;
    int field_w = 30;

    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvprintw(3, 2, "Hotspot Configuration");
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

    const char *field_names[] = {
        "SSID:", "Password:", "Channel:", "Band:",
        "Max Clients:", "Hidden SSID:"
    };

    char field_values[CFG_FIELD_COUNT][64];
    strncpy(field_values[CFG_SSID], cfg->ssid, 63);

    /* Mask password */
    int pw_len = strlen(cfg->password);
    memset(field_values[CFG_PASSWORD], '*', pw_len);
    field_values[CFG_PASSWORD][pw_len] = '\0';

    if (cfg->channel == 0) {
        snprintf(field_values[CFG_CHANNEL], 64, "Auto (match client)");
    } else {
        snprintf(field_values[CFG_CHANNEL], 64, "%d", cfg->channel);
    }

    /* Band is auto-detected from the client's channel */
    int active_ch = cfg->channel > 0 ? cfg->channel :
                    tui->hs_status->wifi.channel;
    if (active_ch >= 32) {
        snprintf(field_values[CFG_BAND_INFO], 64, "5 GHz (auto)");
    } else {
        snprintf(field_values[CFG_BAND_INFO], 64, "2.4 GHz (auto)");
    }
    snprintf(field_values[CFG_MAX_CLIENTS], 64, "%d", cfg->max_clients);
    snprintf(field_values[CFG_HIDDEN], 64, "%s", cfg->hidden ? "Yes" : "No");

    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        int y = start_y + i * 2;
        if (y >= tui->term_rows - 2) break;

        bool selected = ((int)tui->selected_field == i);

        /* Label */
        if (selected) {
            attron(COLOR_PAIR(CP_HIGHLIGHT) | A_BOLD);
            mvprintw(y, 2, " > ");
        } else {
            attron(COLOR_PAIR(CP_NORMAL));
            mvprintw(y, 2, "   ");
        }
        attroff(A_BOLD);
        attroff(COLOR_PAIR(CP_HIGHLIGHT));

        attron(COLOR_PAIR(CP_NORMAL) | (selected ? A_BOLD : 0));
        mvprintw(y, 5, "%-*s", label_w, field_names[i]);
        attroff(COLOR_PAIR(CP_NORMAL) | (selected ? A_BOLD : 0));

        /* Value */
        if (tui->editing && selected) {
            attron(COLOR_PAIR(CP_INPUT));
            mvprintw(y, field_x, " %-*s ", field_w,
                     tui->edit_buffer);
            attroff(COLOR_PAIR(CP_INPUT));
            /* Show cursor */
            curs_set(1);
            move(y, field_x + 1 + tui->edit_cursor);
        } else {
            attron(COLOR_PAIR(selected ? CP_STATUS_OK : CP_NORMAL));
            mvprintw(y, field_x, " %s", field_values[i]);
            attroff(COLOR_PAIR(selected ? CP_STATUS_OK : CP_NORMAL));
        }
    }

    /* Hotspot state notice */
    if (tui->hs_status->state == HS_STATE_RUNNING) {
        int ny = start_y + CFG_FIELD_COUNT * 2 + 1;
        if (ny < tui->term_rows - 2) {
            attron(COLOR_PAIR(CP_STATUS_WARN));
            mvprintw(ny, 2,
                " Note: Stop the hotspot before changing configuration.");
            attroff(COLOR_PAIR(CP_STATUS_WARN));
        }
    }
}

/* ── Clients Screen ──────────────────────────────────────────────────── */

static void draw_clients(TuiState *tui)
{
    HotspotStatus *hs = tui->hs_status;
    int start_y = 4;

    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvprintw(3, 2, "Connected Clients (%d)", hs->client_count);
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

    if (hs->state != HS_STATE_RUNNING) {
        attron(COLOR_PAIR(CP_STATUS_OFF));
        mvprintw(start_y + 1, 4, "Hotspot is not running.");
        attroff(COLOR_PAIR(CP_STATUS_OFF));
        return;
    }

    if (hs->client_count == 0) {
        attron(COLOR_PAIR(CP_STATUS_OFF));
        mvprintw(start_y + 1, 4, "No clients connected yet.");
        mvprintw(start_y + 2, 4, "Waiting for connections...");
        attroff(COLOR_PAIR(CP_STATUS_OFF));
        return;
    }

    /* Table header */
    int col_mac = 4, col_ip = 24, col_host = 44;

    attron(COLOR_PAIR(CP_HIGHLIGHT) | A_BOLD);
    mvprintw(start_y, col_mac,  "%-20s", "MAC Address");
    mvprintw(start_y, col_ip,   "%-20s", "IP Address");
    mvprintw(start_y, col_host, "%-20s", "Hostname");
    attroff(COLOR_PAIR(CP_HIGHLIGHT) | A_BOLD);

    attron(COLOR_PAIR(CP_BORDER));
    draw_hline(start_y + 1, 2, tui->term_cols - 4, ACS_HLINE);
    attroff(COLOR_PAIR(CP_BORDER));

    int max_visible = tui->term_rows - start_y - 4;
    int start_idx = tui->client_scroll;
    if (start_idx > hs->client_count - max_visible)
        start_idx = hs->client_count - max_visible;
    if (start_idx < 0) start_idx = 0;

    for (int i = 0; i < max_visible && (start_idx + i) < hs->client_count; i++) {
        ConnectedClient *c = &hs->clients[start_idx + i];
        int y = start_y + 2 + i;

        attron(COLOR_PAIR(CP_CLIENT));
        mvprintw(y, col_mac,  "%-20s", c->mac);
        mvprintw(y, col_ip,   "%-20s", c->ip);
        mvprintw(y, col_host, "%-20s", c->hostname);
        attroff(COLOR_PAIR(CP_CLIENT));
    }
}

/* ── Log Screen ──────────────────────────────────────────────────────── */

static void draw_log(TuiState *tui)
{
    int start_y = 4;

    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvprintw(3, 2, "Event Log (%d entries)", tui->log_count);
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

    int max_visible = tui->term_rows - start_y - 2;
    int start_idx = tui->log_count - max_visible - tui->log_scroll;
    if (start_idx < 0) start_idx = 0;

    for (int i = 0; i < max_visible && (start_idx + i) < tui->log_count; i++) {
        LogEntry *entry = &tui->logs[start_idx + i];
        int y = start_y + i;
        if (y >= tui->term_rows - 1) break;

        /* Timestamp */
        struct tm *tm = localtime(&entry->timestamp);
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M:%S", tm);

        attron(COLOR_PAIR(CP_NORMAL) | A_DIM);
        mvprintw(y, 2, "%s ", ts);
        attroff(COLOR_PAIR(CP_NORMAL) | A_DIM);

        /* Level indicator */
        int cp;
        const char *prefix;
        switch (entry->level) {
            case LOG_INFO:    cp = CP_LOG_INFO; prefix = "INFO "; break;
            case LOG_WARN:    cp = CP_LOG_WARN; prefix = "WARN "; break;
            case LOG_ERROR:   cp = CP_LOG_ERR;  prefix = "ERR  "; break;
            case LOG_SUCCESS: cp = CP_STATUS_OK; prefix = " OK  "; break;
            default:          cp = CP_NORMAL;    prefix = "     "; break;
        }

        attron(COLOR_PAIR(cp) | A_BOLD);
        printw("%s", prefix);
        attroff(A_BOLD);

        /* Message - truncate to fit */
        int max_msg_w = tui->term_cols - 18;
        printw("%-.*s", max_msg_w, entry->message);
        attroff(COLOR_PAIR(cp));
    }
}

/* ── Redraw ──────────────────────────────────────────────────────────── */

void tui_redraw(TuiState *tui)
{
    getmaxyx(stdscr, tui->term_rows, tui->term_cols);
    erase();

    /* Minimum size check */
    if (tui->term_cols < 60 || tui->term_rows < 15) {
        attron(COLOR_PAIR(CP_STATUS_WARN) | A_BOLD);
        mvprintw(tui->term_rows / 2, 2,
                 "Terminal too small. Resize to at least 60x15.");
        attroff(COLOR_PAIR(CP_STATUS_WARN) | A_BOLD);
        refresh();
        return;
    }

    draw_header(tui);
    draw_tabs(tui);

    switch (tui->current_screen) {
        case SCREEN_DASHBOARD: draw_dashboard(tui); break;
        case SCREEN_CONFIG:    draw_config(tui);    break;
        case SCREEN_CLIENTS:   draw_clients(tui);   break;
        case SCREEN_LOG:       draw_log(tui);       break;
        default: break;
    }

    draw_footer(tui);
    refresh();
}

/* ── Config Field Editing ────────────────────────────────────────────── */

static void start_edit(TuiState *tui)
{
    HotspotConfig *cfg = &tui->hs_status->config;

    /* Don't allow editing while hotspot is running */
    if (tui->hs_status->state == HS_STATE_RUNNING) return;

    tui->editing = true;

    switch (tui->selected_field) {
        case CFG_SSID:
            strncpy(tui->edit_buffer, cfg->ssid, MAX_SSID_LEN - 1);
            break;
        case CFG_PASSWORD:
            strncpy(tui->edit_buffer, cfg->password, MAX_SSID_LEN - 1);
            break;
        case CFG_CHANNEL: {
            if (cfg->channel == 0)
                strcpy(tui->edit_buffer, "0");
            else
                snprintf(tui->edit_buffer, MAX_SSID_LEN, "%d", cfg->channel);
            break;
        }
        case CFG_BAND_INFO:
            /* Read-only — band is auto-detected from client channel */
            tui->editing = false;
            tui_log(tui, LOG_INFO,
                    "Band is auto-detected from WiFi channel. "
                    "AP must use same band as client connection.");
            return;
        case CFG_MAX_CLIENTS:
            snprintf(tui->edit_buffer, MAX_SSID_LEN, "%d", cfg->max_clients);
            break;
        case CFG_HIDDEN:
            /* Toggle */
            cfg->hidden = !cfg->hidden;
            /* Persist */
            if (hotspot_save_config(cfg)) {
                tui_log(tui, LOG_INFO, "Hidden SSID: %s (saved)",
                        cfg->hidden ? "Yes" : "No");
            } else {
                tui_log(tui, LOG_WARN, "Hidden SSID: %s (save failed)",
                        cfg->hidden ? "Yes" : "No");
            }
            tui->editing = false;
            return;
        default:
            tui->editing = false;
            return;
    }
    tui->edit_cursor = strlen(tui->edit_buffer);
}

static void save_edit(TuiState *tui)
{
    HotspotConfig *cfg = &tui->hs_status->config;

    switch (tui->selected_field) {
        case CFG_SSID:
            if (strlen(tui->edit_buffer) > 0) {
                strncpy(cfg->ssid, tui->edit_buffer, MAX_SSID_LEN - 1);
                if (hotspot_save_config(cfg)) {
                    tui_log(tui, LOG_INFO, "SSID changed to: %s (saved)", cfg->ssid);
                } else {
                    tui_log(tui, LOG_WARN, "SSID changed to: %s (save failed)", cfg->ssid);
                }
            }
            break;
        case CFG_PASSWORD:
            if (strlen(tui->edit_buffer) >= 8) {
                strncpy(cfg->password, tui->edit_buffer, MAX_SSID_LEN - 1);
                if (hotspot_save_config(cfg)) {
                    tui_log(tui, LOG_INFO, "Password updated (saved).");
                } else {
                    tui_log(tui, LOG_WARN, "Password updated (save failed).");
                }
            } else {
                tui_log(tui, LOG_WARN, "Password must be at least 8 characters.");
            }
            break;
        case CFG_CHANNEL: {
            int ch = atoi(tui->edit_buffer);
            if (ch >= 0 && ch <= 196) {
                cfg->channel = ch;
                if (hotspot_save_config(cfg)) {
                    tui_log(tui, LOG_INFO, "Channel set to %s (saved)",
                            ch == 0 ? "Auto" : tui->edit_buffer);
                } else {
                    tui_log(tui, LOG_WARN, "Channel set to %s (save failed)",
                            ch == 0 ? "Auto" : tui->edit_buffer);
                }
            } else {
                tui_log(tui, LOG_WARN, "Invalid channel (0=auto, 1-14 for 2.4GHz)");
            }
            break;
        }
        case CFG_MAX_CLIENTS: {
            int mc = atoi(tui->edit_buffer);
            if (mc > 0 && mc <= 255) {
                cfg->max_clients = mc;
                if (hotspot_save_config(cfg)) {
                    tui_log(tui, LOG_INFO, "Max clients set to %d (saved)", mc);
                } else {
                    tui_log(tui, LOG_WARN, "Max clients set to %d (save failed)", mc);
                }
            } else {
                tui_log(tui, LOG_WARN, "Invalid max clients (1-255)");
            }
            break;
        }
        default:
            break;
    }

    tui->editing = false;
    curs_set(0);
}

static void cancel_edit(TuiState *tui)
{
    tui->editing = false;
    curs_set(0);
}

/* ── Handle Edit Key ─────────────────────────────────────────────────── */

static void handle_edit_key(TuiState *tui, int ch)
{
    int len = strlen(tui->edit_buffer);

    switch (ch) {
        case '\n':
        case KEY_ENTER:
            save_edit(tui);
            break;
        case 27: /* ESC */
            cancel_edit(tui);
            break;
        case KEY_BACKSPACE:
        case 127:
        case '\b':
            if (tui->edit_cursor > 0) {
                memmove(&tui->edit_buffer[tui->edit_cursor - 1],
                        &tui->edit_buffer[tui->edit_cursor],
                        len - tui->edit_cursor + 1);
                tui->edit_cursor--;
            }
            break;
        case KEY_LEFT:
            if (tui->edit_cursor > 0) tui->edit_cursor--;
            break;
        case KEY_RIGHT:
            if (tui->edit_cursor < len) tui->edit_cursor++;
            break;
        case KEY_HOME:
            tui->edit_cursor = 0;
            break;
        case KEY_END:
            tui->edit_cursor = len;
            break;
        default:
            if (ch >= 32 && ch < 127 && len < MAX_SSID_LEN - 2) {
                memmove(&tui->edit_buffer[tui->edit_cursor + 1],
                        &tui->edit_buffer[tui->edit_cursor],
                        len - tui->edit_cursor + 1);
                tui->edit_buffer[tui->edit_cursor] = (char)ch;
                tui->edit_cursor++;
            }
            break;
    }
}

/* ── Main Event Loop ─────────────────────────────────────────────────── */

void tui_run(TuiState *tui)
{
    time_t last_refresh = 0;

    while (tui->running) {
        /* Handle resize */
        if (g_resize) {
            g_resize = 0;
            endwin();
            refresh();
            getmaxyx(stdscr, tui->term_rows, tui->term_cols);
        }

        /* Periodic status refresh (every 2 seconds) */
        time_t now = time(NULL);
        if (now - last_refresh >= 2) {
            if (tui->hs_status->state == HS_STATE_RUNNING) {
                hotspot_refresh_status(tui->hs_status);
            } else if (tui->hs_status->wifi.name[0]) {
                net_refresh_wifi_status(&tui->hs_status->wifi);
            }
            last_refresh = now;
        }

        /* Redraw */
        tui_redraw(tui);

        /* Input */
        int ch = getch();
        if (ch == ERR) continue;

        /* If editing, handle edit keys */
        if (tui->editing) {
            handle_edit_key(tui, ch);
            continue;
        }

        switch (ch) {
            case 'q':
            case 'Q':
                tui->running = false;
                break;

            case KEY_F(1):
                tui->current_screen = SCREEN_DASHBOARD;
                break;
            case KEY_F(2):
                tui->current_screen = SCREEN_CONFIG;
                break;
            case KEY_F(3):
                tui->current_screen = SCREEN_CLIENTS;
                break;
            case KEY_F(4):
                tui->current_screen = SCREEN_LOG;
                break;

            case KEY_BTAB: /* Shift+Tab (reverse) */
                tui->current_screen = (tui->current_screen - 1 + SCREEN_COUNT) % SCREEN_COUNT;
                break;

            case '\t': /* Tab (forward) */
                tui->current_screen = (tui->current_screen + 1) % SCREEN_COUNT;
                break;

            case '\n':
            case KEY_ENTER:
                if (tui->current_screen == SCREEN_DASHBOARD) {
                    if (tui->hs_status->state == HS_STATE_STOPPED ||
                        tui->hs_status->state == HS_STATE_ERROR) {
                        tui_log(tui, LOG_INFO, "Starting hotspot...");
                        tui_redraw(tui);
                        if (hotspot_start(tui->hs_status)) {
                            tui_log(tui, LOG_SUCCESS,
                                    "Hotspot started! SSID: %s",
                                    tui->hs_status->config.ssid);
                        } else {
                            tui_log(tui, LOG_ERROR,
                                    "Failed: %s", tui->hs_status->error_msg);
                        }
                    } else if (tui->hs_status->state == HS_STATE_RUNNING) {
                        tui_log(tui, LOG_INFO, "Stopping hotspot...");
                        tui_redraw(tui);
                        hotspot_stop(tui->hs_status);
                        tui_log(tui, LOG_SUCCESS, "Hotspot stopped.");
                    }
                } else if (tui->current_screen == SCREEN_CONFIG) {
                    start_edit(tui);
                }
                break;

            case KEY_UP:
                if (tui->current_screen == SCREEN_CONFIG) {
                    if (tui->selected_field > 0)
                        tui->selected_field--;
                } else if (tui->current_screen == SCREEN_LOG) {
                    if (tui->log_scroll < tui->log_count - 1)
                        tui->log_scroll++;
                } else if (tui->current_screen == SCREEN_CLIENTS) {
                    if (tui->client_scroll > 0)
                        tui->client_scroll--;
                }
                break;

            case KEY_DOWN:
                if (tui->current_screen == SCREEN_CONFIG) {
                    if (tui->selected_field < CFG_FIELD_COUNT - 1)
                        tui->selected_field++;
                } else if (tui->current_screen == SCREEN_LOG) {
                    if (tui->log_scroll > 0)
                        tui->log_scroll--;
                } else if (tui->current_screen == SCREEN_CLIENTS) {
                    tui->client_scroll++;
                }
                break;

            default:
                break;
        }
    }
}
