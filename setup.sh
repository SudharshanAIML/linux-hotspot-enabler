#!/usr/bin/env bash
# Lightweight cross-distro installer for dependencies used by Linux Hotspot Enabler
# Supports: Debian/Ubuntu, Arch, Fedora/RHEL, openSUSE, Void
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "Re-running with sudo..."
  exec sudo bash "$0" "$@"
fi

info() { printf "  \033[1;34m%s\033[0m\n" "$*"; }
ok()   { printf "  \033[1;32m%s\033[0m\n" "$*"; }
err()  { printf "  \033[1;31m%s\033[0m\n" "$*"; }

detect_distro() {
  . /etc/os-release 2>/dev/null || return 1
  echo "${ID:-unknown}" "${ID_LIKE:-}"
}

install_deps_apt() {
  apt update
  DEPS=(iw hostapd dnsmasq iptables libncurses-dev build-essential)
  apt install -y "${DEPS[@]}"
}

install_deps_pacman() {
  pacman -Sy --noconfirm iw hostapd dnsmasq iptables ncurses base-devel
}

install_deps_dnf() {
  dnf install -y iw hostapd dnsmasq iptables ncurses-devel gcc make
}

install_deps_zypper() {
  zypper --non-interactive install iw hostapd dnsmasq iptables ncurses-devel gcc make
}

install_deps_xbps() {
  xbps-install -Sy iw hostapd dnsmasq iptables ncurses-devel gcc make
}

verify_tools() {
  local missing=0
  for t in iw hostapd dnsmasq iptables; do
    if ! command -v "$t" >/dev/null 2>&1; then
      echo "    ✗ $t"
      missing=1
    else
      echo "    ✓ $t"
    fi
  done
  return $missing
}

main() {
  info "Detecting distro..."
  read -r id id_like <<<"$(detect_distro || echo unknown )"

  info "Installing dependencies for: $id (id_like: $id_like)"
  case "$id" in
    ubuntu|debian|linuxmint|zorin|pop)
      install_deps_apt
      ;;
    arch|manjaro|endeavouros)
      install_deps_pacman
      ;;
    fedora|rhel|centos|rocky)
      install_deps_dnf
      ;;
    opensuse*|suse)
      install_deps_zypper
      ;;
    void)
      install_deps_xbps
      ;;
    *)
      # try id_like hints
      if [[ "$id_like" == *"debian"* ]] || [[ "$id_like" == *"ubuntu"* ]]; then
        install_deps_apt
      elif [[ "$id_like" == *"arch"* ]]; then
        install_deps_pacman
      elif [[ "$id_like" == *"fedora"* ]] || [[ "$id_like" == *"rhel"* ]]; then
        install_deps_dnf
      else
        err "Unsupported/unknown distro. See README for manual steps:"
        echo
        sed -n '1,160p' README.md
        exit 1
      fi
      ;;
  esac

  echo
  info "Verifying installed tools..."
  if verify_tools; then
    ok "All required tools are available."
    ok "Build with: make && sudo ./hotspot-enabler"
  else
    err "Some required tools are still missing. Check package manager output."
    exit 1
  fi
}

main "$@"