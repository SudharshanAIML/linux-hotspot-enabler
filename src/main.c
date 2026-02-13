/*
 * main.c - Entry point for Linux Hotspot Enabler
 *
 * Checks root privileges, verifies dependencies, detects WiFi interface,
 * and launches the ncurses TUI.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>

#include "net_utils.h"
#include "hotspot.h"
#include "tui.h"

/* ── Globals for signal handling ─────────────────────────────────────── */

static HotspotStatus g_hs_status;
static TuiState      g_tui;
static volatile sig_atomic_t g_shutdown = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
    g_tui.running = false;
}

/* ── Print banner (non-TUI mode) ─────────────────────────────────────── */

static void print_banner(void)
{
    printf("\n");
    printf("  ╔══════════════════════════════════════════════╗\n");
    printf("  ║       LINUX HOTSPOT ENABLER v1.0             ║\n");
    printf("  ║   Simultaneous WiFi + Hotspot for Linux      ║\n");
    printf("  ╚══════════════════════════════════════════════╝\n");
    printf("\n");
}

/* ── Dependency Check (non-TUI mode) ─────────────────────────────────── */

static int check_and_report_deps(void)
{
    DependencyStatus deps = net_check_dependencies();

    if (deps.all_present) return 0;

    printf("  ⚠  Missing dependencies:\n\n");
    if (!deps.has_iw)       printf("    ✗ iw        — wireless configuration tool\n");
    if (!deps.has_hostapd)  printf("    ✗ hostapd   — access point daemon\n");
    if (!deps.has_dnsmasq)  printf("    ✗ dnsmasq   — DHCP/DNS server\n");
    if (!deps.has_iptables) printf("    ✗ iptables  — firewall/NAT rules\n");

    printf("\n  Install with:\n\n");
    char cmd[MAX_CMD_LEN];
    net_get_install_command(cmd, sizeof(cmd));
    printf("    %s\n\n", cmd);

    return 1;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    print_banner();

    /* 1. Root check */
    if (geteuid() != 0) {
        printf("  ✗ This tool requires root privileges.\n");
        printf("    Run with: sudo %s\n\n", argv[0]);
        return 1;
    }

    /* 2. Dependency check */
    if (check_and_report_deps() != 0) {
        return 1;
    }
    printf("  ✓ All dependencies found.\n");

    /* 3. Init hotspot status */
    hotspot_init(&g_hs_status);

    /* 4. Detect WiFi interface */
    if (!net_detect_wifi_interface(&g_hs_status.wifi)) {
        printf("  ✗ No WiFi interface detected!\n");
        printf("    Make sure your WiFi adapter is connected and drivers are loaded.\n\n");
        return 1;
    }

    printf("  ✓ WiFi interface: %s\n", g_hs_status.wifi.name);
    if (g_hs_status.wifi.connected) {
        printf("  ✓ Connected to: %s (channel %d)\n",
               g_hs_status.wifi.ssid, g_hs_status.wifi.channel);
    } else {
        printf("  ⚠ WiFi is not connected to any network.\n");
        printf("    Connect to WiFi first for internet sharing.\n");
    }

    /* Get PHY name */
    net_get_phy_name(g_hs_status.wifi.name,
                     g_hs_status.phy, sizeof(g_hs_status.phy));

    if (g_hs_status.wifi.supports_ap) {
        printf("  ✓ AP/STA concurrency supported.\n");
    } else {
        printf("  ⚠ AP/STA concurrency may not be supported by your adapter.\n");
        printf("    The hotspot might not work. Try anyway? [Y/n] ");
        fflush(stdout);
        int c = getchar();
        if (c == 'n' || c == 'N') {
            printf("\n  Exiting.\n\n");
            return 1;
        }
    }

    printf("\n  Launching TUI...\n");
    usleep(500000);

    /* 5. Setup signal handlers */
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags   = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 6. Init and run TUI */
    tui_init(&g_tui, &g_hs_status);
    tui_run(&g_tui);
    tui_cleanup(&g_tui);

    /* 7. Cleanup on exit */
    printf("\n");
    print_banner();

    if (g_hs_status.state == HS_STATE_RUNNING) {
        printf("  Stopping hotspot...\n");
        hotspot_stop(&g_hs_status);
        printf("  ✓ Hotspot stopped.\n");
    }

    /* Final cleanup - make sure everything is clean */
    hotspot_cleanup(&g_hs_status);
    printf("  ✓ Cleanup complete. Goodbye!\n\n");

    return 0;
}
