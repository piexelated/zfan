# Changelog

All notable changes to this project. Versions follow [Semantic Versioning](https://semver.org/); the driver and
`zfan` share one version.

## [0.5.0] - 2026-10-09

First public release.

- `hp-zbook-fan` kernel driver: fan profiles through the standard `platform_profile` interface and fan speeds through
  hwmon. Loads on HP ZBooks whose ACPI tables name the same EC fan registers at the same offsets (checked read-only
  before anything is written); tested on the ZBook Fury G1i 16" (board 8DE2, BIOS 01.05.01).
- Fan levels quiet, auto and boost. By default the desktop power mode picks the level (power saver and balanced give
  auto, performance gives boost); manual pins one. The choice survives reboots and updates.
- `zfan` dashboard: power mode or manual, a quieter ↔ cooler slider, CPU and GPU temperatures (the die's and HP's
  own reading), power, limits and clocks, and fan speeds with their targets. A details view (`i`) shows every CPU
  power limit, clocks, the GPU, HP's and the EC's sensors and the battery.
- `zfan doctor` checks the driver, boot setup, permissions, power profiles, fans and updates, and prints a fix for
  each problem.
- `install.sh` checks the laptop, kernel (6.14+), build tools and headers first, builds the driver with DKMS, and
  installs nothing if the driver declines the laptop. Fan control works without sudo for the `wheel` or `sudo` group.
- `zfan update` installs the latest GitHub release after verifying its checksum; `zfan uninstall` removes everything.
