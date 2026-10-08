# zfan

Fan control for the HP ZBook Fury G1i 16" on Linux: a kernel driver and a terminal dashboard.

```
  control      ● power mode    ○ manual                    performance → boost

               Quieter                           Cooler
  fans         ─────────────────────────────────────△──
               quiet             auto             boost

               temp   hp     power    limit      clock
  cpu          95°    78°    49 W     80 W       3.2 GHz    throttling 86%
  gpu          55°    52°    8 W      95 W       P8
```

## Requirements

- HP ZBook Fury G1i 16" (board `8DE2`; check with `cat /sys/class/dmi/id/board_name`). Tested on that model with
  BIOS 01.05.01 only. Other ZBook Fury G1i models and HP ZBooks may work but are untested: `install.sh` refuses
  them unless you run `sudo ./install.sh --force`, at your own risk, since the driver writes HP's EC fan registers.
- Linux 6.14+ with headers for the running kernel, DKMS, gcc, make and Python 3.10+
  - Fedora: `sudo dnf install dkms kernel-devel-$(uname -r) gcc make python3`
  - Debian / Ubuntu: `sudo apt install dkms linux-headers-$(uname -r) build-essential python3`
  - Arch: `sudo pacman -S dkms linux-headers base-devel python`
- Optional: a power-profile service (`tuned-ppd` or `power-profiles-daemon`) so the fans follow the desktop power
  mode, and `nvidia-smi` for GPU power and clocks

## Install

```sh
git clone https://github.com/piexelated/zfan.git
cd zfan
sudo ./install.sh
zfan
```

`install.sh` checks the requirements first and tells you what's missing. With Secure Boot on, enroll DKMS's signing
key once: `sudo mokutil --import /var/lib/dkms/mok.pub`, reboot and choose *Enroll MOK*. Run `zfan doctor` if
anything looks off.

## Use

The `control` row shows who sets the fan level:

- **power mode** (default): the fans follow the desktop power mode. Power saver and balanced give auto, performance
  gives boost.
- **manual**: the level you pick stays, whatever the power mode.

| Key | Action |
|---|---|
| `f` | switch between power mode and manual |
| `←` `→` | in manual: quieter or cooler |
| `i` | details: power limits, clocks, HP and EC sensors, battery |
| `q` | quit |

| Fan level | Fans |
|---|---|
| quiet | capped around 4300 RPM |
| auto | HP's automatic curve |
| boost | up to ~6100 / 6500 / 8500 RPM under load; same as auto when cool |

The choice survives reboots and updates.

## Commands

| Command | Effect |
|---|---|
| `zfan status` / `zfan details` | print the dashboard once |
| `zfan fans follow\|quiet\|auto\|boost` | set the fan mode |
| `zfan doctor` | check the setup and print a fix for each problem |
| `zfan update` | install the latest release |
| `zfan uninstall` | remove the driver and zfan |

Only `doctor` and `update` go online.

## Releasing

Bump the version in `cli/zfan`, `driver/dkms.conf` and the driver's `MODULE_VERSION`, add it to
[CHANGELOG.md](CHANGELOG.md), then push a `vX.Y.Z` tag. CI checks that the versions agree and publishes the release.

## License

[GPL-2.0-or-later](LICENSE)
