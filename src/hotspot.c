/*
 * hotspot.c - Core hotspot management for Linux Hotspot Enabler
 *
 * Creates a virtual AP interface, manages hostapd + dnsmasq processes,
 * and configures iptables NAT for internet sharing.
 *
 * Flow: create ap0 → NM unmanage → hostapd brings it up →
 *       assign IP → dnsmasq → iptables NAT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "hotspot.h"

/* ── Initialization ──────────────────────────────────────────────────── */

void hotspot_default_config(HotspotConfig *config)
{
    strncpy(config->ssid, "LinuxHotspot", MAX_SSID_LEN - 1);
    strncpy(config->password, "password123", MAX_SSID_LEN - 1);
    config->channel     = 0;  /* auto — match client */
    config->max_clients = 10;
    config->hidden      = false;
}

void hotspot_init(HotspotStatus *status)
{
    memset(status, 0, sizeof(HotspotStatus));
    status->state = HS_STATE_STOPPED;
    strncpy(status->ap_iface, AP_IFACE_NAME, MAX_IFACE_NAME - 1);
    hotspot_default_config(&status->config);
}

/* ── Generate hostapd config ─────────────────────────────────────────── */

/*
 * Auto-detect band from channel number:
 *   Channels 1-14   → 2.4 GHz (hw_mode=g)
 *   Channels 32-177  → 5 GHz   (hw_mode=a)
 *
 * With AP/STA concurrency on a single radio, the AP MUST use the
 * same channel (and therefore same band) as the client connection.
 * There is no manual 5GHz toggle — it's physically determined by
 * which channel the client is connected on.
 */
static bool is_5ghz_channel(int ch)
{
    return (ch >= 32);
}

static bool generate_hostapd_conf(const HotspotStatus *status)
{
    FILE *fp = fopen(HOSTAPD_CONF_PATH, "w");
    if (!fp) return false;

    /* Channel: always match the WiFi client for AP/STA concurrency */
    int channel = status->config.channel;
    if (channel == 0) {
        channel = status->wifi.channel;
        if (channel <= 0) channel = 6; /* safe fallback */
    }

    bool use_5ghz = is_5ghz_channel(channel);
    const char *hw_mode = use_5ghz ? "a" : "g";

    fprintf(fp,
        "interface=%s\n"
        "driver=nl80211\n"
        "ssid=%s\n"
        "hw_mode=%s\n"
        "channel=%d\n"
        "wmm_enabled=0\n"
        "macaddr_acl=0\n"
        "auth_algs=1\n"
        "ignore_broadcast_ssid=%d\n"
        "wpa=2\n"
        "wpa_passphrase=%s\n"
        "wpa_key_mgmt=WPA-PSK\n"
        "rsn_pairwise=CCMP\n"
        "ieee80211n=1\n",
        status->ap_iface,
        status->config.ssid,
        hw_mode,
        channel,
        status->config.hidden ? 1 : 0,
        status->config.password
    );

    if (use_5ghz) {
        fprintf(fp, "ieee80211ac=1\n");
        /* Regulatory: required for 5 GHz DFS channels */
        fprintf(fp, "country_code=US\n");
        fprintf(fp, "ieee80211d=1\n");
    }

    fclose(fp);
    return true;
}

/* ── Generate dnsmasq config ─────────────────────────────────────────── */

static bool generate_dnsmasq_conf(const HotspotStatus *status)
{
    FILE *fp = fopen(DNSMASQ_CONF_PATH, "w");
    if (!fp) return false;

    fprintf(fp,
        "interface=%s\n"
        "bind-interfaces\n"
        "dhcp-range=%s,%s,12h\n"
        "dhcp-option=option:router,%s\n"
        "dhcp-option=option:dns-server,8.8.8.8,8.8.4.4\n"
        "dhcp-leasefile=%s\n"
        "log-facility=/tmp/hotspot_enabler_dnsmasq.log\n",
        status->ap_iface,
        AP_DHCP_START,
        AP_DHCP_END,
        AP_GATEWAY,
        DNSMASQ_LEASE_FILE
    );

    fclose(fp);
    return true;
}

