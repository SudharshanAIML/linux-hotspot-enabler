#!/usr/bin/env bash
# =============================================================================
#  Linux Hotspot Enabler — Universal Setup Script
#  Supports: Ubuntu · Debian · Zorin · Linux Mint · Pop!_OS · elementary OS
#            Arch Linux · Manjaro · EndeavourOS · Garuda
#            Fedora · RHEL · CentOS Stream · AlmaLinux · Rocky Linux
#            openSUSE Leap · openSUSE Tumbleweed
#            Raspberry Pi OS · Kali Linux · Parrot OS
# =============================================================================
set -euo pipefail

# ── Terminal colours ──────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

# ── Logging helpers ───────────────────────────────────────────────────────────
info()    { echo -e "${CYAN}[INFO]${RESET}  $*"; }
success() { echo -e "${GREEN}[  OK]${RESET}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
error()   { echo -e "${RED}[FAIL]${RESET}  $*" >&2; }
step()    { echo -e "\n${BOLD}${BLUE}▶ $*${RESET}"; }
die()     { error "$*"; exit 1; }

# ── Banner ────────────────────────────────────────────────────────────────────
print_banner() {
    echo -e "${BOLD}${CYAN}"
    cat <<'EOF'
  ╔══════════════════════════════════════════════════════╗
  ║         Linux Hotspot Enabler — Setup Script         ║
  ║     Simultaneous WiFi + Hotspot on any Linux box     ║
  ╚══════════════════════════════════════════════════════╝
EOF
    echo -e "${RESET}"
}

# ─────────────────────────────────────────────────────────────────────────────
# 1. ENVIRONMENT GUARD: must run as root (or via sudo)
# ─────────────────────────────────────────────────────────────────────────────
check_root() {
    step "Checking privileges"
    if [[ $EUID -ne 0 ]]; then
        error "This script must be run as root."
        echo -e "  Re-run with:  ${BOLD}sudo bash $0${RESET}"
        exit 1
    fi
    success "Running as root"
}

# ─────────────────────────────────────────────────────────────────────────────
# 2. KERNEL VERSION CHECK (need ≥ 4.0 for nl80211 AP/STA)
# ─────────────────────────────────────────────────────────────────────────────
check_kernel() {
    step "Checking kernel version"
    local kver; kver=$(uname -r)
    local kmaj; kmaj=$(echo "$kver" | cut -d. -f1)
    local kmin; kmin=$(echo "$kver" | cut -d. -f2)
    info "Kernel: $kver"
    if [[ $kmaj -lt 4 ]]; then
        die "Kernel $kver is too old. Linux 4.0+ is required for AP/STA concurrency."
    fi
    if [[ $kmaj -eq 4 && $kmin -lt 0 ]]; then
        die "Kernel $kver is too old. Linux 4.0+ is required."
    fi
    success "Kernel $kver is compatible"
}

# ─────────────────────────────────────────────────────────────────────────────
# 3. ARCHITECTURE CHECK
# ─────────────────────────────────────────────────────────────────────────────
check_arch() {
    step "Checking CPU architecture"
    local arch; arch=$(uname -m)
    info "Architecture: $arch"
    case "$arch" in
        x86_64|aarch64|armv7l|armv6l|riscv64)
            success "Architecture $arch is supported" ;;
        *)
            warn "Architecture '$arch' is untested. Proceeding with caution." ;;
    esac
}

