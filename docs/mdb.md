# MDB — Muse Developer Bridge

MDB is the watch's developer bridge: an ADB-style debug + control
console for AIWatchOS, built for hardware bring-up. It exists so the
host (Juno, on the Mac) can *see* the watch — pull the composed
framebuffer, read health, stream touch — and *drive* it — inject
taps, swipes, button presses, app switches — without a human finger.

This document is the **protocol contract**. The Mac-side host half
(CLI + MCP server) is a separate follow-on that implements against
this spec; anything marked **PLANNED** is not in the firmware yet.

All examples in this document use placeholders (`HomeWiFi`,
`your-mac.your-tailnet.ts.net`, `192.0.2.10`). Real network names,
addresses, and credentials never appear in this repository.

## Transports

| Transport | Status | Notes |
|---|---|---|
| USB serial (USB Serial JTAG) | **Implemented** | 115200 8N1 nominal (USB CDC ignores baud). macOS device: `/dev/cu.usbmodem<NNNN>`. Same port as the boot log. |
| Hermes gadget link | PLANNED | Same line grammar, framed by the link's message channel. |
| Muse device link | PLANNED | Same line grammar. |

The grammar is deliberately transport-shaped: one command per line,
responses are line-oriented except for framed binary blocks, so the
same parser works over any byte stream.

## Gating / security

- Compiled in only with `CONFIG_AIWATCHOS_MDB_ENABLE`
  (`main/Kconfig.projbuild`), **default ON for bring-up builds**.
- Turn it OFF for any build that leaves the bench: anything that can
  open the serial port can read the screen and inject input.
- **Redaction rule:** MDB never emits secret *values*. Health reports
  config *presence* as booleans only. (Pre-existing firmware log lines
  captured by `mdb logs` may contain non-secret operational detail
  such as the configured network name in plain text — treat captured
  logs as sensitive anyway.)

## Line protocol

- Send: ASCII command line terminated by `\n` (CRLF accepted).
- Every command starts with the prefix `mdb `. Lines without the
  prefix are ignored silently (the port is shared with the log
  console and human typing).
- Immediate replies are single lines: `OK ...` or `ERR ...`.
- Block replies are framed: a `<NAME>_BEGIN` line (some carry a byte
  count), the payload, and a matching `<NAME>_END` line.
- The firmware prints `MDB ready — 'mdb help'` once at boot.
- `mdb help` lists the command set on-device.

## Commands — inspection

### `mdb screenshot`
Streams the current composed framebuffer (buffer-truth: exactly what
the OS drew, before any panel/flush effects).

```
> mdb screenshot
< FBDUMP_BEGIN 410 502 RGB565 411640
< ...411640 raw bytes...
< FBDUMP_END
```

- Header: `FBDUMP_BEGIN <width> <height> <format> <byte-count>`.
- Payload: row-major, top row first, no padding. Pixel format
  `RGB565`: one little-endian uint16 per pixel **in the
  framebuffer's canonical host order** — i.e. the value the composer
  wrote (`0xF800` = red). The byte-swap the CO5300 panel requires
  happens later, in the flush path, and is *not* applied here: a
  correct dump proves composition; comparing it against a photo of
  the glass isolates flush/panel faults.
- Log output is suppressed for the duration of the dump so no text
  interleaves into the binary payload.
- Caveats: the buffer is live — a dump taken mid-animation can tear
  (one frame's top, the next frame's bottom). The dump works while
  the panel sleeps (it is the composed buffer, not a panel readback).

### `mdb health`
One-shot state snapshot as `key=value` lines between
`MDB_HEALTH_BEGIN` / `MDB_HEALTH_END`:

| Field | Meaning |
|---|---|
| `uptime_s` | seconds since boot |
| `boot_count` | boots since cold power-on (RTC-retained) |
| `panic_count` | of those, how many followed a panic reset |
| `reset_reason` | `poweron`/`panic`/`task_wdt`/`int_wdt`/`brownout`/`sw`/`ext`/`deepsleep`/`sdio`/`other` |
| `heap_free`, `heap_min` | internal SRAM free / all-time minimum free (bytes) |
| `psram_free` | PSRAM free (bytes) |
| `battery_pct` | 0–100, or 255 when no battery reading |
| `battery_mv`, `charging` | millivolts, 0/1 |
| `wifi_state` | `off`/`no_credentials`/`connecting`/`connected`/`failed` (the net layer's own names) |
| `wifi_ip` | dotted quad when connected, else `-` |
| `wifi_rssi` | dBm when connected, else 0 |
| `muse_link` | Muse link state name from the net layer |
| `hermes_paired` | 0/1 |
| `hermes_endpoint` | `lan` (this build has a single, local endpoint) |
| `hermes_health` | the Hermes app's own one-line health string |
| `cfg_wifi`, `cfg_hatch_token`, `cfg_vm_token`, `cfg_tts_key` | **presence booleans only — never values** |
| `weather_status` | `unconfigured`/`connecting`/`ok`/`fetch_failed`/`unknown` |
| `weather_age_s` | age of the last-good weather snapshot; -1 when none or when the board clock is undisciplined |
| `current_app` | foreground app id (see `mdb goto`) |
| `asleep` | panel asleep, 0/1 |

### `mdb logs`
Dumps the recent log ring between `MDB_LOGS_BEGIN <bytes>` and
`MDB_LOGS_END`. The ring is ~32 KB in PSRAM, fed by an
`esp_log_set_vprintf` hook installed at the top of `app_main` (the
original console output is chained, unchanged). Oldest first. Logs
from before MDB starts (ROM + bootloader + earliest init) are not in
the ring.

### `mdb touch`
Streams touch-controller readings until any byte is received:

```
> mdb touch
< MDB_TOUCH_BEGIN send-any-byte-to-stop
< TOUCH 1 205 300        (n_points, x, y of first point, HAL coords)
< TOUCH_UP
< MDB_TOUCH_END
```

Poll cadence ~50 ms; a line is printed on change and on every poll
while a point is down. Coordinates are post-transform HAL
coordinates (410×502 space). Note: while streaming, the MDB task and
the main loop both read the controller; a press can be observed by
both. Debug-grade by design.

### `mdb panic`
Between `MDB_PANIC_BEGIN` / `MDB_PANIC_END`: `reset_reason`,
`boot_count`, `panic_count` (RTC no-init retention; there is no
coredump partition in this build — counters + reset reason only).

## Commands — control (input injection)

Injection rides the OS's existing input seam: the console task fills
a small frame script, and the main loop consumes it at its normal
input sampling point — *before* the display-power policy and the
touch synthesizer — so injected input is indistinguishable from
physical input downstream. While a sequence is active it replaces
physical touch for those frames. Latency is one frame (~40 ms).

| Command | Effect | Reply |
|---|---|---|
| `mdb tap <x> <y>` | Press at (x, y) for ~120 ms, then release | `OK tap` |
| `mdb swipe <x1> <y1> <x2> <y2> [ms]` | Interpolated drag (default 300 ms, clamped to the frame script) | `OK swipe` |
| `mdb press boot` | BOOT button (mask bit 0) for ~200 ms | `OK press boot` |
| `mdb press pwr` | PWR side key (mask bit 1) for ~200 ms — a short press; toggles sleep per the power policy | `OK press pwr` |
| `mdb goto <app-id>` / `mdb open <app-id>` | Switch foreground app via AppManager (no fake touches) | `OK goto queued` |
| `mdb wake` | If asleep: a centre tap — wakes the panel; the power policy swallows the waking press, so it cannot hit UI | `OK wake` |
| `mdb sleep` | If awake: a PWR short press, which the power policy turns into sleep on release | `OK sleep` |
| `mdb reboot` | `esp_restart()` after a short drain delay | `OK rebooting` |

App ids: `clock`, `launcher`, `muse`, `hermes`, `settings`,
`notifications`, `weather`.

**PLANNED, not implemented:** `mdb press ptt` — PTT is not a
hardware button in this firmware (it is an on-screen target in the
Muse/Hermes apps, and BOOT doubles as the physical attention key);
use `mdb tap` on the PTT button, or `mdb press boot`. The command
currently returns an `ERR` explaining this.

Malformed commands return a single `ERR ...` line with usage.

## Mac-side capture recipes

### Screenshot → PNG

```python
import serial, numpy as np
from PIL import Image

port = "/dev/cu.usbmodem<NNNN>"        # placeholder; ls /dev/cu.usbmodem*
ser = serial.Serial(port, 115200, timeout=10)
ser.write(b"mdb screenshot\n")
# scan to the header line
while True:
    line = ser.readline().decode(errors="replace").strip()
    if line.startswith("FBDUMP_BEGIN"):
        w, h, fmt, n = line.split()[1], line.split()[2], line.split()[3], int(line.split()[4])
        break
raw = b""
while len(raw) < n:
    raw += ser.read(n - len(raw))
px = np.frombuffer(raw, dtype="<u2").reshape(int(h), int(w)).astype(np.uint32)
r = ((px >> 11) & 0x1F) * 255 // 31
g = ((px >> 5) & 0x3F) * 255 // 63
b = (px & 0x1F) * 255 // 31
Image.fromarray(np.dstack([r, g, b]).astype("uint8"), "RGB").save("watch.png")
```

Buffer-truth vs panel-truth: if the PNG is correct but the glass is
wrong, the fault is in the flush/panel path; if the PNG is wrong,
the fault is in composition. That split is the bridge's reason to
exist.

### Health / logs / touch

```bash
# any terminal works; e.g. with pyserial's miniterm or:
python3 -c "import serial,sys; s=serial.Serial('/dev/cu.usbmodem<NNNN>',115200,timeout=5); s.write(b'mdb health\n'); print(s.read(4096).decode(errors='replace'))"
```

## Implementation map

| Piece | File |
|---|---|
| Kconfig gate | `main/Kconfig.projbuild` (`AIWATCHOS_MDB_ENABLE`) |
| Console task, log ring, dump/health/touch/panic | `main/mdb_console.cpp` |
| Injection hooks + main-loop seam | `main/mdb.hpp`, `main/app_main.cpp` |
| Weather snapshot source | `aiwatchos_weather::weather_service_snapshot()` |