/* ── NetworkManager Management ───────────────────────────────────────── */

#define NM_UNMANAGED_CONF "/etc/NetworkManager/conf.d/hotspot-enabler-unmanaged.conf"

/*
 * Cross-distro network manager handling:
 *   - NetworkManager (Ubuntu, Fedora, Zorin, Mint, Arch GUI)
 *   - connman (some lightweight distros)
 *   - systemd-networkd (server distros)
 *   - None (some minimal installs)
 *
 * All commands use 2>/dev/null so missing tools don't cause errors.
 */
static void nm_unmanage_interface(const char *ap_iface)
{
    char cmd[MAX_CMD_LEN];

    /* -- NetworkManager (most desktop Linux distros) -- */
    FILE *fp = fopen(NM_UNMANAGED_CONF, "w");
    if (fp) {
        fprintf(fp,
            "[keyfile]\n"
            "unmanaged-devices=interface-name:%s\n",
            ap_iface);
        fclose(fp);
    }
    net_exec_silent("nmcli general reload conf 2>/dev/null");
    usleep(500000);
    snprintf(cmd, sizeof(cmd),
             "nmcli device set %s managed no 2>/dev/null", ap_iface);
    net_exec_silent(cmd);

    /* -- connman (Raspberry Pi OS, some lightweight distros) -- */
    snprintf(cmd, sizeof(cmd),
             "connmanctl disable wifi %s 2>/dev/null", ap_iface);
    net_exec_silent(cmd);

    /* -- wpa_supplicant (may auto-attach to new interfaces) -- */
    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i %s disconnect 2>/dev/null", ap_iface);
    net_exec_silent(cmd);
    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i %s terminate 2>/dev/null", ap_iface);
    net_exec_silent(cmd);

    usleep(500000);
}

static void nm_cleanup_unmanaged(void)
{
    unlink(NM_UNMANAGED_CONF);
    net_exec_silent("nmcli general reload conf 2>/dev/null");
}

/* ── Create Virtual AP Interface ─────────────────────────────────────── */

/*
 * Creates the virtual AP interface and tells NM to ignore it.
 * Does NOT bring it up or assign IP — hostapd handles that.
 */
static bool create_ap_interface(HotspotStatus *status)
{
    char cmd[MAX_CMD_LEN];

    /* Stop conflicting services */
    net_exec_silent("pkill hostapd 2>/dev/null");
    snprintf(cmd, sizeof(cmd),
             "pkill -f 'dnsmasq.*%s' 2>/dev/null", status->ap_iface);
    net_exec_silent(cmd);
    net_exec_silent("rfkill unblock wifi 2>/dev/null");
    usleep(300000);

    /* Remove ap0 if already exists */
    snprintf(cmd, sizeof(cmd), "iw dev %s del >/dev/null 2>&1", status->ap_iface);
    net_exec_silent(cmd);
    usleep(500000);

    /* Pre-configure NM to ignore the interface BEFORE creating it */
    FILE *fp = fopen(NM_UNMANAGED_CONF, "w");
    if (fp) {
        fprintf(fp,
            "[keyfile]\n"
            "unmanaged-devices=interface-name:%s\n",
            status->ap_iface);
        fclose(fp);
        net_exec_silent("nmcli general reload conf 2>/dev/null");
        usleep(300000);
    }

    /* Create virtual interface */
    snprintf(cmd, sizeof(cmd),
             "iw dev %s interface add %s type __ap",
             status->wifi.name, status->ap_iface);
    if (net_exec_silent(cmd) != 0) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "Failed to create virtual AP interface. "
                 "Your WiFi driver may not support AP/STA concurrency.");
        return false;
    }

    /* Wait for interface to appear, then make NM release it */
    usleep(500000);
    nm_unmanage_interface(status->ap_iface);

    return true;
}

