# Replay 2026 Badge Docs Site

This directory is the static public documentation site for the Replay 2026
Badge. Source files are plain HTML and split CSS: no client-side JavaScript and
no package manager. Vercel runs the repository docs build script to emit
minified deploy output in `docs-dist/`.

Open [`index.html`](index.html) directly in a browser to preview the site.

## Files

- `index.html`: docs home page.
- `basics.html`: first-use badge overview.
- `get-started.html`: flashing, JumperIDE, and first app workflow.
- `developer-guide.html`: MicroPython developer guide.
- `api-reference.html`: badge MicroPython API summary.
- `apps.html`: app structure and filesystem notes.
- `hardware.html`: hardware overview and source package pointers.
- `hacks.html`: advanced topics, IR, and gotchas.
- `css/`: split source styles that are bundled per page during deploy builds.
- `assets/screenshots/`: captured badge OLED screenshots used by repo docs.

## Build

Run the deploy build locally with:

```bash
node ../scripts/build-docs.mjs
```

The generated `../docs-dist/` directory is ignored by git.

## Badge Screenshots

Use the firmware helper to capture the attached badge OLED as a PNG:

```bash
cd ../firmware
~/.platformio/penv/bin/python scripts/capture_oled_fb.py \
  --out ../docs/assets/screenshots/my-screen.png
```

If multiple badges are connected, run the helper with `--list-ports`, then pass
the desired `--port`.

## WiFi Diagnostics

Badges can join 2.4 GHz WiFi networks only. For connection debugging, use
MicroPython's `network.WLAN` module from the raw REPL to print visible SSIDs,
channels, RSSI, and auth modes:

```python
import network, time

wlan = network.WLAN(network.STA_IF)
wlan.active(True)
time.sleep(1)

for ssid, bssid, channel, rssi, authmode, hidden in wlan.scan():
    name = ssid.decode("utf-8", "ignore")
    print(name, "channel", channel, "rssi", rssi, "auth", authmode)

wlan.active(False)
```

The scan output does not include passwords.
