#!/bin/sh
# Install the hp-zbook-fury-fan driver (DKMS), boot-time loading, udev permissions and the zfan CLI.
# Run as root from the project directory.
set -eu

PACKAGE=hp-zbook-fury-fan
VERSION=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' driver/dkms.conf)
SOURCE_DIR=/usr/src/$PACKAGE-$VERSION

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
	install -m 644 packaging/70-hp-zbook-fury-fan.rules /etc/udev/rules.d/70-hp-zbook-fury-fan.rules
	udevadm control --reload
}

install_cli() {
	install -m 755 cli/zfan /usr/local/bin/zfan
	# The CLI used to be called furyfan.
	rm -f /usr/local/bin/furyfan
}

reapply_power_profile() {
	modprobe hp_zbook_fury_fan
	# The hwmon device can appear before udev has loaded the new rule; replay its add event.
	udevadm trigger --action=add --subsystem-match=hwmon --attr-match=name=hp_zbook_fury --settle
	udevadm trigger --action=add --subsystem-match=powercap --sysname-match=intel-rapl:0 --settle
	if command -v tuned-adm >/dev/null; then
		tuned-adm profile "$(tuned-adm active | sed -n 's/^Current active profile: //p')"
	fi
}

install_driver
install_boot_integration
install_cli
reapply_power_profile
echo "installed $PACKAGE $VERSION"
