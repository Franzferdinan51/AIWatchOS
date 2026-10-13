# AIWatchOS

An **AI-first watch OS** for the [Waveshare ESP32-S3-Touch-AMOLED-2.06](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06)
development board (ESP32-S3R8, 410×502 CO5300 AMOLED, FT3168 touch).

## What it is

AIWatchOS is a modular watch operating system built on ESP-IDF that treats AI
voice interaction as first-class apps. It provides:

- **Hardware abstraction layer (HAL)** — drivers for the 410×502 CO5300 QSPI AMOLED,
  FT3168 capacitive touch (I²C addr `0x38`), ES8311 speaker + ES7210 microphone codecs
  via I²S, AXP2101 power management, and a PCF85063 RTC (I²C addr `0x51`)
  that keeps wall-clock time on battery. All pin assignments are derived from the
  [muse-gadget-206](https://github.com/Franzferdinan51/muse-gadget-206) board config.
- **App framework** — an `AppManager` that maintains a registry of installed apps,
  dispatches tick/render/touch events to the foreground app, and supports touch-driven
  app switching. Each app implements simple C-style callbacks (`init`, `tick`,
  `render`, `on_touch`).
- **Integrated AI apps**:
  - **`muse`** — push-to-talk voice app adapted from
    [muse-gadget-206](https://github.com/Franzferdinan51/muse-gadget-206):
    on-device capture, IMA-ADPCM encoding and an offline outbox, plus a
    real transport (`components/aiwatchos_net`: WiFi STA + SNTP, WSS +
    Noise XX to the Hatch VM, account/VM token resolution, queued turns
    drained FIFO when linked). The transport ships **unconfigured** —
    set WiFi + Hatch credentials via Kconfig or the NVS setup seam, or
    the app stays in offline-outbox mode. Replies arrive as text events
    and render on screen; the MP3 playback path is bench-verified, but
    the host protocol does not send audio replies.
  - **`hermes`** — **placeholder**: the app shell renders its screens and
    honestly reports `LINK OFFLINE`; it does not speak the HermesGadget
    protocol yet. The host side is ready (the HermesGadget plugin hub
    runs on the gateway, pairing + HMAC + chat proven end-to-end), and a
    protocol-v1 watch client is in progress on a separate branch.
- **Standard smartwatch features**:
  - Clock face with hour/minute/second hands (Bresenham line drawing on RGB565 framebuffer).
    Time comes from the PCF85063 RTC, disciplined by SNTP when online, with a
    build-time epoch as the never-set fallback; the displayed time-of-day
    applies a configurable UTC offset (default EDT, `AIWATCHOS_TZ_OFFSET_MIN`)
  - Battery status display in a top status bar (AXP2101 fuel gauge readout)
  - Touch-driven launcher UI with edge-swipe app switching and tap-to-launch grid
  - Settings page: brightness/volume/DND toggle/theme/about-device, touch-adjustable controls optimized for finger-friendly targets on the 410×502 AMOLED
  - All app text renders in a shared 5×7 bitmap font (no placeholder blocks)
  - Display sleep after 30 s idle (wake on touch/BOOT/PWR; the waking touch is
    swallowed), PWR short-press sleep/wake toggle
  - Notifications display: stored notifications in a bounded PSRAM buffer
    (max 16); rendering/scroll quirks are tracked in `docs/ui/UI_UX_AUDIT.md`
  - Weather app: temperature/condition/humidity layout — **currently shows
    placeholder data**; live network fetch is not wired yet

## Hardware

| Part | Detail |
|---|---|
| MCU | ESP32-S3R8, dual-core 240 MHz, 8 MB octal PSRAM, 32 MB flash |
| Display | 2.06" AMOLED, 410×502, CO5300 over QSPI (CS=GPIO12, SCK=GPIO11, D0-D3=GPIO4-7) |
| Touch | FT3168 capacitive, I²C addr `0x38` (SDA=GPIO15, SCL=GPIO14, INT=GPIO38) |
| Audio | ES8311 speaker + ES7210 dual mic via I²S (MCLK=GPIO16, BCLK=GPIO41, WS=GPIO45, DOUT=GPIO42, DIN=GPIO40) |
| Power | AXP2101 PMIC (I²C addr `0x34`), 3.7V LiPo with MX1.25 connector |

## Project structure

```
AIWatchOS/
├── CMakeLists.txt              # Top-level ESP-IDF project definition
├── sdkconfig.defaults          # Build config targeting ESP32-S3, PSRAM, CO5300 AMOLED
├── partitions.csv              # Partition table (factory + 2 OTA slots + assets)
├── main/
│   └── app_main.cpp            # Entry point: init HAL, register apps, event loop
├── components/aiwatchos_os/    # OS core: HAL drivers, AppManager, clock face, launcher UI
│   ├── include/aiwatchos/      # Public headers (app.hpp, hal.hpp, app_manager.hpp)
│   ├── src/                    # Driver implementations + UI renderers
│   ├── idf_component.yml       # Managed deps (touch, CO5300, codecs)
│   └── test/app_registry_test.cpp  # Host unit tests (see below)
├── components/noise_core/      # Vendored Noise crypto core (upstream muse-gadget-206)
├── components/minimp3/         # Vendored MP3 decoder for spoken replies
├── components/aiwatchos_net/   # WiFi/SNTP + Muse transport (WSS + Noise XX)
├── docs/ui/                    # UI/UX audit, design tokens, resource budget
├── apps/muse/                  # Muse voice app: PTT capture, ADPCM turn buffer, reply playback
│   ├── src/muse_adpcm.c        # Ported IMA-ADPCM encoder (upstream muse-gadget-206)
│   └── test_data/test_reply.mp3  # Bench reply fixture for host tests
└── apps/hermes/                # Hermes AI agent app adapter
```

## Building

Requires the [ESP-IDF toolchain](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/)
(verified with ESP-IDF v5.3.6; the first build downloads managed components, so it needs network).

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
```

## Host tests

`components/aiwatchos_os/test/app_registry_test.cpp` runs on any C++17 host and
drives the real shipped code (AppManager, drivers, voice pipeline, Noise
crypto, MP3 decode). The exact command is documented at the top of the file;
the Noise test needs PSA headers (e.g. `brew install mbedtls`) and reports
SKIPPED without them. Run it from the repo root.

## Status / known limitations

- Hermes watch app is a placeholder (see above); protocol-v1 client in progress.
- Weather fetches REAL current conditions from Open-Meteo (no API key)
  via components/aiwatchos_weather: Kconfig location (default Huber
  Heights, Ohio), 30-minute refresh, honest NO DATA YET / OFFLINE /
  STALE states — placeholder data is gone.
- No on-watch settings UI for network credentials yet (Kconfig/NVS only).
- Spoken replies are not wired: host replies are text; the MP3 path awaits a TTS source.
- UI quality gaps (typography, launcher labels, flush cost) are catalogued in
  `docs/ui/UI_UX_AUDIT.md`; the gloss-blue redesign lives in
  `docs/ui/DESIGN_TOKENS.md` + `docs/ui/IMPLEMENTATION_PLAN.md`, and the
  resource budget every build must respect is `docs/ui/RESOURCE_BUDGET.md`.

## License

Apache 2.0 — see [LICENSE](LICENSE).
