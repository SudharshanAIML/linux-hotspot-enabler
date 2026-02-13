/*
 * hotspot.h - Core hotspot management API for Linux Hotspot Enabler
 *
 * Manages the virtual AP interface, hostapd, dnsmasq, and iptables NAT
 * to provide simultaneous WiFi client + AP hotspot.
 */

#ifndef HOTSPOT_H
#define HOTSPOT_H

#include <stdbool.h>
#include <time.h>
#include "net_utils.h"

#define AP_IFACE_NAME     "ap0"
#define AP_SUBNET         "192.168.12"
#define AP_GATEWAY        "192.168.12.1"
#define AP_NETMASK        "255.255.255.0"
#define AP_DHCP_START     "192.168.12.10"
#define AP_DHCP_END       "192.168.12.254"

#define HOSTAPD_CONF_PATH "/tmp/hotspot_enabler_hostapd.conf"
#define DNSMASQ_CONF_PATH "/tmp/hotspot_enabler_dnsmasq.conf"
#define DNSMASQ_LEASE_FILE "/tmp/hotspot_enabler_dnsmasq.leases"
#define HOSTAPD_LOG_PATH  "/tmp/hotspot_enabler_hostapd.log"

/* ── Hotspot Configuration ───────────────────────────────────────────── */

typedef struct {
    char ssid[MAX_SSID_LEN];
    char password[MAX_SSID_LEN];
    int  channel;           /* 0 = auto (match client) */
    bool use_5ghz;          /* false = 2.4GHz, true = 5GHz */
    int  max_clients;
    bool hidden;
} HotspotConfig;

/* ── Hotspot Runtime State ───────────────────────────────────────────── */

typedef enum {
    HS_STATE_STOPPED,
    HS_STATE_STARTING,
    HS_STATE_RUNNING,
    HS_STATE_ERROR,
    HS_STATE_STOPPING
} HotspotState;

typedef struct {
    HotspotState    state;
    HotspotConfig   config;
    WifiInterface   wifi;           /* Client WiFi info */
    char            ap_iface[MAX_IFACE_NAME];
    char            phy[MAX_IFACE_NAME];
    int             client_count;
    ConnectedClient clients[MAX_CLIENTS];
    time_t          start_time;
    char            error_msg[MAX_CMD_LEN];
    pid_t           hostapd_pid;
    pid_t           dnsmasq_pid;
    bool            ip_forward_was_enabled;
} HotspotStatus;

/* ── Functions ───────────────────────────────────────────────────────── */

/* Initialize with default configuration */
void hotspot_init(HotspotStatus *status);

/* Set default config values */
void hotspot_default_config(HotspotConfig *config);

/* Start the hotspot — creates AP interface, launches hostapd + dnsmasq */
bool hotspot_start(HotspotStatus *status);

/* Stop the hotspot — kills processes, removes interface, cleans up */
bool hotspot_stop(HotspotStatus *status);

/* Refresh status — update client list, check processes alive */
void hotspot_refresh_status(HotspotStatus *status);

/* Get uptime string (e.g., "1h 23m 45s") */
void hotspot_get_uptime_str(const HotspotStatus *status, char *buf, size_t bufsize);

/* Clean up everything (called on exit/signal) */
void hotspot_cleanup(HotspotStatus *status);

#endif /* HOTSPOT_H */
