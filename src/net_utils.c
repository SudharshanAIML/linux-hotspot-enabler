/*
 * net_utils.c - Network utility functions for Linux Hotspot Enabler
 *
 * Interface detection, dependency checks, distro detection, and
 * helper functions for network operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include "net_utils.h"
#include "hotspot.h"

/* ── Helper: Execute command and capture output ──────────────────────── */

bool net_exec_cmd(const char *cmd, char *output, size_t output_size)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;

    size_t total = 0;
    char buf[256];
    output[0] = '\0';

    while (fgets(buf, sizeof(buf), fp)) {
        size_t len = strlen(buf);
        if (total + len < output_size - 1) {
            strcat(output, buf);
            total += len;
        }
    }

    int status = pclose(fp);
    return (status == 0);
}

int net_exec_silent(const char *cmd)
{
    return system(cmd);
}

/* ── Dependency Checking ─────────────────────────────────────────────── */

static bool check_tool(const char *name)
{
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "which %s >/dev/null 2>&1", name);
    return (system(cmd) == 0);
}

DependencyStatus net_check_dependencies(void)
{
    DependencyStatus ds;
    ds.has_iw       = check_tool("iw");
    ds.has_hostapd  = check_tool("hostapd");
    ds.has_dnsmasq  = check_tool("dnsmasq");
    ds.has_iptables = check_tool("iptables");
    ds.all_present  = ds.has_iw && ds.has_hostapd &&
                      ds.has_dnsmasq && ds.has_iptables;
    return ds;
}

/* ── Distro Detection ────────────────────────────────────────────────── */

DistroInfo net_detect_distro(void)
{
    DistroInfo info;
    info.family = DISTRO_UNKNOWN;
    snprintf(info.name, sizeof(info.name), "Unknown");
    snprintf(info.install_cmd, sizeof(info.install_cmd), "# Unknown distro");

    char line[MAX_LINE_LEN];
    char id[64] = {0};
    char id_like[128] = {0};
    char pretty_name[128] = {0};

    FILE *fp = fopen("/etc/os-release", "r");
    if (!fp) return info;

    while (fgets(line, sizeof(line), fp)) {
        /* Remove trailing newline */
        line[strcspn(line, "\n")] = '\0';

        if (strncmp(line, "ID=", 3) == 0) {
            strncpy(id, line + 3, sizeof(id) - 1);
            /* Strip quotes */
            char *p = id;
            if (*p == '"') { p++; }
            size_t len = strlen(p);
            if (len > 0 && p[len-1] == '"') p[len-1] = '\0';
            memmove(id, p, strlen(p) + 1);
        }
        else if (strncmp(line, "ID_LIKE=", 8) == 0) {
            strncpy(id_like, line + 8, sizeof(id_like) - 1);
            char *p = id_like;
            if (*p == '"') { p++; }
            size_t len = strlen(p);
            if (len > 0 && p[len-1] == '"') p[len-1] = '\0';
            memmove(id_like, p, strlen(p) + 1);
        }
        else if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
            strncpy(pretty_name, line + 12, sizeof(pretty_name) - 1);
            char *p = pretty_name;
            if (*p == '"') { p++; }
            size_t len = strlen(p);
            if (len > 0 && p[len-1] == '"') p[len-1] = '\0';
            memmove(pretty_name, p, strlen(p) + 1);
        }
    }
    fclose(fp);

    if (strlen(pretty_name) > 0) {
        strncpy(info.name, pretty_name, sizeof(info.name) - 1);
    }

    /* Determine family */
    const char *packages = "iw hostapd dnsmasq iptables";

    if (strcmp(id, "ubuntu") == 0 || strcmp(id, "debian") == 0 ||
        strcmp(id, "zorin") == 0  || strcmp(id, "linuxmint") == 0 ||
        strcmp(id, "pop") == 0    || strcmp(id, "elementary") == 0 ||
        strstr(id_like, "ubuntu") || strstr(id_like, "debian")) {
        info.family = DISTRO_DEBIAN;
        snprintf(info.install_cmd, sizeof(info.install_cmd),
                 "sudo apt update && sudo apt install -y %s", packages);
    }
    else if (strcmp(id, "arch") == 0 || strcmp(id, "manjaro") == 0 ||
             strcmp(id, "endeavouros") == 0 ||
             strstr(id_like, "arch")) {
        info.family = DISTRO_ARCH;
        snprintf(info.install_cmd, sizeof(info.install_cmd),
                 "sudo pacman -Sy --noconfirm %s", packages);
    }
    else if (strcmp(id, "fedora") == 0 || strcmp(id, "rhel") == 0 ||
             strcmp(id, "centos") == 0 || strcmp(id, "rocky") == 0 ||
             strstr(id_like, "fedora") || strstr(id_like, "rhel")) {
        info.family = DISTRO_FEDORA;
        snprintf(info.install_cmd, sizeof(info.install_cmd),
                 "sudo dnf install -y %s", packages);
    }
    else if (strcmp(id, "opensuse-leap") == 0 ||
             strcmp(id, "opensuse-tumbleweed") == 0 ||
             strstr(id_like, "suse")) {
        info.family = DISTRO_OPENSUSE;
        snprintf(info.install_cmd, sizeof(info.install_cmd),
                 "sudo zypper install -y %s", packages);
    }
    else if (strcmp(id, "void") == 0) {
        info.family = DISTRO_VOID;
        snprintf(info.install_cmd, sizeof(info.install_cmd),
                 "sudo xbps-install -Sy %s", packages);
    }
    else {
        snprintf(info.install_cmd, sizeof(info.install_cmd),
                 "# Install manually: %s", packages);
    }

    return info;
}