# ─────────────────────────────────────────────────────────────────────────────
# 4. OS / DISTRO DETECTION
# ─────────────────────────────────────────────────────────────────────────────
detect_distro() {
    step "Detecting Linux distribution"

    OS_ID=""
    OS_ID_LIKE=""
    OS_VERSION_ID=""
    OS_PRETTY_NAME=""
    PKG_MANAGER=""
    INSTALL_CMD=""
    UPDATE_CMD=""
    NCURSES_DEV=""
    BUILD_ESSENTIAL=""

    # Source /etc/os-release if available (most distros since 2012)
    if [[ -f /etc/os-release ]]; then
        # shellcheck source=/dev/null
        source /etc/os-release
        OS_ID="${ID:-}"
        OS_ID_LIKE="${ID_LIKE:-}"
        OS_VERSION_ID="${VERSION_ID:-}"
        OS_PRETTY_NAME="${PRETTY_NAME:-unknown}"
    elif [[ -f /etc/lsb-release ]]; then
        # Older Ubuntu / Debian fallback
        # shellcheck source=/dev/null
        source /etc/lsb-release
        OS_ID="${DISTRIB_ID,,}"
        OS_PRETTY_NAME="${DISTRIB_DESCRIPTION:-unknown}"
    elif [[ -f /etc/debian_version ]]; then
        OS_ID="debian"
        OS_PRETTY_NAME="Debian $(cat /etc/debian_version)"
    elif [[ -f /etc/redhat-release ]]; then
        OS_ID="rhel"
        OS_PRETTY_NAME="$(cat /etc/redhat-release)"
    elif [[ -f /etc/arch-release ]]; then
        OS_ID="arch"
        OS_PRETTY_NAME="Arch Linux"
    elif [[ -f /etc/SuSE-release ]] || [[ -f /etc/opensuse-release ]]; then
        OS_ID="opensuse"
        OS_PRETTY_NAME="openSUSE"
    else
        die "Cannot detect Linux distribution. Please install dependencies manually (see README.md)."
    fi

    info "Detected: ${OS_PRETTY_NAME}"

    # Normalise for matching
    local id_lower="${OS_ID,,}"
    local like_lower="${OS_ID_LIKE,,}"

    # ── Debian / Ubuntu family ────────────────────────────────────────────
    if [[ "$id_lower" =~ ^(debian|ubuntu|linuxmint|mint|zorin|pop|elementary|kali|parrot|raspbian|pimoxmox|neon)$ ]] \
        || [[ "$like_lower" =~ debian ]] \
        || [[ "$like_lower" =~ ubuntu ]]; then

        PKG_MANAGER="apt"
        UPDATE_CMD="apt-get update -qq"
        INSTALL_CMD="apt-get install -y"
        NCURSES_DEV="libncurses-dev"
        BUILD_ESSENTIAL="build-essential"

    # ── Arch family ──────────────────────────────────────────────────────
    elif [[ "$id_lower" =~ ^(arch|manjaro|endeavouros|garuda|artix|parabola|hyperbola|blackarch)$ ]] \
        || [[ "$like_lower" =~ arch ]]; then

        PKG_MANAGER="pacman"
        UPDATE_CMD=""          # pacman -Sy handles this
        INSTALL_CMD="pacman -Sy --noconfirm"
        NCURSES_DEV="ncurses"
        BUILD_ESSENTIAL="base-devel"

    # ── Fedora / RHEL / CentOS family ────────────────────────────────────
    elif [[ "$id_lower" =~ ^(fedora|rhel|centos|almalinux|rocky|ol|scientific|eurolinux|amzn)$ ]] \
        || [[ "$like_lower" =~ (rhel|fedora) ]]; then

        # Prefer dnf over yum (dnf available since Fedora 18, RHEL 8)
        if command -v dnf &>/dev/null; then
            PKG_MANAGER="dnf"
            INSTALL_CMD="dnf install -y"
            UPDATE_CMD=""
        elif command -v yum &>/dev/null; then
            PKG_MANAGER="yum"
            INSTALL_CMD="yum install -y"
            UPDATE_CMD=""
        else
            die "Neither dnf nor yum found on this RHEL/Fedora system."
        fi
        NCURSES_DEV="ncurses-devel"
        BUILD_ESSENTIAL="gcc make"

    # ── openSUSE family ───────────────────────────────────────────────────
    elif [[ "$id_lower" =~ ^(opensuse.*|sles|sled)$ ]] \
        || [[ "$like_lower" =~ suse ]]; then

        PKG_MANAGER="zypper"
        UPDATE_CMD="zypper refresh -y"
        INSTALL_CMD="zypper install -y"
        NCURSES_DEV="ncurses-devel"
        BUILD_ESSENTIAL="gcc make"

    # ── Alpine Linux (musl-based) ─────────────────────────────────────────
    elif [[ "$id_lower" == "alpine" ]]; then
        PKG_MANAGER="apk"
        UPDATE_CMD="apk update"
        INSTALL_CMD="apk add --no-cache"
        NCURSES_DEV="ncurses-dev"
        BUILD_ESSENTIAL="build-base"

    # ── Void Linux ────────────────────────────────────────────────────────
    elif [[ "$id_lower" == "void" ]]; then
        PKG_MANAGER="xbps"
        UPDATE_CMD="xbps-install -S"
        INSTALL_CMD="xbps-install -y"
        NCURSES_DEV="ncurses-devel"
        BUILD_ESSENTIAL="base-devel"

    # ── Gentoo ────────────────────────────────────────────────────────────
    elif [[ "$id_lower" == "gentoo" ]]; then
        PKG_MANAGER="portage"
        UPDATE_CMD=""
        INSTALL_CMD="emerge -av --autounmask-write=y"
        NCURSES_DEV="sys-libs/ncurses"
        BUILD_ESSENTIAL="sys-devel/gcc sys-devel/make"

    else
        warn "Unknown distro '${OS_PRETTY_NAME}'. Attempting apt/dnf/pacman auto-detection..."
        if   command -v apt-get &>/dev/null; then
            PKG_MANAGER="apt"; UPDATE_CMD="apt-get update -qq"; INSTALL_CMD="apt-get install -y"
            NCURSES_DEV="libncurses-dev"; BUILD_ESSENTIAL="build-essential"
        elif command -v dnf     &>/dev/null; then
            PKG_MANAGER="dnf";  UPDATE_CMD=""; INSTALL_CMD="dnf install -y"
            NCURSES_DEV="ncurses-devel"; BUILD_ESSENTIAL="gcc make"
        elif command -v pacman  &>/dev/null; then
            PKG_MANAGER="pacman"; UPDATE_CMD=""; INSTALL_CMD="pacman -Sy --noconfirm"
            NCURSES_DEV="ncurses"; BUILD_ESSENTIAL="base-devel"
        elif command -v zypper  &>/dev/null; then
            PKG_MANAGER="zypper"; UPDATE_CMD="zypper refresh -y"; INSTALL_CMD="zypper install -y"
            NCURSES_DEV="ncurses-devel"; BUILD_ESSENTIAL="gcc make"
        else
            die "No recognised package manager found. Install dependencies manually — see README.md."
        fi
        warn "Guessed package manager: $PKG_MANAGER"
    fi

    success "Package manager: $PKG_MANAGER"
}