/* ── Assign IP to AP interface (called AFTER hostapd starts) ─────────── */

static bool assign_ap_ip(HotspotStatus *status)
{
    char cmd[MAX_CMD_LEN];

    /* Bring up if not already (hostapd should have done this) */
    snprintf(cmd, sizeof(cmd), "ip link set %s up 2>/dev/null", status->ap_iface);
    net_exec_silent(cmd);

    /* Flush existing addresses */
    snprintf(cmd, sizeof(cmd), "ip addr flush dev %s 2>/dev/null", status->ap_iface);
    net_exec_silent(cmd);

    /* Assign gateway IP */
    snprintf(cmd, sizeof(cmd), "ip addr add %s/24 dev %s",
             AP_GATEWAY, status->ap_iface);
    int ret = net_exec_silent(cmd);

    if (ret != 0) {
        /* Retry — might already be assigned (RTNETLINK: File exists) */
        usleep(200000);
        snprintf(cmd, sizeof(cmd),
                 "ip addr replace %s/24 dev %s 2>/dev/null",
                 AP_GATEWAY, status->ap_iface);
        net_exec_silent(cmd);
    }

    return true;
}

/* ── Setup iptables NAT ──────────────────────────────────────────────── */

static bool setup_nat(HotspotStatus *status)
{
    char cmd[MAX_CMD_LEN];

    /* Save current IP forwarding state */
    char output[16] = {0};
    net_exec_cmd("cat /proc/sys/net/ipv4/ip_forward", output, sizeof(output));
    status->ip_forward_was_enabled = (atoi(output) == 1);

    /* Enable IP forwarding via sysctl (more reliable than echo) */
    net_exec_silent("sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1");
    net_exec_silent("echo 1 > /proc/sys/net/ipv4/ip_forward 2>/dev/null");

    /* NAT masquerade */
    snprintf(cmd, sizeof(cmd),
             "iptables -t nat -A POSTROUTING -o %s -j MASQUERADE",
             status->wifi.name);
    net_exec_silent(cmd);

    /* Allow forwarding */
    snprintf(cmd, sizeof(cmd),
             "iptables -A FORWARD -i %s -o %s -m state "
             "--state RELATED,ESTABLISHED -j ACCEPT",
             status->wifi.name, status->ap_iface);
    net_exec_silent(cmd);

    snprintf(cmd, sizeof(cmd),
             "iptables -A FORWARD -i %s -o %s -j ACCEPT",
             status->ap_iface, status->wifi.name);
    net_exec_silent(cmd);

    return true;
}

/* ── Remove iptables NAT ────────────────────────────────────────────── */

static void remove_nat(HotspotStatus *status)
{
    char cmd[MAX_CMD_LEN];

    snprintf(cmd, sizeof(cmd),
             "iptables -t nat -D POSTROUTING -o %s -j MASQUERADE 2>/dev/null",
             status->wifi.name);
    net_exec_silent(cmd);

    snprintf(cmd, sizeof(cmd),
             "iptables -D FORWARD -i %s -o %s -m state "
             "--state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null",
             status->wifi.name, status->ap_iface);
    net_exec_silent(cmd);

    snprintf(cmd, sizeof(cmd),
             "iptables -D FORWARD -i %s -o %s -j ACCEPT 2>/dev/null",
             status->ap_iface, status->wifi.name);
    net_exec_silent(cmd);

    if (!status->ip_forward_was_enabled) {
        net_exec_silent("sysctl -w net.ipv4.ip_forward=0 >/dev/null 2>&1");
    }
}

/* ── Start hostapd ───────────────────────────────────────────────────── */