void net_get_install_command(char *buf, size_t bufsize)
{
    DistroInfo di = net_detect_distro();
    strncpy(buf, di.install_cmd, bufsize - 1);
    buf[bufsize - 1] = '\0';
}

/* ── WiFi Interface Detection ────────────────────────────────────────── */

bool net_detect_wifi_interface(WifiInterface *iface)
{
    memset(iface, 0, sizeof(WifiInterface));

    DIR *dir = opendir("/sys/class/net");
    if (!dir) return false;

    struct dirent *entry;
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Check if it's a wireless interface */
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "/sys/class/net/%.200s/wireless", entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0) {
            /* Skip our own AP interface */
            if (strcmp(entry->d_name, "ap0") == 0) continue;

            strncpy(iface->name, entry->d_name, MAX_IFACE_NAME - 1);
            found = true;
            break;
        }
    }
    closedir(dir);

    if (found) {
        net_refresh_wifi_status(iface);

        /* Check AP support */
        char phy[MAX_IFACE_NAME];
        if (net_get_phy_name(iface->name, phy, sizeof(phy))) {
            iface->supports_ap = net_check_ap_support(phy);
        }
    }

    return found;
}

/* ── Refresh WiFi Status ─────────────────────────────────────────────── */

bool net_refresh_wifi_status(WifiInterface *iface)
{
    char cmd[MAX_CMD_LEN];
    char output[4096];

    /* Get SSID */
    snprintf(cmd, sizeof(cmd), "iw dev %s link 2>/dev/null", iface->name);
    if (net_exec_cmd(cmd, output, sizeof(output))) {
        char *ssid_line = strstr(output, "SSID:");
        if (ssid_line) {
            ssid_line += 5;
            while (*ssid_line == ' ') ssid_line++;
            char *nl = strchr(ssid_line, '\n');
            if (nl) *nl = '\0';
            strncpy(iface->ssid, ssid_line, MAX_SSID_LEN - 1);
            iface->connected = true;
        } else {
            iface->connected = false;
            iface->ssid[0] = '\0';
        }

        /* Get signal */
        char *sig_line = strstr(output, "signal:");
        if (sig_line) {
            iface->signal_dbm = atoi(sig_line + 7);
        }
    }

    /* Get IP address */
    snprintf(cmd, sizeof(cmd),
             "ip -4 addr show %s 2>/dev/null | grep -oP 'inet \\K[\\d.]+'",
             iface->name);
    if (net_exec_cmd(cmd, output, sizeof(output))) {
        char *nl = strchr(output, '\n');
        if (nl) *nl = '\0';
        if (strlen(output) > 0) {
            strncpy(iface->ip, output, MAX_IP_LEN - 1);
        }
    }

    /* Get MAC address */
    snprintf(cmd, sizeof(cmd),
             "cat /sys/class/net/%s/address 2>/dev/null", iface->name);
    if (net_exec_cmd(cmd, output, sizeof(output))) {
        char *nl = strchr(output, '\n');
        if (nl) *nl = '\0';
        strncpy(iface->mac, output, MAX_MAC_LEN - 1);
    }

    /* Get channel */
    iface->channel = net_get_current_channel(iface->name);

    return iface->connected;
}

