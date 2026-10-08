# hp-zbook-fury-fan

Fan profiles for the HP ZBook Fury G1i (board 8DE2) on Linux, through the standard `platform_profile` interface.
The desktop power-mode switch (via `tuned-ppd` or `power-profiles-daemon`) controls the fans with no extra tooling.

| Power mode | platform_profile | EC `AFAN` (0x2D) | Fans under load |
|---|---|---|---|
| Performance | `performance` | `0x11` | boost, ~6100 / 6500 / 8500 RPM |
| Balanced | `balanced` | `0x00` | HP automatic |
| Power saver | `quiet` | `0x00` | HP automatic (as in HP's own mode table) |

Why `AFAN`: once the OS owns ACPI, the EC drops host fan-speed writes but still honors this fan-mode selector.

Behavior:
- binds only on DMI vendor `HP`, board `8DE2`;
- re-applies the chosen mode after resume and if firmware resets `AFAN` (checked every 10 s);
- restores automatic mode (`0x00`) on unload.

hwmon (`hp_zbook_fury`): `fan1..3_input` and `fan1..3_target` in RPM, `pwm1_enable` = `0` full speed / `2` fan mode.

Platform device attributes:
- `fan_mode`: `follow` (default, set by the platform profile as above), or `quiet` (capped, AFAN `0x22`) / `auto` / `boost` to pin a curve
  independently of the power profile.
- `fan_max_available`: `1` when full speed drives fans 1 and 3 to hardware maximum. At load and on resume the driver
  writes the maximum override to fan 1 and checks that the EC kept it. `0` means the EC drops host overrides (the
  BIOS locks them before the OS starts); `pwm1_enable = 0` is then refused with `EPERM`.

Install with `../install.sh` (DKMS); the `zfan` CLI is described in `../README.md`.

Build and try manually:

```sh
make
sudo insmod hp_zbook_fury_fan.ko
cat /sys/firmware/acpi/platform_profile_choices   # quiet balanced performance
```

Tested 2026-10-06 on BIOS 01.05.01 / EC 55.3C.00, kernel 7.2.0, Fedora 43 with tuned-ppd; installed via DKMS 0.4.1.
Full speed is locked on this BIOS; see the main README.
