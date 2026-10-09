#!/bin/sh
# Install the hp-zbook-fan driver (DKMS), boot-time loading, udev permissions and the zfan CLI.
# Run as root from the project directory.
set -eu

PACKAGE=hp-zbook-fan
VERSION=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' driver/dkms.conf)
SOURCE_DIR=/usr/src/$PACKAGE-$VERSION
SUPPORTED_VENDOR=HP
SUPPORTED_PRODUCT=ZBook
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

# The driver itself checks that the fan controller matches when it loads (see load_driver).
require_hp_zbook() {
	vendor=$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null || true)
	product=$(cat /sys/class/dmi/id/product_name 2>/dev/null || true)
	case "$vendor/$product" in
	"$SUPPORTED_VENDOR/"*"$SUPPORTED_PRODUCT"*) return ;;
	esac
	fail "this is a '${vendor:-unknown}' '${product:-unknown}'" "zfan works only on HP ZBook laptops"
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

remove_dkms_versions() {
	dkms status "$PACKAGE" 2>/dev/null | sed -n "s|^$PACKAGE/\([^,:]*\).*|\1|p" | sort -u |
		while read -r old; do dkms remove "$PACKAGE/$old" --all || true; done
}

install_driver() {
	rmmod hp_zbook_fan 2>/dev/null || true
	remove_dkms_versions
	rm -rf "$SOURCE_DIR"
	install -d "$SOURCE_DIR"
	install -m 644 driver/hp_zbook_fan.c driver/Makefile driver/dkms.conf "$SOURCE_DIR"/
	dkms install "$PACKAGE/$VERSION"
}

install_boot_integration() {
	install -m 644 packaging/hp-zbook-fan.modules-load.conf /etc/modules-load.d/hp-zbook-fan.conf
	sed "s/@ADMIN_GROUP@/$1/g" packaging/70-hp-zbook-fan.rules >/etc/udev/rules.d/70-hp-zbook-fan.rules
	chmod 644 /etc/udev/rules.d/70-hp-zbook-fan.rules
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
}

install_uninstaller() {
	install -d -m 755 "$ZFAN_LIB_DIR"
	install -m 755 uninstall.sh "$INSTALLED_UNINSTALLER"
}

secure_boot_enabled() {
	command -v mokutil >/dev/null && mokutil --sb-state 2>/dev/null | grep -q "SecureBoot enabled"
}

# Runs before anything else is installed, so a laptop the driver turns down is left as it was.
load_driver() {
	error=$(modprobe hp_zbook_fan 2>&1) && return
	remove_dkms_versions
	rm -rf "$SOURCE_DIR"
	case $error in
	*"No such device"*)
		fail "this ZBook's fan controller doesn't match the one zfan knows; nothing was installed" \
			"$(dmesg | grep hp_zbook_fan | tail -n 1)"
		;;
	esac
	if secure_boot_enabled; then
		fail "Secure Boot rejected the driver: enroll DKMS's signing key once, then run install.sh again" \
			"sudo mokutil --import $DKMS_SIGNING_KEY   (choose a password, then 'Enroll MOK' at the next boot)"
	fi
	fail "the driver did not load; nothing was installed" "$error"
}

apply_permissions_and_profile() {
	# The hwmon device can appear before udev has loaded the new rule; replay its add event.
	udevadm trigger --action=add --subsystem-match=hwmon --attr-match=name=hp_zbook --settle
	udevadm trigger --action=add --subsystem-match=powercap --sysname-match=intel-rapl:0 --settle
	if command -v tuned-adm >/dev/null; then
		tuned-adm profile "$(tuned-adm active | sed -n 's/^Current active profile: //p')"
	fi
}

require_root
require_hp_zbook
require_kernel
require_build_tools
group=$(admin_group)

install_driver
load_driver
install_saved_fan_mode "$group"
install_boot_integration "$group"
install_cli
install_uninstaller
apply_permissions_and_profile
echo "installed $PACKAGE $VERSION; members of '$group' can change fan modes without sudo. Run: zfan"
