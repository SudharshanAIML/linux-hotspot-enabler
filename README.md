<div align="center">

# 🌐 Linux Hotspot Enabler

**Simultaneous WiFi + Hotspot on Linux — without disconnecting**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Language: C](https://img.shields.io/badge/Language-C11-orange.svg)]()
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)]()

_A lightweight C tool with a responsive ncurses TUI that creates a virtual AP interface alongside your existing WiFi connection, enabling internet sharing while staying connected._

</div>

---

## 🎯 The Problem

On most Linux distributions (including Zorin, Ubuntu, Mint), enabling a Wi-Fi hotspot **disconnects you from your current WiFi network**. This is because the built-in hotspot feature takes exclusive control of the wireless adapter.

**Linux Hotspot Enabler** solves this by leveraging **AP/STA concurrency** — creating a virtual access point (`ap0`) on the same radio as your WiFi client connection, so both run simultaneously.

---

## ✨ Features

| Feature                            | Description                                                 |
| ---------------------------------- | ----------------------------------------------------------- |
| 📡 **Simultaneous WiFi + Hotspot** | Stay connected to WiFi while sharing your internet          |
| 🖥️ **Responsive TUI**              | Beautiful ncurses terminal interface with 4 screens         |
| 🔧 **Auto-Detection**              | Automatically finds WiFi interface, channel & AP support    |
| 🌍 **Cross-Distro**                | Ubuntu, Zorin, Debian, Mint, Arch, Fedora, RHEL & more      |
| 👥 **Live Client Monitoring**      | See connected devices with IP, MAC, and hostname            |
| 🔐 **WPA2 Security**               | Hotspot is password-protected with WPA2/CCMP                |
| 🧹 **Clean Shutdown**              | Properly removes virtual interfaces, NAT rules & temp files |
| ⚙️ **Configurable**                | Edit SSID, password, channel, 5GHz mode, hidden network     |

---

## 📸 Screenshots

### Dashboard

```
┌─ WiFi Client ───────────────────────┐ ┌─ Hotspot ────────────────────────────┐
│ Status:      Connected              │ │ Status:      RUNNING                 │
│ SSID:        MyNetwork              │ │ SSID:        LinuxHotspot            │
│ IP:          192.168.1.42           │ │ Interface:   ap0                     │
│ Channel:     6                      │ │ Clients:     2                       │
│ Signal:      -45 dBm                │ │ Uptime:      1h 23m 15s             │
└─────────────────────────────────────┘ └──────────────────────────────────────┘
```

### Configuration

```
┌─ Hotspot Configuration ─────────────────────────────────────────────────────┐
│ SSID:           LinuxHotspot                                                │
│ Password:       ••••••••••••                                                │
│ Channel:        Auto (match client)                                         │
│ Band:           2.4 GHz                                                     │
│ Hidden:         No                                                          │
│ Max Clients:    10                                                          │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 📦 Dependencies

| Tool           | Purpose                                 |
| -------------- | --------------------------------------- |
| `iw`           | Wireless interface management           |
| `hostapd`      | Access Point daemon                     |
| `dnsmasq`      | DHCP & DNS server for hotspot clients   |
| `iptables`     | NAT/firewall rules for internet sharing |
| `ncurses`      | Terminal UI library (dev headers)       |
| `gcc` / `make` | Build toolchain                         |

### Install Dependencies

<details>
<summary><b>🟠 Ubuntu / Zorin / Debian / Linux Mint</b></summary>

```bash
sudo apt update && sudo apt install -y iw hostapd dnsmasq iptables libncurses-dev build-essential
```

</details>

<details>
<summary><b>🔵 Arch Linux / Manjaro</b></summary>

```bash
sudo pacman -Sy --noconfirm iw hostapd dnsmasq iptables ncurses base-devel
```

</details>

<details>
<summary><b>🔴 Fedora / RHEL / CentOS</b></summary>

```bash
sudo dnf install -y iw hostapd dnsmasq iptables ncurses-devel gcc make
```

</details>

<details>
<summary><b>🟢 openSUSE</b></summary>

```bash
sudo zypper install -y iw hostapd dnsmasq iptables ncurses-devel gcc make
```

</details>

---

## 🔨 Build & Install

```bash
# Clone the repository
git clone https://github.com/SudharshanAIML/linux-hotspot-enabler.git
cd linux-hotspot-enabler

# Build
make