static bool start_hostapd(HotspotStatus *status)
{
    char cmd[MAX_CMD_LEN];

    /* hostapd brings the interface up itself */
    snprintf(cmd, sizeof(cmd),
             "hostapd -B %s -f %s",
             HOSTAPD_CONF_PATH, HOSTAPD_LOG_PATH);

    if (net_exec_silent(cmd) != 0) {
        char log_output[512] = {0};
        snprintf(cmd, sizeof(cmd), "tail -5 %s 2>/dev/null", HOSTAPD_LOG_PATH);
        net_exec_cmd(cmd, log_output, sizeof(log_output));
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "hostapd failed: %.400s", log_output);
        return false;
    }

    /* Give hostapd time to initialize and bring interface up */
    usleep(1500000); /* 1.5 seconds */

    char output[64] = {0};
    net_exec_cmd("pidof hostapd", output, sizeof(output));
    status->hostapd_pid = atoi(output);

    return (status->hostapd_pid > 0);
}

/* ── Start dnsmasq ───────────────────────────────────────────────────── */

static bool start_dnsmasq(HotspotStatus *status)
{
    char cmd[MAX_CMD_LEN];

    /* Kill any conflicting dnsmasq */
    snprintf(cmd, sizeof(cmd),
             "pkill -f 'dnsmasq.*%s' 2>/dev/null", status->ap_iface);
    net_exec_silent(cmd);
    net_exec_silent("systemctl stop dnsmasq 2>/dev/null");
    usleep(300000);

    snprintf(cmd, sizeof(cmd),
             "dnsmasq -C %s --pid-file=/tmp/hotspot_enabler_dnsmasq.pid",
             DNSMASQ_CONF_PATH);

    if (net_exec_silent(cmd) != 0) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "Failed to start dnsmasq. Port 53 may be in use.");
        return false;
    }

    char output[64] = {0};
    usleep(300000);
    net_exec_cmd("cat /tmp/hotspot_enabler_dnsmasq.pid 2>/dev/null",
                 output, sizeof(output));
    status->dnsmasq_pid = atoi(output);

    return (status->dnsmasq_pid > 0);
}

/* ── Kill Process Safely ─────────────────────────────────────────────── */

static void kill_process(pid_t pid, const char *name)
{
    if (pid <= 0) return;

    if (kill(pid, SIGTERM) == 0) {
        for (int i = 0; i < 30; i++) {
            if (kill(pid, 0) != 0) return;
            usleep(100000);
        }
        kill(pid, SIGKILL);
    }

    if (name && name[0]) {
        char cmd[MAX_CMD_LEN];
        snprintf(cmd, sizeof(cmd), "pkill -9 %s 2>/dev/null", name);
        net_exec_silent(cmd);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  START HOTSPOT
 *
 *  Flow: detect WiFi → create ap0 → NM unmanage → generate configs →
 *        start hostapd (brings ap0 up) → assign IP → dnsmasq → NAT
 * ════════════════════════════════════════════════════════════════════════ */

bool hotspot_start(HotspotStatus *status)
{
    status->state = HS_STATE_STARTING;
    status->error_msg[0] = '\0';

    /* 1. Detect WiFi interface */
    if (!net_detect_wifi_interface(&status->wifi)) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "No WiFi interface detected.");
        status->state = HS_STATE_ERROR;
        return false;
    }

    /* 2. Get PHY name */
    if (!net_get_phy_name(status->wifi.name, status->phy, sizeof(status->phy))) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "Cannot determine physical WiFi device.");
        status->state = HS_STATE_ERROR;
        return false;
    }

    /* 3. Create virtual AP interface (does NOT bring it up) */
    if (!create_ap_interface(status)) {
        status->state = HS_STATE_ERROR;
        return false;
    }

    /* 4. Generate configs */
    if (!generate_hostapd_conf(status)) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "Failed to generate hostapd configuration.");
        status->state = HS_STATE_ERROR;
        hotspot_cleanup(status);
        return false;
    }

    if (!generate_dnsmasq_conf(status)) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "Failed to generate dnsmasq configuration.");
        status->state = HS_STATE_ERROR;
        hotspot_cleanup(status);
        return false;
    }

    /* 5. Start hostapd — this brings the AP interface UP */
    if (!start_hostapd(status)) {
        status->state = HS_STATE_ERROR;
        hotspot_cleanup(status);
        return false;
    }

    /* 6. Assign IP to AP interface (after hostapd brought it up) */
    assign_ap_ip(status);

    /* 7. Start dnsmasq */
    if (!start_dnsmasq(status)) {
        status->state = HS_STATE_ERROR;
        hotspot_cleanup(status);
        return false;
    }

    /* 8. Setup NAT */
    if (!setup_nat(status)) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "Failed to configure NAT forwarding.");
        status->state = HS_STATE_ERROR;
        hotspot_cleanup(status);
        return false;
    }

    status->state = HS_STATE_RUNNING;
    status->start_time = time(NULL);
    status->client_count = 0;

    return true;
}