# ─────────────────────────────────────────────────────────────────────────────
# 5. PACKAGE CACHE UPDATE
# ─────────────────────────────────────────────────────────────────────────────
refresh_package_cache() {
    if [[ -n "$UPDATE_CMD" ]]; then
        step "Refreshing package cache"
        if ! $UPDATE_CMD; then
            warn "Package cache refresh failed — attempting to continue anyway."
        else
            success "Package cache refreshed"
        fi
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 6. DEPENDENCY INSTALLATION
# ─────────────────────────────────────────────────────────────────────────────
install_dependencies() {
    step "Installing runtime and build dependencies"

    local -a PKGS_TO_INSTALL=()

    # Build the package list for the detected manager
    case "$PKG_MANAGER" in
        apt)
            PKGS_TO_INSTALL=(iw hostapd dnsmasq iptables "$NCURSES_DEV" "$BUILD_ESSENTIAL") ;;
        pacman)
            PKGS_TO_INSTALL=(iw hostapd dnsmasq iptables "$NCURSES_DEV" "$BUILD_ESSENTIAL") ;;
        dnf|yum)
            PKGS_TO_INSTALL=(iw hostapd dnsmasq iptables "$NCURSES_DEV" gcc make) ;;
        zypper)
            PKGS_TO_INSTALL=(iw hostapd dnsmasq iptables "$NCURSES_DEV" gcc make) ;;
        apk)
            PKGS_TO_INSTALL=(iw hostapd dnsmasq iptables ncurses-dev build-base) ;;
        xbps)
            PKGS_TO_INSTALL=(iw hostapd dnsmasq iptables ncurses-devel base-devel) ;;
        portage)
            PKGS_TO_INSTALL=(net-wireless/iw net-wireless/hostapd net-dns/dnsmasq
                             net-firewall/iptables sys-libs/ncurses sys-devel/gcc sys-devel/make) ;;
    esac

    info "Packages to install: ${PKGS_TO_INSTALL[*]}"

    local install_failed=0
    case "$PKG_MANAGER" in
        apt)    apt-get install -y "${PKGS_TO_INSTALL[@]}" || install_failed=1 ;;
        pacman) pacman -Sy --noconfirm "${PKGS_TO_INSTALL[@]}" || install_failed=1 ;;
        dnf)    dnf install -y "${PKGS_TO_INSTALL[@]}" || install_failed=1 ;;
        yum)    yum install -y "${PKGS_TO_INSTALL[@]}" || install_failed=1 ;;
        zypper) zypper install -y "${PKGS_TO_INSTALL[@]}" || install_failed=1 ;;
        apk)    apk add --no-cache "${PKGS_TO_INSTALL[@]}" || install_failed=1 ;;
        xbps)   xbps-install -y "${PKGS_TO_INSTALL[@]}" || install_failed=1 ;;
        portage) emerge -av "${PKGS_TO_INSTALL[@]}" || install_failed=1 ;;
    esac

    if [[ $install_failed -eq 1 ]]; then
        error "Package installation failed."
        echo "  Tip: Check your internet connection and mirror configuration."
        exit 1
    fi

    success "All packages installed"
}

