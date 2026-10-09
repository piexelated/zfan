#!/usr/bin/env python3
"""Render the README screenshots (docs/images/*.png) with zfan's own drawing code.

Run on a laptop with the driver loaded: python3 tools/screenshots.py
Needs google-chrome (headless) and Pillow. The details and doctor shots are live readings; the dashboard shots
put example temperatures and fan speeds on top of them so every state looks the same on every run.
"""
import contextlib
import copy
import html
import importlib.machinery
import importlib.util
import io
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
OUTPUT_DIR = ROOT / "docs" / "images"
TERMINAL_COLUMNS = 92
TERMINAL_ROWS = 60
BROWSER_SIZE_PX = (1400, 1800)
SCALE = 2
CHROME = "google-chrome"
CHROME_TIMEOUT_S = 60
RATE_SAMPLE_S = 2
ANSI_COLOR = re.compile(r"\x1b\[(1;)?38;2;(\d+);(\d+);(\d+)m(.*?)\x1b\[0m", re.S)

PAGE = """<!doctype html><meta charset="utf-8"><style>
html, body {{ margin: 0; background: transparent; }}
.window {{ display: inline-block; margin: 48px; border-radius: 12px; overflow: hidden;
  background: #14161b; border: 1px solid #2a2e37; box-shadow: 0 24px 60px rgba(0, 0, 0, .45); }}
.bar {{ height: 34px; display: flex; align-items: center; padding: 0 14px; gap: 8px; background: #1b1e25;
  border-bottom: 1px solid #262a33; font: 13px system-ui, sans-serif; color: #7a828e; }}
.dot {{ width: 12px; height: 12px; border-radius: 50%; }}
.title {{ flex: 1; text-align: center; margin-right: 52px; }}
pre {{ margin: 0; padding: 14px 18px 18px; font: 14px/1.45 "DejaVu Sans Mono", monospace; color: rgb(222, 226, 232); }}
</style><div class="window"><div class="bar"><span class="dot" style="background:#ee766c"></span>
<span class="dot" style="background:#f0b660"></span><span class="dot" style="background:#76b6c4"></span>
<span class="title">{title}</span></div><pre>{body}</pre></div>"""


def load_zfan():
    loader = importlib.machinery.SourceFileLoader("zfan", str(ROOT / "cli" / "zfan"))
    module = importlib.util.module_from_spec(importlib.util.spec_from_loader("zfan", loader))
    loader.exec_module(module)
    module.COLOR = True
    return module


def ansi_to_html(text):
    def span(match):
        bold, red, green, blue, content = match.groups()
        weight = "font-weight:700;" if bold else ""
        return f'<span style="color:rgb({red},{green},{blue});{weight}">{html.escape(content)}</span>'

    parts, last = [], 0
    for match in ANSI_COLOR.finditer(text):
        parts.append(html.escape(text[last:match.start()]))
        parts.append(span(match))
        last = match.end()
    parts.append(html.escape(text[last:]))
    return "".join(parts)


def example_state(zfan, base, fan_mode, power, rpms, targets, cpu_celsius, hp_celsius, package_w, throttle, ghz,
                  gpu_awake):
    snapshot = copy.copy(base)
    snapshot.cpu = copy.copy(base.cpu)
    snapshot.fan_mode, snapshot.power = fan_mode, power
    snapshot.rpms, snapshot.targets = list(rpms), list(targets)
    snapshot.cpu.celsius, snapshot.cpu.hp_celsius = cpu_celsius, hp_celsius
    snapshot.cpu.package_w, snapshot.cpu.throttle_percent, snapshot.cpu.avg_ghz = package_w, throttle, ghz
    if gpu_awake:
        snapshot.gpu_state = "active"
        snapshot.gpu = zfan.GpuReading(["71", "88", "95", "95", "130", "P0", "2280", "9001", "93", "0x0"])
    else:
        snapshot.gpu_state, snapshot.gpu = "suspended", None
    snapshot.hp_zones = dict(base.hp_zones, cpu=hp_celsius, gpu=69 if gpu_awake else 44)
    return snapshot


def dashboard(zfan, snapshot, view):
    screen = object.__new__(zfan.Dashboard)
    screen.snapshot, screen.view, screen.toast = snapshot, view, zfan.Toast()
    return screen.render(TERMINAL_COLUMNS, TERMINAL_ROWS)


def doctor_output(zfan):
    # Show what a published install sees; the live check can't reach a private repository.
    zfan.latest_release = lambda: zfan.Release(zfan.VERSION, {})
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        zfan.print_doctor()
    return output.getvalue()


def shots(zfan):
    hardware = zfan.FanHardware()
    # Package power and throttling are rates: the first reading only starts the counters.
    zfan.Snapshot(hardware)
    time.sleep(RATE_SAMPLE_S)
    base = zfan.Snapshot(hardware)
    under_load = example_state(zfan, base, "follow", "performance", (4630, 4980, 6240), (6100, 6450, 8400),
                               91, 79, 62, 0, 4.3, gpu_awake=True)
    pinned_quiet = example_state(zfan, base, "quiet", "performance", (3480, 3610, 3700), (3480, 3610, 3700),
                                 64, 61, 18, 0, 3.1, gpu_awake=False)
    return {
        "dashboard": ("zfan", dashboard(zfan, under_load, zfan.View.MAIN)),
        "manual": ("zfan", dashboard(zfan, pinned_quiet, zfan.View.MAIN)),
        "details": ("zfan · details", dashboard(zfan, base, zfan.View.DETAILS)),
        "doctor": ("zfan doctor", doctor_output(zfan)),
    }


def screenshot(page, png):
    width, height = BROWSER_SIZE_PX
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                    f"--force-device-scale-factor={SCALE}", "--default-background-color=00000000",
                    f"--window-size={width},{height}", f"--screenshot={png}", page.as_uri()],
                   check=True, capture_output=True, timeout=CHROME_TIMEOUT_S)
    with Image.open(png) as image:
        image.crop(image.getbbox()).save(png, optimize=True)


def main():
    zfan = load_zfan()
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as workdir:
        for name, (title, text) in shots(zfan).items():
            page = Path(workdir) / f"{name}.html"
            page.write_text(PAGE.format(title=html.escape(title), body=ansi_to_html(text.strip("\n"))))
            png = OUTPUT_DIR / f"{name}.png"
            screenshot(page, png)
            print(png.relative_to(ROOT))


if __name__ == "__main__":
    sys.exit(main())
