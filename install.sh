#!/bin/sh
# Install the hp-zbook-fury-fan driver (DKMS), boot-time loading, udev permissions and the zfan CLI.
# Run as root from the project directory.
set -eu

PACKAGE=hp-zbook-fury-fan
VERSION=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' driver/dkms.conf)
SOURCE_DIR=/usr/src/$PACKAGE-$VERSION
SUPPORTED_VENDOR=HP
SUPPORTED_BOARD=8DE2
# devm_platform_profile_register() with platform_profile_ops landed in Linux 6.14.
MIN_KERNEL=6.14
KERNEL=$(uname -r)
REQUIRED_COMMANDS="dkms make gcc python3 udevadm modprobe"
DKMS_SIGNING_KEY=/var/lib/dkms/mok.pub
# zfan saves the fan mode here; the udev rule restores it when the driver loads.
SAVED_FAN_MODE=/etc/zfan/fan-mode
# A copy of uninstall.sh lives here so `zfan uninstall` works without the project directory.
ZFAN_LIB_DIR=/usr/local/lib/zfan
INSTALLED_UNINSTALLER=$ZFAN_LIB_DIR/uninstall.sh

fail() {
	echo "install.sh: $1" >&2
	if [ $# -gt 1 ]; then echo "  $2" >&2; fi
	exit 1
}

distro_family() {
	# shellcheck disable=SC1091
	. /etc/os-release
	case " ${ID:-} ${ID_LIKE:-} " in
	*" fedora "* | *" rhel "*) echo fedora ;;
	*" debian "* | *" ubuntu "*) echo debian ;;
	*" arch "*) echo arch ;;
	*) echo other ;;
	esac
}

package_hint() {
	case $(distro_family) in
	fedora) echo "sudo dnf install dkms kernel-devel-$KERNEL gcc make python3" ;;
	debian) echo "sudo apt install dkms linux-headers-$KERNEL build-essential python3" ;;
	arch) echo "sudo pacman -S dkms linux-headers base-devel python" ;;
	*) echo "install dkms, the headers for kernel $KERNEL, gcc, make and python3" ;;
	esac
}

require_root() {
	[ "$(id -u)" -eq 0 ] || fail "needs root" "sudo ./install.sh"
}

require_supported_laptop() {
	vendor=$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null || true)
	board=$(cat /sys/class/dmi/id/board_name 2>/dev/null || true)
	[ "$vendor" = "$SUPPORTED_VENDOR" ] && [ "$board" = "$SUPPORTED_BOARD" ] && return
	fail "this is '${vendor:-unknown}' board '${board:-unknown}'" \
		"the driver supports only the HP ZBook Fury G1i 16\" (board $SUPPORTED_BOARD)"
}

require_kernel() {
	running=$(echo "$KERNEL" | cut -d. -f1-2)
	oldest=$(printf '%s\n%s\n' "$MIN_KERNEL" "$running" | sort -V | head -n1)
	[ "$oldest" = "$MIN_KERNEL" ] || fail "kernel $KERNEL is too old" "the driver needs Linux $MIN_KERNEL or newer"
}

require_build_tools() {
	missing=""
	for command in $REQUIRED_COMMANDS; do
		command -v "$command" >/dev/null || missing="$missing $command"
	done
	[ -z "$missing" ] || fail "missing:$missing" "$(package_hint)"
	[ -d "/lib/modules/$KERNEL/build" ] || fail "no headers for the running kernel $KERNEL" "$(package_hint)"
}

admin_group() {
	for group in wheel sudo; do
		if getent group "$group" >/dev/null; then
			echo "$group"
			return
		fi
	done
	fail "no wheel or sudo group to grant fan control to"
}

remove_old_dkms_versions() {
	dkms status "$PACKAGE" 2>/dev/null | sed -n "s|^$PACKAGE/\([^,:]*\).*|\1|p" | sort -u |
		while read -r old; do dkms remove "$PACKAGE/$old" --all || true; done
}

install_driver() {
	rmmod hp_zbook_fury_fan 2>/dev/null || true
	remove_old_dkms_versions
	rm -rf "$SOURCE_DIR"
	install -d "$SOURCE_DIR"
	install -m 644 driver/hp_zbook_fury_fan.c driver/Makefile driver/dkms.conf "$SOURCE_DIR"/
	dkms install "$PACKAGE/$VERSION"
}

install_boot_integration() {
	install -m 644 packaging/hp-zbook-fury-fan.modules-load.conf /etc/modules-load.d/hp-zbook-fury-fan.conf
	sed "s/@ADMIN_GROUP@/$1/g" packaging/70-hp-zbook-fury-fan.rules >/etc/udev/rules.d/70-hp-zbook-fury-fan.rules
	chmod 644 /etc/udev/rules.d/70-hp-zbook-fury-fan.rules
	udevadm control --reload
}

install_saved_fan_mode() {
	install -d -m 755 "$(dirname "$SAVED_FAN_MODE")"
	# Keep the saved choice across reinstalls and updates.
	[ -f "$SAVED_FAN_MODE" ] || echo follow >"$SAVED_FAN_MODE"
	chgrp "$1" "$SAVED_FAN_MODE"
	chmod 664 "$SAVED_FAN_MODE"
}

install_cli() {
	install -m 755 cli/zfan /usr/local/bin/zfan
	# The CLI used to be called furyfan.
	rm -f /usr/local/bin/furyfan
}

install_uninstaller() {
	install -d -m 755 "$ZFAN_LIB_DIR"
	install -m 755 uninstall.sh "$INSTALLED_UNINSTALLER"
}

secure_boot_enabled() {
	command -v mokutil >/dev/null && mokutil --sb-state 2>/dev/null | grep -q "SecureBoot enabled"
}

load_driver() {
	modprobe hp_zbook_fury_fan && return
	if secure_boot_enabled; then
		fail "Secure Boot rejected the driver: enroll DKMS's signing key once, then reboot" \
			"sudo mokutil --import $DKMS_SIGNING_KEY   (choose a password, then 'Enroll MOK' at the next boot)"
	fi
	fail "the driver did not load" "see: sudo dmesg | tail"
}

apply_permissions_and_profile() {
	# The hwmon device can appear before udev has loaded the new rule; replay its add event.
	udevadm trigger --action=add --subsystem-match=hwmon --attr-match=name=hp_zbook_fury --settle
	udevadm trigger --action=add --subsystem-match=powercap --sysname-match=intel-rapl:0 --settle
	if command -v tuned-adm >/dev/null; then
		tuned-adm profile "$(tuned-adm active | sed -n 's/^Current active profile: //p')"
	fi
}

require_root
require_supported_laptop
require_kernel
require_build_tools
group=$(admin_group)

install_driver
install_saved_fan_mode "$group"
install_boot_integration "$group"
install_cli
install_uninstaller
load_driver
apply_permissions_and_profile
echo "installed $PACKAGE $VERSION; members of '$group' can change fan modes without sudo. Run: zfan"