/* ── PHY Name ────────────────────────────────────────────────────────── */

bool net_get_phy_name(const char *iface, char *phy, size_t physize)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211/name", iface);

    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    if (fgets(phy, physize, fp)) {
        phy[strcspn(phy, "\n")] = '\0';
        fclose(fp);
        return true;
    }
    fclose(fp);
    return false;
}

/* ── AP/STA Concurrency Check ────────────────────────────────────────── */

bool net_check_ap_support(const char *phy)
{
    char cmd[MAX_CMD_LEN];
    char output[4096];

    /*
     * Method 1: Check "valid interface combinations" for both managed
     * and AP in the same combination block. Use grep -A to grab lines
     * after the header (avoids sed tab-matching issues).
     */
    snprintf(cmd, sizeof(cmd),
             "iw phy %s info 2>/dev/null "
             "| grep -A 8 'valid interface combinations:'",
             phy);
    output[0] = '\0';
    net_exec_cmd(cmd, output, sizeof(output));
    if (strlen(output) > 0) {
        bool has_managed = (strstr(output, "managed") != NULL);
        bool has_ap      = (strstr(output, "AP") != NULL);
        if (has_managed && has_ap) return true;
    }

    /*
     * Method 2: Fallback — check "Supported interface modes" for both
     * managed and AP. Most modern drivers listing both do support it.
     */
    snprintf(cmd, sizeof(cmd),
             "iw phy %s info 2>/dev/null "
             "| grep -A 10 'Supported interface modes:'",
             phy);
    output[0] = '\0';
    net_exec_cmd(cmd, output, sizeof(output));
    if (strlen(output) > 0) {
        bool has_managed = (strstr(output, "managed") != NULL);
        bool has_ap      = (strstr(output, "AP") != NULL);
        if (has_managed && has_ap) return true;
    }

    return false;
}

/* ── Get Current Channel ─────────────────────────────────────────────── */

int net_get_current_channel(const char *iface)
{
    char cmd[MAX_CMD_LEN];
    char output[2048];

    snprintf(cmd, sizeof(cmd), "iw dev %s info 2>/dev/null", iface);
    if (!net_exec_cmd(cmd, output, sizeof(output))) return 0;

    char *ch_line = strstr(output, "channel ");
    if (ch_line) {
        return atoi(ch_line + 8);
    }
    return 0;
}

/* ── Connected Clients ───────────────────────────────────────────────── */

int net_get_connected_clients(ConnectedClient *clients, int max_clients)
{
    int count = 0;

    /* Read dnsmasq lease file */
    FILE *fp = fopen(DNSMASQ_LEASE_FILE, "r");
    if (!fp) return 0;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), fp) && count < max_clients) {
        /* Format: timestamp mac ip hostname clientid */
        char ts[32], mac[MAX_MAC_LEN], ip[MAX_IP_LEN], hostname[MAX_SSID_LEN];
        if (sscanf(line, "%31s %17s %45s %63s", ts, mac, ip, hostname) >= 3) {
            strncpy(clients[count].mac, mac, MAX_MAC_LEN - 1);
            strncpy(clients[count].ip, ip, MAX_IP_LEN - 1);
            if (hostname[0] == '*') {
                strncpy(clients[count].hostname, "(unknown)", MAX_SSID_LEN - 1);
            } else {
                strncpy(clients[count].hostname, hostname, MAX_SSID_LEN - 1);
            }
            count++;
        }
    }
    fclose(fp);

    return count;
}
