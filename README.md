# HP ZBook Fury G1i fan control for Linux

Fan control for the HP ZBook Fury G1i 16" (board `8DE2`) from a running Linux system: a kernel driver and a CLI.

```
               Quieter                           Cooler
  fans         ─────────────────────────────────────△──    follows performance
               quiet             auto             boost

               temp   hp     power    limit      clock
  cpu          95°    78°    49 W     80 W       3.2 GHz    throttling 86%
  gpu          55°    52°    8 W      95 W       P8

  fan 1        ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━│━━━━━━━━──────────────   4636 rpm
```

## Requirements

- **The laptop:** HP ZBook Fury G1i 16" (DMI vendor `HP`, board `8DE2`). The driver refuses to load anywhere else.
  Check with `cat /sys/class/dmi/id/board_name`.
- **Linux 6.14 or newer**, with headers for the running kernel.
- **Build tools and Python 3.10+:**

  | Distribution | Command |
  |---|---|
  | Fedora | `sudo dnf install dkms kernel-devel-$(uname -r) gcc make python3` |
  | Debian / Ubuntu | `sudo apt install dkms linux-headers-$(uname -r) build-essential python3` |
  | Arch | `sudo pacman -S dkms linux-headers base-devel python` |

- **A power-profile service** (`tuned-ppd` or `power-profiles-daemon`, the default on GNOME and KDE) if you want the
  fans to follow the desktop power mode. Without one, pin a fan level in zfan instead.
- **Optional:** `nvidia-smi` (from the NVIDIA driver) for GPU power, clocks and limits. Everything else zfan shows comes
  from drivers the kernel loads by itself (`coretemp`, `intel_rapl`, ACPI thermal zones, `hp-wmi`).

## Install

```sh
git clone https://github.com/RePixelatedMC/zfan.git
cd zfan
sudo ./install.sh
zfan
```

`install.sh` checks all of the above first and tells you what is missing and how to install it. Then it builds the
driver with DKMS (rebuilt on kernel updates), loads it at boot, lets members of your admin group (`wheel`, or `sudo` on
Debian and Ubuntu) change the fan mode without sudo, installs `zfan` to `/usr/local/bin` and re-applies the current
tuned profile. Run `zfan doctor` if anything looks off.

**Secure Boot:** if it is on, the kernel only loads signed modules. DKMS signs the driver with its own key, which you
enroll once: `sudo mokutil --import /var/lib/dkms/mok.pub`, pick a password, reboot, choose *Enroll MOK* and enter
it. `install.sh` tells you when this is needed.

## Use

`zfan` opens a live dashboard (84 columns or wider):

- **main view:** the fan level slider; CPU and GPU (die temperature, HP's own smoothed reading of it, power,
  sustained limit, clock, plus a warning while throttling or power-limited); fan speeds, with a tick at the target while a fan is still ramping.
- **details (`i`):** every CPU power limit as the kernel exposes it, one column per interface (MSR
  `intel-rapl:0`, MMIO `intel-rapl-mmio:0`) plus the one that applies (the lower), time windows, rated power and the
  other RAPL zones (core, uncore, psys); CPU clocks, governor, EPP, turbo; GPU limits, clocks and what holds it back
  (the dGPU is never woken for this); HP's ACPI thermal zones (CPU, GPU, skin, battery, charger and board);
  every EC sensor (the hottest picks the fan-curve step), the raw `AFAN` byte,
  the tuned profile and the BIOS "Customized Fan Control Options" value (`fan ceiling`); battery state, energy, health and cycles.

Package power needs the CPU energy counter, which is root-only by default as a side-channel mitigation;
`install.sh` lets your admin group read it (they can sudo anyway).

| Key | Action |
|---|---|
| `←` `→` / `h` `l` | pin a quieter or cooler fan level |
| `f` | follow the desktop power profile again |
| `i` / `Tab` | details view |
| `q` / `Esc` | quit |

The power profile is the desktop one (quick settings; CPU tuning via tuned); zfan doesn't set it. By default the
fans follow it like HP's own mode table does (quiet → auto, balanced → auto, performance → boost), shown as a hollow
marker. Moving the slider pins `quiet`, `auto` or `boost` regardless of the profile (solid marker).

| Fan mode | Fans |
|---|---|
| quiet | all three equal, ~3100–3700 RPM at idle, capped ~4300 RPM under load (lowers fan 3, raises 1 and 2) |
| auto | HP automatic curve |
| boost | boost curve, ~6100 / 6500 / 8500 RPM under load; same as auto when cool |

For scripts:

| Command | Effect |
|---|---|
| `zfan status` | one-shot status (also used automatically when output is not a terminal) |
| `zfan details` | one-shot details view |
| `zfan doctor` | check driver, boot setup, permissions, power profiles, fans, full speed and updates; prints a fix for each problem |
| `zfan fans follow\|quiet\|auto\|boost` | set the fan mode |
| `zfan max` / `zfan max off` | full fan speed on / off |
| `zfan --version` | print the version |

The desktop power-mode switch (tuned-ppd or power-profiles-daemon) drives the same profiles. `sensors` shows the fan speeds.

`zfan doctor` is the only command that goes online: it asks GitHub for the latest release (one request, 3 s timeout)
and also warns when the loaded driver and zfan are different versions. Update with `git pull && sudo ./install.sh`.

## Full speed is locked (for now)

Full speed means driving fans 1 and 3 to hardware maximum through the EC's host overrides (fan 2 has none). On BIOS
01.05.01 a BIOS driver locks those overrides before Linux starts, and nothing Linux can send unlocks them. The
driver checks at load and on resume whether the EC accepts an override; while it doesn't, `zfan max` refuses and
boost is the maximum. The dashboard leaves full speed out until it works.

The driver only ever writes "maximum" or "release" to the EC, never a slower speed, and the EC's emergency fan and
throttling protection stays active.

## Layout

- `driver/` — `hp_zbook_fury_fan` kernel module (platform_profile + hwmon)
- `cli/zfan` — TUI dashboard and script commands (Python 3, standard library only; honors `NO_COLOR`)
- `packaging/` — udev rule and modules-load config used by `install.sh`
- `tools/check-version.sh` — checks that every version string agrees (used by CI and releases)

## Versions and releases

The driver and `zfan` share one [semantic version](https://semver.org/), kept in three places: `VERSION` in
`cli/zfan`, `PACKAGE_VERSION` in `driver/dkms.conf` and `MODULE_VERSION` in the driver. To release:

1. Bump all three and add a `## [x.y.z]` section to [`CHANGELOG.md`](CHANGELOG.md).
2. `tools/check-version.sh` must print the new version.
3. Tag and push: `git tag vx.y.z && git push origin vx.y.z`.

The release workflow checks the tag against the version, then publishes a GitHub release with the changelog section
as notes and a source archive. CI builds the driver against Fedora's current kernel, checks the CLI on Python 3.10
and 3.13, and runs ShellCheck.

## License

[GPL-2.0-or-later](LICENSE).
