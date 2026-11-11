#!/bin/bash
# Install omarchy-xdr: the kernel module (loaded at every boot), the
# omarchy-xdr command, and the top-bar widget.
#
#   ./install.sh              everything (asks for sudo)
#   ./install.sh --user-only  command and bar widget only
#   ./install.sh --uninstall
set -euo pipefail

root=$(dirname "$(readlink -f "$0")")
plugin_id=xdr.brightness
plugin_dir=${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/plugins/$plugin_id
unit_dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user

(( EUID )) || { echo "run as your own user, not root" >&2; exit 1; }

user_part() {
  install -Dm755 "$root/bin/omarchy-xdr" "$HOME/.local/bin/omarchy-xdr"
  install -Dm644 "$root/system/omarchy-xdr.service" "$unit_dir/omarchy-xdr.service"
  systemctl --user daemon-reload
  mkdir -p "$plugin_dir"
  install -m644 "$root/plugin/$plugin_id"/* "$plugin_dir"/
  if command -v omarchy >/dev/null && ! grep -q "\"$plugin_id\"" "${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/shell.json" 2>/dev/null; then
    omarchy bar put $plugin_id --after omarchy.audio || echo "add the '$plugin_id' widget to the bar by hand: omarchy bar put $plugin_id"
  fi
}

system_part() {
  sudo install -d /usr/local/lib/omarchy-xdr /usr/local/src/omarchy-xdr
  sudo install -m644 "$root/probe/apple_xdr_probe.c" "$root/probe/Makefile" /usr/local/src/omarchy-xdr/
  sudo install -m755 "$root/probe/gen-layout.sh" /usr/local/src/omarchy-xdr/
  sudo install -m755 "$root/system/omarchy-xdr-load" /usr/local/lib/omarchy-xdr/load
  sudo install -m644 "$root/system/omarchy-xdr-probe.service" /etc/systemd/system/
  sudo systemctl daemon-reload
  sudo systemctl stop omarchy-xdr-probe.service 2>/dev/null || true
  sudo rmmod apple_xdr_probe 2>/dev/null || true
  sudo systemctl enable --now omarchy-xdr-probe.service
}

case ${1:-} in
  --user-only) user_part ;;
  --uninstall)
    systemctl --user disable omarchy-xdr.service 2>/dev/null || true
    rm -f "$HOME/.local/bin/omarchy-xdr" "$unit_dir/omarchy-xdr.service"
    rm -rf "$plugin_dir"
    sudo systemctl disable --now omarchy-xdr-probe.service 2>/dev/null || true
    sudo rm -rf /etc/systemd/system/omarchy-xdr-probe.service /usr/local/lib/omarchy-xdr /usr/local/src/omarchy-xdr /var/cache/omarchy-xdr
    sudo systemctl daemon-reload
    echo "removed. Take '$plugin_id' out of ~/.config/omarchy/shell.json if it is still listed."
    ;;
  "")
    system_part
    user_part
    "$HOME/.local/bin/omarchy-xdr" apply
    "$HOME/.local/bin/omarchy-xdr" status
    ;;
  *) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
