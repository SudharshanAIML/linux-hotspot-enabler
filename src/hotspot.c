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
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>

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

/* ── Persistent configuration (simple key=value file) ─────────────── */

/* Save hotspot config atomically to HOTSPOT_CONFIG_PATH with 0600 perms. */
bool hotspot_save_config(const HotspotConfig *config)
{
    char tmp_path[PATH_MAX];
    int fd = -1;
    FILE *fp = NULL;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmpXXXXXX", HOTSPOT_CONFIG_PATH);
    fd = mkstemp(tmp_path);
    if (fd < 0) return false;

    /* Set strict permissions */
    fchmod(fd, S_IRUSR | S_IWUSR);

    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp_path);
        return false;
    }

    fprintf(fp, "ssid=%s\n", config->ssid);
    fprintf(fp, "password=%s\n", config->password);
    fprintf(fp, "channel=%d\n", config->channel);
    fprintf(fp, "max_clients=%d\n", config->max_clients);
    fprintf(fp, "hidden=%d\n", config->hidden ? 1 : 0);

    fflush(fp);
    fsync(fd);
    fclose(fp);

    if (rename(tmp_path, HOTSPOT_CONFIG_PATH) != 0) {
        unlink(tmp_path);
        return false;
    }

    return true;
}

/* Load persisted config (returns true if file existed and values applied) */
bool hotspot_load_config(HotspotConfig *config)
{
    FILE *fp = fopen(HOTSPOT_CONFIG_PATH, "r");
    if (!fp) return false;

    char line[512];
    bool any = false;
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        if (strcmp(key, "ssid") == 0) {
            strncpy(config->ssid, val, MAX_SSID_LEN - 1);
            any = true;
        } else if (strcmp(key, "password") == 0) {
            strncpy(config->password, val, MAX_SSID_LEN - 1);
            any = true;
        } else if (strcmp(key, "channel") == 0) {
            config->channel = atoi(val);
            any = true;
        } else if (strcmp(key, "max_clients") == 0) {
            config->max_clients = atoi(val);
            any = true;
        } else if (strcmp(key, "hidden") == 0) {
            config->hidden = (atoi(val) != 0);
            any = true;
        }
    }

    fclose(fp);
    return any;
}

/* ── Generate hostapd config ─────────────────────────────────────────── */

/*
 * Auto-detect band from channel number:
 *   Channels 1-14   → 2.4 GHz (hw_mode=g)
 *   Channels 32+    → 5 GHz   (hw_mode=a)
 */
static bool is_5ghz_channel(int ch)
{
    return (ch >= 32);
}

/*
 * Get the regulatory country code from the system.
 * Falls back to "US" if detection fails.
 */
static void get_country_code(char *cc, size_t cc_size)
{
    char output[256] = {0};

    /* Try iw reg get — works on most distros */
    if (net_exec_cmd("iw reg get 2>/dev/null | grep -oP 'country \\K[A-Z]{2}' | head -1",
                     output, sizeof(output))) {
        /* Trim whitespace/newlines */
        char *p = output;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ') p++;
        *p = '\0';
        if (strlen(output) == 2) {
            strncpy(cc, output, cc_size - 1);
            return;
        }
    }

    /* Fallback: try locale-based detection */
    output[0] = '\0';
    if (net_exec_cmd("locale | grep -oP 'LC_ALL=.*?\\K[A-Z]{2}' 2>/dev/null | head -1",
                     output, sizeof(output))) {
        char *p = output;
        while (*p && *p != '\n') p++;
        *p = '\0';
        if (strlen(output) == 2) {
            strncpy(cc, output, cc_size - 1);
            return;
        }
    }

    /* Default fallback */
    strncpy(cc, "US", cc_size - 1);
}

