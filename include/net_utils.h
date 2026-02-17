/*
 * net_utils.h - Network utility functions for Linux Hotspot Enabler
 *
 * Provides interface detection, dependency checking, and distro-aware
 * package management helpers.
 */

#ifndef NET_UTILS_H
#define NET_UTILS_H

#include <stdbool.h>

#define MAX_IFACE_NAME    32
#define MAX_SSID_LEN      64
#define MAX_PATH_LEN      256
#define MAX_IP_LEN        46
#define MAX_MAC_LEN       18
#define MAX_CMD_LEN       512
#define MAX_LINE_LEN      256
#define MAX_CLIENTS        64

/* ── Dependency Info ─────────────────────────────────────────────────── */

typedef struct {
    bool has_iw;
    bool has_hostapd;
    bool has_dnsmasq;
    bool has_iptables;
    bool all_present;
} DependencyStatus;

/* ── WiFi Interface Info ─────────────────────────────────────────────── */

typedef struct {
    char name[MAX_IFACE_NAME];
    char ssid[MAX_SSID_LEN];
    char ip[MAX_IP_LEN];
    char mac[MAX_MAC_LEN];
    int  channel;
    int  signal_dbm;
    bool connected;
    bool supports_ap;
} WifiInterface;

/* ── Connected Client Info ───────────────────────────────────────────── */

typedef struct {
    char mac[MAX_MAC_LEN];
    char ip[MAX_IP_LEN];
    char hostname[MAX_SSID_LEN];
} ConnectedClient;

/* ── Distro Info ─────────────────────────────────────────────────────── */

typedef enum {
    DISTRO_DEBIAN,    /* Ubuntu, Zorin, Mint, etc. */
    DISTRO_ARCH,      /* Arch, Manjaro, EndeavourOS */
    DISTRO_FEDORA,    /* Fedora, RHEL, CentOS */
    DISTRO_OPENSUSE,
    DISTRO_VOID,
    DISTRO_UNKNOWN
} DistroFamily;

typedef struct {
    DistroFamily family;
    char name[64];
    char install_cmd[MAX_CMD_LEN];
} DistroInfo;

/* ── Functions ───────────────────────────────────────────────────────── */

/* Check if all required tools are installed */
DependencyStatus net_check_dependencies(void);

/* Get the install command string for missing dependencies */
void net_get_install_command(char *buf, size_t bufsize);

/* Detect the Linux distro family */
DistroInfo net_detect_distro(void);

/* Find the primary WiFi interface */
bool net_detect_wifi_interface(WifiInterface *iface);

/* Refresh WiFi interface status (SSID, IP, signal, channel) */
bool net_refresh_wifi_status(WifiInterface *iface);

/* Check if interface supports AP/STA concurrency */
bool net_check_ap_support(const char *phy);

/* Check if interface supports simultaneous AP+Station mode */
bool net_check_ap_sta_concurrency(const char *phy);

/* Get the phy device name for an interface */
bool net_get_phy_name(const char *iface, char *phy, size_t physize);

/* Get the current channel of the WiFi interface */
int net_get_current_channel(const char *iface);

/* Get connected clients from DHCP leases */
int net_get_connected_clients(ConnectedClient *clients, int max_clients);

/* Execute a command and capture output */
bool net_exec_cmd(const char *cmd, char *output, size_t output_size);

/* Execute a command silently (no output capture) */
int net_exec_silent(const char *cmd);

#endif /* NET_UTILS_H */