/* ── Stop Hotspot ────────────────────────────────────────────────────── */

bool hotspot_stop(HotspotStatus *status)
{
    status->state = HS_STATE_STOPPING;
    hotspot_cleanup(status);
    status->state = HS_STATE_STOPPED;
    return true;
}

/* ── Cleanup ─────────────────────────────────────────────────────────── */

void hotspot_cleanup(HotspotStatus *status)
{
    /* Kill hostapd */
    kill_process(status->hostapd_pid, "hostapd");
    status->hostapd_pid = 0;

    /* Kill dnsmasq (ours) */
    kill_process(status->dnsmasq_pid, "");

    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd),
             "pkill -f '%s' 2>/dev/null", DNSMASQ_CONF_PATH);
    net_exec_silent(cmd);
    status->dnsmasq_pid = 0;

    /* Remove NAT rules */
    remove_nat(status);

    /* Remove AP interface */
    snprintf(cmd, sizeof(cmd), "iw dev %s del 2>/dev/null", status->ap_iface);
    net_exec_silent(cmd);

    /* Restore NetworkManager config */
    nm_cleanup_unmanaged();

    /* Clean up temp files */
    unlink(HOSTAPD_CONF_PATH);
    unlink(DNSMASQ_CONF_PATH);
    unlink(DNSMASQ_LEASE_FILE);
    unlink(HOSTAPD_LOG_PATH);
    unlink("/tmp/hotspot_enabler_dnsmasq.pid");
    unlink("/tmp/hotspot_enabler_dnsmasq.log");

    status->client_count = 0;
    status->start_time = 0;
}

/* ── Refresh Status ──────────────────────────────────────────────────── */

void hotspot_refresh_status(HotspotStatus *status)
{
    if (status->state != HS_STATE_RUNNING) return;

    if (status->hostapd_pid > 0 && kill(status->hostapd_pid, 0) != 0) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "hostapd process died unexpectedly.");
        status->state = HS_STATE_ERROR;
        return;
    }

    if (status->dnsmasq_pid > 0 && kill(status->dnsmasq_pid, 0) != 0) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "dnsmasq process died unexpectedly.");
        status->state = HS_STATE_ERROR;
        return;
    }

    net_refresh_wifi_status(&status->wifi);

    status->client_count = net_get_connected_clients(
        status->clients, MAX_CLIENTS);
}

/* ── Uptime String ───────────────────────────────────────────────────── */

void hotspot_get_uptime_str(const HotspotStatus *status,
                            char *buf, size_t bufsize)
{
    if (status->start_time == 0) {
        snprintf(buf, bufsize, "--");
        return;
    }

    time_t elapsed = time(NULL) - status->start_time;
    int hours   = elapsed / 3600;
    int minutes = (elapsed % 3600) / 60;
    int seconds = elapsed % 60;

    if (hours > 0) {
        snprintf(buf, bufsize, "%dh %dm %ds", hours, minutes, seconds);
    } else if (minutes > 0) {
        snprintf(buf, bufsize, "%dm %ds", minutes, seconds);
    } else {
        snprintf(buf, bufsize, "%ds", seconds);
    }
}
