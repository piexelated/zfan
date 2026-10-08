#!/bin/sh
# Remove everything install.sh installed. Safe to run again when parts are already gone.
# Run as root: from the project directory, or as the installed copy (zfan uninstall).
# DKMS's Secure Boot signing key (/var/lib/dkms/mok.*) stays: other DKMS modules share it.
set -eu

PACKAGE=hp-zbook-fury-fan
MODULE=hp_zbook_fury_fan
# install.sh keeps a copy of this script here so it outlives the project directory.
ZFAN_LIB_DIR=/usr/local/lib/zfan
SAVED_FAN_MODE_DIR=/etc/zfan

fail() {
	echo "uninstall.sh: $1" >&2
	if [ $# -gt 1 ]; then echo "  $2" >&2; fi
	exit 1
}

require_root() {
	[ "$(id -u)" -eq 0 ] || fail "needs root" "sudo ./uninstall.sh"
}

unload_driver() {
	# Unloading hands the fans back to HP's automatic mode.
	rmmod "$MODULE" 2>/dev/null || true
}

remove_dkms_versions() {
	dkms status "$PACKAGE" 2>/dev/null | sed -n "s|^$PACKAGE/\([^,:]*\).*|\1|p" | sort -u |
		while read -r version; do dkms remove "$PACKAGE/$version" --all || true; done
	rm -rf /usr/src/"$PACKAGE"-*
}

remove_boot_integration() {
	rm -f /etc/modules-load.d/hp-zbook-fury-fan.conf /etc/udev/rules.d/70-hp-zbook-fury-fan.rules
	udevadm control --reload
}

remove_cli_and_state() {
	rm -rf "$SAVED_FAN_MODE_DIR"
	rm -f /usr/local/bin/zfan
	rm -rf "$ZFAN_LIB_DIR"
}

require_root
unload_driver
remove_dkms_versions
remove_boot_integration
remove_cli_and_state
echo "removed the $PACKAGE driver, zfan and the saved fan mode; the CPU energy-counter permission returns to root-only at the next boot"
