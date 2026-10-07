# AIWatchOS

An **AI-first watch OS** for the [Waveshare ESP32-S3-Touch-AMOLED-2.06](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06)
development board (ESP32-S3R8, 410×502 CO5300 AMOLED, FT3168 touch).

## What it is

AIWatchOS is a modular watch operating system built on ESP-IDF that treats AI
voice interaction as first-class apps. It provides:

- **Hardware abstraction layer (HAL)** — drivers for the 410×502 CO5300 QSPI AMOLED,
  FT3168 capacitive touch (I²C addr `0x38`), ES8311 speaker + ES7210 microphone codecs
  via I²S, and AXP2101 power management. All pin assignments are derived from the
  [muse-gadget-206](https://github.com/Franzferdinan51/muse-gadget-206) board config.
- **App framework** — an `AppManager` that maintains a registry of installed apps,
  dispatches tick/render/touch events to the foreground app, and supports touch-driven
  app switching. Each app implements simple C-style callbacks (`init`, `tick`,
  `render`, `on_touch`).
- **Integrated AI apps**:
  - **`muse`** — wraps [muse-gadget-206](https://github.com/Franzferdinan51/muse-gadget-206)
    as a push-to-talk voice interaction app with Noise protocol encrypted WebSocket transport.
  - **`hermes`** — wraps [HermesGadget](https://github.com/Franzferdinan51/HermesGadget)
    as an AI agent conversation app (WebSocket-based, with on-screen reply display and OTA).
- **Standard smartwatch features**:
  - Clock face with hour/minute/second hands (Bresenham line drawing on RGB565 framebuffer)
  - Battery status display in a top status bar (AXP2101 fuel gauge readout)
  - Touch-driven launcher UI with edge-swipe app switching and tap-to-launch grid
  - Settings page: brightness/volume/DND toggle/theme/about-device, touch-adjustable controls optimized for finger-friendly targets on the 410×502 AMOLED
  - Notifications display: multi-line message rendering with vertical scroll via touch-drag (bounded PSRAM buffer of max 16 notifications)
  - Weather app: temperature/condition/humidity display centered on screen, no bitmaps to minimize memory usage

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
esp32-watch-os/
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
├── apps/muse/                  # Muse voice app: PTT capture, ADPCM turn buffer, reply playback
│   ├── src/muse_adpcm.c        # Ported IMA-ADPCM encoder (upstream muse-gadget-206)
│   └── test_data/test_reply.mp3  # Bench reply fixture for host tests
└── apps/hermes/                # Hermes AI agent app adapter
```

## Building

Requires the [ESP-IDF toolchain](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/)
(IDF 5.x; the first build downloads managed components, so it needs network).

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

## License

Apache 2.0 — see [LICENSE](LICENSE).