static bool generate_hostapd_conf(const HotspotStatus *status, bool minimal)
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

    /* Detect country code from system regulatory domain */
    char country[4] = {0};
    get_country_code(country, sizeof(country));

    /*
     * Core hostapd config — kept minimal for maximum driver compatibility.
     * On some adapters (especially with AP/STA concurrency), advanced
     * features like 802.11n/ac can cause initialization failures.
     */
    fprintf(fp,
        "interface=%s\n"
        "driver=nl80211\n"
        "ssid=%s\n"
        "hw_mode=%s\n"
        "channel=%d\n"
        "country_code=%s\n"
        "ieee80211d=1\n"
        "wmm_enabled=%d\n"
        "macaddr_acl=0\n"
        "auth_algs=1\n"
        "ignore_broadcast_ssid=%d\n"
        "wpa=2\n"
        "wpa_passphrase=%s\n"
        "wpa_key_mgmt=WPA-PSK\n"
        "rsn_pairwise=CCMP\n",
        status->ap_iface,
        status->config.ssid,
        hw_mode,
        channel,
        country,
        use_5ghz ? 1 : 0,   /* WMM mandatory for 5GHz */
        status->config.hidden ? 1 : 0,
        status->config.password
    );

    /* Only add 802.11n/ac if NOT in minimal fallback mode */
    if (!minimal) {
        fprintf(fp, "ieee80211n=1\n");
        if (use_5ghz) {
            fprintf(fp, "ieee80211ac=1\n");
        }
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
 * Aggressively remove a stale AP interface.
 * Returns true if the interface no longer exists.
 */
static bool force_remove_interface(const char *iface)
{
    char cmd[MAX_CMD_LEN];
    char output[64] = {0};

    /* Check if interface exists */
    snprintf(cmd, sizeof(cmd), "ip link show %s 2>/dev/null", iface);
    if (!net_exec_cmd(cmd, output, sizeof(output)) || strlen(output) == 0)
        return true;  /* Already gone */

    /* Step 1: Bring it DOWN */
    snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", iface);
    net_exec_silent(cmd);

    /* Step 2: Flush addresses */
    snprintf(cmd, sizeof(cmd), "ip addr flush dev %s 2>/dev/null", iface);
    net_exec_silent(cmd);

    /* Step 3: Detach wpa_supplicant from it */
    snprintf(cmd, sizeof(cmd), "wpa_cli -i %s terminate 2>/dev/null", iface);
    net_exec_silent(cmd);
    snprintf(cmd, sizeof(cmd),
             "rm -f /var/run/wpa_supplicant/%s /run/wpa_supplicant/%s 2>/dev/null",
             iface, iface);
    net_exec_silent(cmd);

    /* Step 4: Tell NM to release it */
    snprintf(cmd, sizeof(cmd), "nmcli device set %s managed no 2>/dev/null", iface);
    net_exec_silent(cmd);
    usleep(300000);

    /* Step 5: Delete via iw */
    snprintf(cmd, sizeof(cmd), "iw dev %s del 2>/dev/null", iface);
    net_exec_silent(cmd);
    usleep(500000);

    /* Verify it's gone */
    output[0] = '\0';
    snprintf(cmd, sizeof(cmd), "ip link show %s 2>/dev/null", iface);
    if (net_exec_cmd(cmd, output, sizeof(output)) && strlen(output) > 0)
        return false;  /* Still exists */

    return true;
}

/*
 * Creates the virtual AP interface and tells NM to ignore it.
 * Does NOT bring it up or assign IP — hostapd handles that.
 */
static bool create_ap_interface(HotspotStatus *status)
{
    char cmd[MAX_CMD_LEN];

    /* Stop conflicting services */
    net_exec_silent("pkill hostapd 2>/dev/null");
    net_exec_silent("pkill -f 'dnsmasq.*hotspot_enabler' 2>/dev/null");
    net_exec_silent("rfkill unblock wifi 2>/dev/null");
    usleep(300000);

    /*
     * Try to create an AP interface. If the default name (ap0) is
     * stuck from a previous run, try ap1, ap2, ap3.
     */
    const char *ap_names[] = { "ap0", "ap1", "ap2", "ap3" };
    int num_names = 4;
    bool created = false;

    for (int i = 0; i < num_names; i++) {
        const char *try_name = ap_names[i];

        /* Try to remove any stale interface with this name */
        force_remove_interface(try_name);

        /* Pre-configure NM to ignore this interface BEFORE creating it */
        FILE *fp = fopen(NM_UNMANAGED_CONF, "w");
        if (fp) {
            fprintf(fp,
                "[keyfile]\n"
                "unmanaged-devices=interface-name:%s\n",
                try_name);
            fclose(fp);
            net_exec_silent("nmcli general reload conf 2>/dev/null");
            usleep(300000);
        }

        /* Create virtual interface */
        snprintf(cmd, sizeof(cmd),
                 "iw dev %s interface add %s type __ap",
                 status->wifi.name, try_name);

        if (net_exec_silent(cmd) == 0) {
            /* Success! Update the interface name in status */
            strncpy(status->ap_iface, try_name, MAX_IFACE_NAME - 1);
            created = true;
            break;
        }
    }

    if (!created) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "Failed to create virtual AP interface. "
                 "All names (ap0-ap3) are in use or your driver "
                 "doesn't support AP/STA concurrency.");
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

/* ── Prepare AP interface for hostapd ────────────────────────────────── */

/*
 * On many distros, wpa_supplicant or iwd may attach to the ap0
 * interface before hostapd can. hostapd requires exclusive nl80211
 * access AND the interface to be DOWN.
 *
 * IMPORTANT: We must NOT use "pkill -f wpa_supplicant" because on
 * Arch/Manjaro/etc, a single wpa_supplicant process manages ALL
 * interfaces. Killing it would drop the WiFi client connection.
 * Instead, we use wpa_cli to detach only the ap0 interface.
 */
static void prepare_for_hostapd(const char *ap_iface)
{
    char cmd[MAX_CMD_LEN];

    /* Safely detach wpa_supplicant from ap0 only (not kill it) */
    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i %s disconnect 2>/dev/null", ap_iface);
    net_exec_silent(cmd);
    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i %s terminate 2>/dev/null", ap_iface);
    net_exec_silent(cmd);

    /* Remove wpa_supplicant control socket for ap0 if it exists */
    snprintf(cmd, sizeof(cmd),
             "rm -f /var/run/wpa_supplicant/%s 2>/dev/null", ap_iface);
    net_exec_silent(cmd);
    snprintf(cmd, sizeof(cmd),
             "rm -f /run/wpa_supplicant/%s 2>/dev/null", ap_iface);
    net_exec_silent(cmd);

    /* Re-tell NM to unmanage (it may have re-grabbed after creation) */
    snprintf(cmd, sizeof(cmd),
             "nmcli device set %s managed no 2>/dev/null", ap_iface);
    net_exec_silent(cmd);

    /* Bring interface DOWN — hostapd needs it DOWN to initialize */
    snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", ap_iface);
    net_exec_silent(cmd);

    /* Wait for interface to be fully released */
    usleep(500000); /* 0.5 seconds */
}

/* ── Start hostapd ───────────────────────────────────────────────────── */

/*
 * Attempt a single hostapd start. Returns true if hostapd is running.
 */
static bool try_hostapd_once(HotspotStatus *status, char *log_out, size_t log_sz)
{
    char cmd[MAX_CMD_LEN];

    prepare_for_hostapd(status->ap_iface);

    snprintf(cmd, sizeof(cmd),
             "hostapd -B %s -f %s >/dev/null 2>&1",
             HOSTAPD_CONF_PATH, HOSTAPD_LOG_PATH);

    if (net_exec_silent(cmd) == 0) {
        usleep(1000000); /* 1 second for init */

        char output[64] = {0};
        net_exec_cmd("pidof hostapd", output, sizeof(output));
        status->hostapd_pid = atoi(output);

        if (status->hostapd_pid > 0)
            return true;
    }

    /* Failed — capture error lines from log */
    log_out[0] = '\0';
    snprintf(cmd, sizeof(cmd),
             "grep -iE 'Could not|FAIL|Error|refused' %s 2>/dev/null | tail -2",
             HOSTAPD_LOG_PATH);
    net_exec_cmd(cmd, log_out, log_sz);

    if (strlen(log_out) == 0) {
        snprintf(cmd, sizeof(cmd), "tail -2 %s 2>/dev/null", HOSTAPD_LOG_PATH);
        net_exec_cmd(cmd, log_out, log_sz);
    }

    net_exec_silent("pkill hostapd 2>/dev/null");
    usleep(500000);
    return false;
}

static bool start_hostapd(HotspotStatus *status)
{
    char cmd[MAX_CMD_LEN];
    char log_output[MAX_CMD_LEN] = {0};
    int client_channel = status->wifi.channel;
    bool is_5ghz = is_5ghz_channel(client_channel);

    /* Set regulatory domain before starting hostapd */
    char country[4] = {0};
    get_country_code(country, sizeof(country));
    snprintf(cmd, sizeof(cmd), "iw reg set %s 2>/dev/null", country);
    net_exec_silent(cmd);
    usleep(200000);

    /*
     * Three-phase startup strategy:
     *
     *   Phase 1: Full config (802.11n/ac) on client's channel
     *   Phase 2: Minimal config (basic) on client's channel
     *   Phase 3: Fallback to 2.4 GHz channel 6
     *            (only if client is on 5GHz and driver blocks AP on 5GHz)
     *
     * The "Could not select hw_mode and channel" error (-3) means
     * the driver/regulatory domain blocks AP on the requested channel.
     * When detected, we skip directly to Phase 3.
     */

    /* ── Phase 1: Full config on client's channel ──────────────────── */
    generate_hostapd_conf(status, false);
    if (try_hostapd_once(status, log_output, sizeof(log_output)))
        return true;

    /* Check if it's a channel/hw_mode rejection — skip to 2.4GHz */
    bool channel_rejected = (strstr(log_output, "Could not select") != NULL);

    if (!channel_rejected) {
        /* ── Phase 2: Minimal config on client's channel ───────────── */
        generate_hostapd_conf(status, true);
        if (try_hostapd_once(status, log_output, sizeof(log_output)))
            return true;

        channel_rejected = (strstr(log_output, "Could not select") != NULL);
    }

    /* ── Phase 3: 2.4GHz fallback (only if 5GHz was rejected) ──────── */
    if (channel_rejected && is_5ghz) {
        /* Override channel to 2.4GHz channel 6 */
        int saved_channel = status->config.channel;
        status->config.channel = 6;

        /* Try full config on 2.4GHz */
        generate_hostapd_conf(status, false);
        if (try_hostapd_once(status, log_output, sizeof(log_output))) {
            status->config.channel = saved_channel;
            return true;
        }

        /* Try minimal config on 2.4GHz */
        generate_hostapd_conf(status, true);
        if (try_hostapd_once(status, log_output, sizeof(log_output))) {
            status->config.channel = saved_channel;
            return true;
        }

        status->config.channel = saved_channel;
    }

    /* All attempts failed — set detailed error message */
    for (char *p = log_output; *p; p++) {
        if (*p == '\n') *p = ' ';
    }

    if (channel_rejected && is_5ghz) {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "AP not supported on 5GHz (ch %d) or 2.4GHz by this driver. "
                 "Try connecting to a 2.4GHz WiFi network first.",
                 client_channel);
    } else {
        snprintf(status->error_msg, sizeof(status->error_msg),
                 "hostapd failed: %.400s", log_output);
    }
    return false;
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
    if (!generate_hostapd_conf(status, false)) {
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
