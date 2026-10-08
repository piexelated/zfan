# Changelog

All notable changes to this project. Versions follow [Semantic Versioning](https://semver.org/); the driver and
`zfan` share one version.

## [0.5.0] - 2026-10-08

### Changed

- The CLI is now `zfan` (was `furyfan`); `install.sh` removes the old binary.
- The dashboard is one fan slider (quieter ↔ cooler). Fans follow the desktop power profile (hollow marker) until you
  pin a level with `←` `→`; `f` follows again.
- The CPU row shows the die temperature next to HP's own smoothed reading (`hp` column); the GPU row shows HP's GPU
  reading, also while the dGPU sleeps.
- Fan bars mark the target speed while a fan is still ramping.

### Added

- Details view: HP's ACPI thermal zones (CPU, GPU, skin, battery, charger, board) and the BIOS "Customized Fan Control
  Options" value as `fan ceiling`.
- `zfan --version`; the version shows next to the name in the dashboard.
- `install.sh` checks the laptop, kernel (6.14+), build tools and headers first and prints the install command for
  Fedora, Debian/Ubuntu or Arch; it explains Secure Boot key enrollment when the kernel rejects the driver.
- Fan control without sudo works for the `sudo` group where there is no `wheel` group (Debian, Ubuntu).
- README: requirements and install steps for a fresh machine.

### Removed

- Power profile control (`furyfan power`): the desktop power mode already drives it.
- Full speed from the dashboard while the EC locks it (`zfan max` remains for scripts).

## [0.4.2]

First tracked version: `hp_zbook_fury_fan` driver (platform_profile + hwmon, fan modes through the EC's AFAN byte)
and the `furyfan` CLI.