# ─────────────────────────────────────────────────────────────────────────────
# 7. POST-INSTALL BINARY VERIFICATION
# ─────────────────────────────────────────────────────────────────────────────
verify_binaries() {
    step "Verifying installed binaries"
    local missing=0
    for bin in iw hostapd dnsmasq iptables gcc make; do
        if command -v "$bin" &>/dev/null; then
            success "  ✓  $(command -v "$bin") — $(${bin} --version 2>&1 | head -1 || true)"
        else
            error "  ✗  '$bin' not found in PATH after installation."
            missing=$((missing + 1))
        fi
    done

    # ncurses is a library — check for header or pkg-config
    if pkg-config --exists ncurses 2>/dev/null || \
       [[ -f /usr/include/ncurses.h ]] || \
       [[ -f /usr/include/ncurses/ncurses.h ]]; then
        success "  ✓  ncurses development headers found"
    else
        warn "  ncurses headers not found — build may fail"
        missing=$((missing + 1))
    fi

    if [[ $missing -gt 0 ]]; then
        die "$missing required component(s) missing. Cannot continue."
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 8. WIFI HARDWARE CHECKS
# ─────────────────────────────────────────────────────────────────────────────
check_wifi_hardware() {
    step "Checking WiFi hardware"

    # rfkill — unblock any software kill-switch
    if command -v rfkill &>/dev/null; then
        info "Checking rfkill status..."
        local blocked
        blocked=$(rfkill list wifi 2>/dev/null | grep -c "Soft blocked: yes" || true)
        if [[ $blocked -gt 0 ]]; then
            warn "WiFi is soft-blocked (rfkill). Unblocking..."
            rfkill unblock wifi
            success "WiFi rfkill unblocked"
        else
            success "No rfkill blocks detected"
        fi
    fi

    # Detect any wireless interface
    local wif=""
    if command -v iw &>/dev/null; then
        wif=$(iw dev 2>/dev/null | awk '/Interface/{print $2; exit}')
    fi
    if [[ -z "$wif" ]]; then
        # Fallback: look in /sys/class/net
        for d in /sys/class/net/*/wireless; do
            [[ -d "$d" ]] && { wif=$(basename "$(dirname "$d")"); break; }
        done
    fi

    if [[ -z "$wif" ]]; then
        warn "No wireless interface detected."
        warn "Make sure your WiFi adapter is connected and its driver is loaded."
        warn "Check: lsmod | grep <driver_name>"
        WIFI_IFACE=""
    else
        WIFI_IFACE="$wif"
        success "WiFi interface detected: $WIFI_IFACE"

        # AP/STA concurrency check
        info "Checking AP/STA concurrency support on $WIFI_IFACE..."
        local phy
        phy=$(iw dev "$WIFI_IFACE" info 2>/dev/null | awk '/wiphy/{print "phy"$2}')
        if [[ -n "$phy" ]]; then
            local combos
            combos=$(iw phy "$phy" info 2>/dev/null | grep -A8 "valid interface combinations" || true)
            if echo "$combos" | grep -q "AP"; then
                success "AP/STA concurrency is SUPPORTED by $phy"
            else
                warn "AP/STA concurrency may NOT be supported by $phy."
                warn "The hotspot may not work. You can still build and try."
            fi
        fi
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 9. NETWORK MANAGER CONFLICT DETECTION
# ─────────────────────────────────────────────────────────────────────────────
handle_networkmanager() {
    step "Checking NetworkManager configuration"

    if systemctl is-active --quiet NetworkManager 2>/dev/null; then
        info "NetworkManager is running"

        # Check if NM has an unmanaged rule for 'ap0' already
        local nm_unmanaged="/etc/NetworkManager/conf.d/99-hotspot-ap0.conf"
        if [[ ! -f "$nm_unmanaged" ]]; then
            info "Creating NetworkManager unmanaged rule for ap0..."
            cat > "$nm_unmanaged" <<'NMCFG'
# Created by linux-hotspot-enabler setup
# Prevents NetworkManager from taking over the virtual AP interface
[keyfile]
unmanaged-devices=interface-name:ap0
NMCFG
            if systemctl reload NetworkManager 2>/dev/null; then
                success "NetworkManager reloaded with ap0 unmanaged rule"
            else
                warn "Could not reload NetworkManager — you may need to reboot."
            fi
        else
            success "NetworkManager unmanaged rule already exists"
        fi
    else
        info "NetworkManager is not running — no action needed"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 10. SYSTEMD-RESOLVED PORT 53 CONFLICT
# ─────────────────────────────────────────────────────────────────────────────
handle_systemd_resolved() {
    step "Checking systemd-resolved port 53 conflict"

    if systemctl is-active --quiet systemd-resolved 2>/dev/null; then
        # Check if it binds to port 53 (which blocks dnsmasq)
        local stub_conf="/etc/systemd/resolved.conf"
        local stub_enabled
        stub_enabled=$(grep -i "^DNSStubListener" "$stub_conf" 2>/dev/null | tail -1 | cut -d= -f2 | tr -d ' ' || echo "yes")

        if [[ "${stub_enabled,,}" != "no" ]]; then
            warn "systemd-resolved stub listener is active (port 53)."
            warn "Disabling DNSStubListener so dnsmasq can bind to port 53..."

            # Edit or create the drop-in
            local dropin="/etc/systemd/resolved.conf.d/99-hotspot-no-stub.conf"
            mkdir -p "$(dirname "$dropin")"
            cat > "$dropin" <<'RCONF'
[Resolve]
DNSStubListener=no
RCONF
            systemctl restart systemd-resolved 2>/dev/null || true
            success "DNSStubListener disabled (drop-in written)"
        else
            success "DNSStubListener is already disabled"
        fi
    else
        success "systemd-resolved is not running — no conflict"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 11. IP FORWARDING (persist across reboots)
# ─────────────────────────────────────────────────────────────────────────────
enable_ip_forwarding() {
    step "Enabling IPv4 forwarding"

    # Enable immediately in the running kernel
    echo 1 > /proc/sys/net/ipv4/ip_forward
    success "ip_forward set to 1 for this session"

    # Persist via sysctl
    local sysctl_conf="/etc/sysctl.d/99-hotspot-forward.conf"
    if [[ ! -f "$sysctl_conf" ]] || ! grep -q "net.ipv4.ip_forward=1" "$sysctl_conf" 2>/dev/null; then
        echo "net.ipv4.ip_forward=1" > "$sysctl_conf"
        sysctl --system &>/dev/null || sysctl -p "$sysctl_conf" 2>/dev/null || true
        success "ip_forward persisted to $sysctl_conf"
    else
        success "ip_forward already persisted"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 12. OPTIONAL EPEL/REMI FOR RHEL-FAMILY (hostapd often missing)
# ─────────────────────────────────────────────────────────────────────────────
maybe_enable_epel() {
    local id_lower="${OS_ID,,}"
    # Only for RHEL/CentOS/AlmaLinux/Rocky where hostapd is not in base repos
    if [[ "$id_lower" =~ ^(rhel|centos|almalinux|rocky|ol|scientific|eurolinux)$ ]] && \
       [[ "$PKG_MANAGER" =~ ^(dnf|yum)$ ]]; then

        step "Checking EPEL repository (RHEL/CentOS family)"
        if ! rpm -q epel-release &>/dev/null 2>&1; then
            info "Installing EPEL repository (needed for hostapd)..."
            if ! $INSTALL_CMD epel-release; then
                warn "Could not install EPEL. hostapd may be unavailable — install manually."
            else
                success "EPEL enabled"
            fi
        else
            success "EPEL is already enabled"
        fi
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 13. BUILD THE PROJECT
# ─────────────────────────────────────────────────────────────────────────────
build_project() {
    step "Building Linux Hotspot Enabler"

    # Determine script directory (works even when called with sudo from another dir)
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    info "Project directory: $SCRIPT_DIR"

    if [[ ! -f "$SCRIPT_DIR/Makefile" ]]; then
        die "Makefile not found in $SCRIPT_DIR. Run this script from the project root."
    fi

    # Clean stale build artifacts
    info "Cleaning previous build artifacts..."
    make -C "$SCRIPT_DIR" clean 2>/dev/null || true

    # Build
    info "Running make..."
    if ! make -C "$SCRIPT_DIR" -j"$(nproc)"; then
        die "Build failed. Check the output above for errors."
    fi

    if [[ -f "$SCRIPT_DIR/hotspot-enabler" ]]; then
        success "Build successful → $SCRIPT_DIR/hotspot-enabler"
    else
        die "Build appeared to succeed but binary not found."
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 14. OPTIONAL SYSTEM-WIDE INSTALL
# ─────────────────────────────────────────────────────────────────────────────
maybe_install_system() {
    step "System-wide installation (optional)"
    echo -e "${YELLOW}Install hotspot-enabler to /usr/local/bin so you can run it from anywhere?${RESET}"
    read -r -p "  [y/N] " answer
    case "${answer,,}" in
        y|yes)
            make -C "$SCRIPT_DIR" install
            success "Installed to /usr/local/bin/hotspot-enabler"
            ;;
        *)
            info "Skipping system-wide install. Run from project dir: sudo ./hotspot-enabler"
            ;;
    esac
}

# ─────────────────────────────────────────────────────────────────────────────
# 15. FINAL SUMMARY
# ─────────────────────────────────────────────────────────────────────────────
print_summary() {
    echo ""
    echo -e "${BOLD}${GREEN}══════════════════════════════════════════════════════${RESET}"
    echo -e "${BOLD}${GREEN}  ✅  Setup complete!${RESET}"
    echo -e "${BOLD}${GREEN}══════════════════════════════════════════════════════${RESET}"
    echo ""
    echo -e "  To start the hotspot manager:"
    echo -e "    ${BOLD}cd $(pwd) && sudo ./hotspot-enabler${RESET}"
    echo ""

    if [[ -n "${WIFI_IFACE:-}" ]]; then
        echo -e "  Detected WiFi interface: ${BOLD}$WIFI_IFACE${RESET}"
    fi

    echo ""
    echo -e "  Quick reference:"
    echo -e "    ${CYAN}F1${RESET} Dashboard  ${CYAN}F2${RESET} Config  ${CYAN}F3${RESET} Clients  ${CYAN}F4${RESET} Log  ${CYAN}q${RESET} Quit"
    echo ""
    echo -e "  Uninstall: ${BOLD}sudo make uninstall${RESET}"
    echo ""
}

# ─────────────────────────────────────────────────────────────────────────────
# MAIN EXECUTION
# ─────────────────────────────────────────────────────────────────────────────
main() {
    print_banner
    check_root
    check_kernel
    check_arch
    detect_distro
    maybe_enable_epel    # Must come before package install on RHEL family
    refresh_package_cache
    install_dependencies
    verify_binaries
    check_wifi_hardware
    handle_networkmanager
    handle_systemd_resolved
    enable_ip_forwarding
    build_project
    maybe_install_system
    print_summary
}

main "$@"
