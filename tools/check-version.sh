#!/bin/sh
# Check that zfan, the DKMS package and the driver carry the same version, and print it.
# With a tag argument (v1.2.3), also check the tag matches. Run from the project directory.
#
# To release: bump VERSION in cli/zfan, PACKAGE_VERSION in driver/dkms.conf and MODULE_VERSION in the driver, add a
# "## [x.y.z]" section to CHANGELOG.md, then push a vX.Y.Z tag; the release workflow runs this and publishes.
set -eu

cli=$(sed -n 's/^VERSION = "\(.*\)"/\1/p' cli/zfan)
dkms=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' driver/dkms.conf)
module=$(sed -n 's/^MODULE_VERSION("\(.*\)");/\1/p' driver/hp_zbook_fan.c)

if [ -z "$cli" ] || [ "$cli" != "$dkms" ] || [ "$cli" != "$module" ]; then
	echo "version mismatch: cli/zfan=$cli driver/dkms.conf=$dkms MODULE_VERSION=$module" >&2
	exit 1
fi
if [ $# -gt 0 ] && [ "$1" != "v$cli" ]; then
	echo "tag $1 does not match version $cli (expected v$cli)" >&2
	exit 1
fi
if ! grep -q "^## \[$cli\]" CHANGELOG.md; then
	echo "CHANGELOG.md has no '## [$cli]' section" >&2
	exit 1
fi
echo "$cli"