# (Optional) Install system-wide
sudo make install
```

To uninstall:

```bash
sudo make uninstall
```

---

## 🚀 Usage

```bash
sudo ./hotspot-enabler
```

> **Note:** Root privileges are required to manage network interfaces, hostapd, dnsmasq, and iptables rules.

### TUI Keyboard Shortcuts

| Key       | Action                                               |
| --------- | ---------------------------------------------------- |
| `F1`      | **Dashboard** — WiFi & Hotspot status overview       |
| `F2`      | **Config** — Edit SSID, password, channel, band      |
| `F3`      | **Clients** — View connected devices                 |
| `F4`      | **Log** — Event and error log                        |
| `Tab`     | Cycle through screens                                |
| `Enter`   | Start/Stop hotspot (Dashboard) · Edit field (Config) |
| `↑` / `↓` | Navigate fields or scroll logs                       |
| `q`       | Quit (with clean shutdown)                           |

---

## ⚙️ How It Works

```
┌──────────────┐         ┌──────────────┐
│   Internet   │◄────────│   Router     │
└──────┬───────┘         └──────┬───────┘
       │                        │ WiFi (e.g. channel 6)
       │                 ┌──────┴───────┐
       │                 │   wlo1       │ ← Your WiFi client
       │                 │  (managed)   │
       │                 ├──────────────┤
       │    iptables NAT │   ap0        │ ← Virtual AP interface
       │◄────────────────│  (__ap)      │
       │                 └──────┬───────┘
       │                        │ Hotspot (same channel)
       │                 ┌──────┴───────┐
       │                 │   Clients    │
       │                 │  Phone, Tab  │
       │                 └──────────────┘
```

### Startup Sequence

1. **Detect** your active WiFi interface and verify AP/STA concurrency support
2. **Create** a virtual AP interface (`ap0`) on the same physical radio
3. **Configure** NetworkManager to ignore the AP interface
4. **Launch** `hostapd` to broadcast your hotspot SSID (WPA2 secured)
5. **Assign** IP address to `ap0` and configure the gateway
6. **Launch** `dnsmasq` to provide DHCP/DNS to connected clients
7. **Configure** `iptables` NAT to forward traffic: hotspot → WiFi → internet
8. **Monitor** connections and provide live status via the TUI

### Shutdown Sequence

1. Stop `hostapd` and `dnsmasq` processes
2. Remove `iptables` NAT rules
3. Delete the virtual `ap0` interface
4. Restore NetworkManager configuration
5. Clean up all temporary config files

---

## 🏗️ Project Structure

```
linux-hotspot-enabler/
├── include/
│   ├── hotspot.h          # Hotspot config, status structs & API
│   ├── net_utils.h        # Network utility structs & functions
│   └── tui.h              # TUI state, screens & rendering
├── src/
│   ├── main.c             # Entry point, root check, dependency verify
│   ├── hotspot.c          # Core hotspot management (hostapd, dnsmasq, NAT)
│   ├── net_utils.c        # Interface detection, AP support, client listing
│   └── tui.c              # ncurses TUI (dashboard, config, clients, log)
├── Makefile               # Build system
├── .gitignore
├── LICENSE
└── README.md
```

---

## ⚠️ Requirements

### Hardware

Your WiFi adapter must support **AP/STA concurrency** (most modern adapters do). Verify with:

```bash
iw list | grep -A 8 "valid interface combinations"
```

Look for output containing both `managed` and `AP`:

```
valid interface combinations:
    * #{ managed } <= 1, #{ AP, P2P-client, P2P-GO } <= 1,
      total <= 3, #channels <= 2
```

### Software

- Linux kernel 4.0+ (for proper nl80211 AP/STA support)
- Root/sudo access
- Active WiFi connection (to share internet from)

---

## 🔧 Troubleshooting

<details>
<summary><b>❌ "WiFi adapter does not support AP+STA concurrency"</b></summary>

Your adapter may not support running AP and client mode simultaneously. Try:

```bash
iw list | grep -A 8 "valid interface combinations"
```

If the output does not show both `managed` and `AP`, your adapter's driver doesn't support this feature.

**Workaround:** Use a second USB WiFi adapter dedicated to the hotspot.

</details>

<details>
<summary><b>❌ "Failed to create virtual AP interface"</b></summary>

- Ensure your WiFi driver is loaded: `lsmod | grep <your_driver>`
- Check if rfkill is blocking: `rfkill list` → `sudo rfkill unblock wifi`
- Try unloading and reloading your WiFi driver
</details>

<details>
<summary><b>❌ "hostapd failed"</b></summary>

- Check the hostapd log: `cat /tmp/hotspot_enabler_hostapd.log`
- Ensure `hostapd` isn't already running: `sudo pkill hostapd`
- Your adapter may not support the selected channel — try a different one in Config (F2)
</details>

<details>
<summary><b>❌ "Failed to start dnsmasq"</b></summary>

Another DNS service may be using port 53:

```bash
sudo systemctl stop systemd-resolved
sudo systemctl stop dnsmasq
```

Then try again.

</details>

<details>
<summary><b>❌ Clients connect but have no internet</b></summary>

- Verify IP forwarding: `cat /proc/sys/net/ipv4/ip_forward` (should be `1`)
- Check iptables rules: `sudo iptables -t nat -L`
- Ensure your WiFi connection itself has internet access
</details>

---

## 🤝 Contributing

Contributions are welcome! Feel free to:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

---

## 📄 License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

---

<div align="center">

**Made with ❤️ for the Linux community**

_If this project helped you, consider giving it a ⭐!_

</div>
