# RESOURCE BUDGET — Waveshare ESP32-S3-Touch-AMOLED-2.06 (AIWatchOS + gadgets)

Read-only audit, 2026-10-13. Every number below comes from a build artifact
(.bin/.elf/.map via xtensa size + idf_size.py), an effective sdkconfig, or a
source line — cited inline. Estimates are labeled ESTIMATE. Unmeasured items
are listed at the end, with what would measure them.

Device: ESP32-S3, 512KB internal SRAM (usable DIRAM pool = 341,760 B after
cache/IRAM carve-outs), 8MB octal PSRAM, 32MB flash, 410x502 AMOLED, battery.

## BUDGET TABLE (limit vs current vs headroom)

| Resource | Limit | Current | Headroom | Verdict |
|---|---|---|---|---|
| AIWatchOS app image (integrated est.) | 2,097,152 (2MB OTA slot, power branch) | ~1.22–1.30 MB (est., §1) | ~800–880 KB (~40%) | SAFE |
| HermesGadget app image | 2,031,616 (its ota_0/1) | 1,228,352 (built .bin) | 803,264 (39.5%) | SAFE |
| muse-gadget-206 app image | 2,097,152 (its ota_0/1, 2M) | UNMEASURED — no .elf/.bin in build dir | ? | UNKNOWN — measure before flash-ready |
| DRAM static (net build, idf_size) | 341,760 DIRAM pool | 113,815 used (33.3%) | 227,945 static-free | OK |
| DRAM runtime free, connected idle (est.) | floor target >= 32,768 | ~90 KB (est., §2) | ~57 KB over floor | TIGHT |
| DRAM runtime free, worst spike (est.) | floor target >= 32,768 | ~50 KB (est., §2) | ~17 KB over floor | TIGHT |
| PSRAM committed | 8,388,608 | ~0.85 MB (§3) | ~7.5 MB | SAFE |
| FAT assets partition (UI fonts/icons) | 5,111,808 | plan ~170 KB (DESIGN_TOKENS §6) | ~4.9 MB | SAFE |
| Flash total (AIWatchOS layout, power br.) | 33,554,432 | ~11.1 MB mapped | ~21 MB unallocated | SAFE |
| Display flush time | 40 ms frame (25 FPS) | ~21 ms transfer (calc, §4) | ~19 ms for render+logic | TIGHT |
| Standby, connected idle (est.) | — | 13–27 h per 400 mAh (est., §5) | — | ESTIMATE only |

## 1. FLASH

Built .bin sizes (disk files; app-image size = file minus ~1.3KB esptool block):

| Tree | .bin | Delta vs baseline 349,296 |
|---|---|---|
| baseline c4dcbbd6 | 349,296 | — |
| font (aiwatchos-build) | 350,224 | +928 |
| power (aiwatchos-power) | 349,856 (image 349,824) | +528 |
| rtc (aiwatchos-rtc) | 350,752 | +1,456 |
| net (aiwatchos-net) | 1,139,744 (image 1,138,464) | +789,168 |
| hermesgadget-fixes | 1,228,352 hermes_gadget.bin | (own baseline 1,227,968; +384) |

Integrated AIWatchOS estimate: the four branch deltas are essentially
additive (they touch disjoint code; the net and rtc branches both add a
board_set_epoch_ms seam — dedupe at merge saves a few hundred bytes).
349,296 + 928 + 528 + 1,456 + 789,168 = ~1,141,000 B, plus:
- Hermes client (branch has NO build yet — tree still at c4dcbbd6):
  ESTIMATE +40–90 KB incremental on the integrated tree (esp_websocket_client
  + frame/HMAC code; mbedtls, cJSON, WiFi already linked by the net branch).
- UI implementation code (rounded/gradient primitives, render changes):
  ESTIMATE +30–60 KB. Font/icon ASSETS do not land in the app slot — they go
  to the 4.99MB FAT partition per DESIGN_TOKENS §6.
Total: ~1.22–1.30 MB vs 2,097,152 slot → ~40% headroom. SAFE. It would even
fit the OLD 0x1C0000 slots (1,835,008) — the 2MB slots are required not by
our size but by the Hermes OTA contract max (2,031,616).

Partition tables (partitions.csv):
- AIWatchOS power branch: factory 2MB @0x10000, ota_0 2MB @0x210000,
  ota_1 2MB @0x410000, ota_data 8K, FAT 0x4E0000 @0x620000, nvs 64K @0xB00000.
- AIWatchOS other branches: ota slots still 0x1C0000 — integration MUST take
  the power branch table.
- HermesGadget: ota_0/1 = 0x1F0000 (2,031,616) each; image uses 60.5%.
- muse-gadget-206 partitions.csv: ota_0/1 = 2M each (offset 0x10000 table,
  selected in esp32/sdkconfig.defaults). Its build dir contains only CMake
  scaffolding (CMakeFiles/config/logs) — the 6.0.1 build had produced no
  .elf/.bin/.map at audit time, so its image size and DRAM are UNMEASURED.

## 2. DRAM — the scarce resource

Measured statics (idf_size.py on the built .map):
- font/baseline class: DIRAM used 71,179 / 341,760 → 270,581 free at reset.
- net build: DIRAM used 113,815 (33.3%) → 227,945 free at reset.
  (.data 22,768 + .bss 5,240 + DIRAM-resident .text 85,807.)
- HermesGadget: DIRAM used 133,643 → 208,117 free at reset; gentler risk
  profile than AIWatchOS-net because its hub link is plain ws (no TLS spike).

Framebuffer — settled question: app_main.cpp allocates the full
410x502x2 = 411,640 B framebuffer with MALLOC_CAP_SPIRAM (device builds).
The 412KB-static-DRAM overflow in hal.hpp history is the HOST/test build
(static array on the build machine) and the pre-fix firmware that compiled
test paths. On device, DRAM exposure is only the DMA bounce buffer:
kBounceRows 20 x 410 x 2 = 16,400 B, MALLOC_CAP_DMA (display_driver.cpp).

Runtime internal-DRAM consumers, net build (from effective sdkconfig +
source; stack sizes are allocated from internal heap by xTaskCreate):
- Task stacks: main 3,584; idle x2 3,072; svc timer 2,048; esp_timer 3,584;
  system event 2,304; tcpip 3,072; WiFi task ~3,584 (IDF-internal);
  aiw_muse_link 24,576 (muse_link.cpp:943); aiw_time 3,072;
  pthread default 3,072 allowance. Total ≈ 52 KB.
- DMA bounce buffer 16,400 (above).
- TLS session (muse_link, esp_tls + mbedTLS): CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN
  =16384, OUT=4096, CONFIG_MBEDTLS_DYNAMIC_BUFFER NOT set → content buffers
  (~20.5 KB) are held for the whole session, plus ~8–12 KB ssl/cipher state,
  plus a handshake transient (peer cert chain parse) of ESTIMATE +8–16 KB.
  Steady ≈ 30 KB; handshake spike ≈ 45 KB. Plain mallocs <=16,384 B land in
  internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384), so TLS buffers
  are internal in practice. Only ONE TLS session exists by design (Hermes is
  plain ws) — a second concurrent TLS is the fastest route to OOM.
- WiFi/LWIP buffers: STATIC_RX 16, STATIC_TX 16, DYNAMIC_RX cap 32,
  RX_BA_WIN 16, LWIP WND/SND only 5,760 each (conservative, good).
  CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y directs these to PSRAM first, but
  "try" is not a guarantee — budget a worst-case internal allowance of
  ~30–55 KB (static RX+TX alone are ~50 KB if both land internal).
- LWIP sockets (max 10), pbuf/mbox (TCPIP_RECVMBOX 32), queues, misc heap,
  fragmentation: allowance ~20 KB.
- CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768 carves a 32 KB internal-only
  reserve out of the heap — general allocations cannot use the last 32 KB;
  effective general headroom is tighter than the raw free figure.

ESTIMATED runtime free DRAM (net build):
- Connected idle: 227,945 − ~52K stacks − 16.4K bounce − ~30K TLS steady
  − ~30K WiFi/LWIP internal mid-case − ~20K misc ≈ ~90 KB free.
- Worst spike (TLS (re)handshake during a turn upload + WiFi burst, WiFi
  buffers internal-heavy): ≈ ~50 KB free, with TLS needing ~16 KB
  CONTIGUOUS for the in-content buffer — fragmentation, not just totals,
  is the failure mode.
Verdict: TIGHT but SAFE for the current feature set. The margin is real but
it is spent the moment anything new allocates big in internal DRAM.

Hermes client note (in flight): plain ws + cJSON + HMAC only — no second
TLS, small frames; its DRAM risk is low IF its buffers follow the same
PSRAM-prefer discipline (see red flag 2).

## 3. PSRAM

Config (identical in every AIWatchOS branch sdkconfig.defaults):
SPIRAM=y, MODE_OCT, TYPE_ESPPSRAM64 (8MB), BOOT_INIT, USE_MALLOC,
FETCH_INSTRUCTIONS=y, RODATA=y, XIP_FROM_PSRAM=y (code+rodata execute from
PSRAM-mapped cache — no large RAM copies), ALLOW_BSS_SEG_EXTERNAL_MEMORY=y,
TRY_ALLOCATE_WIFI_LWIP=y, MALLOC_ALWAYSINTERNAL=16384,
MALLOC_RESERVE_INTERNAL=32768.

Committed PSRAM (net build):
- .ext_ram.bss static: 14,068 (idf_size, External RAM .bss).
- Framebuffer: 411,640.
- Muse turn buffer: kTurnMaxAdpcmBytes = 16000*30/2 = 240,000 (PSRAM,
  muse_adapter.cpp).
- Subscribe stream line buffer: up to kMaxSubLine 128 KB, reply buffer
  4 KB, base64 note chunks 8 KB staging — all via psram_alloc()
  (heap_caps_malloc_prefer SPIRAM-first) in muse_link.cpp.
- WiFi/LWIP PSRAM-first buffers: variable, ~50–100 KB under load (est.).
Total ≈ 0.85 MB of 8 MB → ~7.5 MB free. SAFE, by a wide margin.

Still in SRAM that should move / stay disciplined (recommendations, §6):
the psram_alloc FALLBACK to internal (red flag 2), any future image-decode
or AA-glyph cache (must be PSRAM per DESIGN_TOKENS rule: glyph cache
<=64 KB PSRAM), OTA download staging (stream to the OTA partition with
esp_ota_write in 4 KB chunks per the Hermes contract — never stage a
whole image in heap).

## 4. CPU / LOOP

- Frame period 40 ms (25 FPS, app_main kFrameMs). Full-screen flush moves
  411,640 B from PSRAM through the 16,400 B DMA bounce in 26 chunks over
  QSPI at pclk 40 MHz quad → raw transfer 3.29 Mbit / 160 Mbit/s ≈ 20.6 ms,
  i.e. ~half the frame budget is display plumbing before render cost.
  PSRAM is shared: framebuffer reads + render writes + XIP instruction
  fetch all contend for the same octal bus.
- New background work: aiw_muse_link task runs at priority 5 — ABOVE the
  main/render task (default priority 1) — so TLS/subscribe bursts can
  preempt rendering; aiw_time (SNTP discipline) priority 4, negligible load.
- Audio is polled inside the app tick (no dedicated audio task exists in
  the tree) → any blocked/stretched frame is a direct audio-jitter risk
  during PTT capture/playback.
- The power branch eliminates the whole cost when asleep: asleep loop skips
  tick/render/flush entirely (verified in app_main.cpp) and polls
  touch/buttons at frame rate; wake latency <= 40 ms.
Flags: static screens (clock face) should move to dirty-region/partial
flush — that is the single biggest CPU + power win available; UI gloss
must be row-banded LUT work (DESIGN_TOKENS already specifies this:
<=4 KB SRAM, per-line staging) and never per-pixel PSRAM read-modify-write
across the full screen per frame.

## 5. POWER

Config facts: CONFIG_PM_ENABLE is NOT set (no DFS, no automatic light
sleep). No esp_wifi_set_ps call exists in the net source → WiFi power save
stays at the esp_wifi default (WIFI_PS_MIN_MODEM: modem sleeps between
DTIM beacons). The net branch deliberately keeps the Muse session open
with 20 s keepalive pings and NO idle disconnect → the radio wakes for a
TLS ping at least every 20 s forever, and the TLS session (and its ~30 KB
of buffers) never releases.

ESTIMATES (no battery capacity or current draw exists in any repo
artifact; ranges are ESP32-S3-class engineering estimates — runtime
scales linearly: hours = cell mAh / draw mA):
- Screen on, WiFi connected, rendering at 25 FPS: ~60–100 mA
  → per 400 mAh of cell: ~4–6.5 h.
- Screen off (display-sleep branch), WiFi associated, modem sleep,
  20 s TLS keepalive: ~15–30 mA → per 400 mAh: ~13–27 h standby.
- The display-sleep branch removes the screen (~20–50 mA class) and all
  render/flush CPU; it does NOT reduce the network floor — WiFi + TLS
  keepalive dominate asleep draw.
Levers if standby disappoints (post-arrival, measured first): longer
keepalive while asleep, WIFI_PS_MAX_MODEM, PM_ENABLE + automatic light
sleep. None are free: each trades link latency/reconnect behavior.

## 6. RED FLAGS + REQUIRED TRIMS (ranked)

1. TLS handshake is the DRAM peak event (~45 KB spike incl. cert parse)
   and can coincide with a turn upload + WiFi burst → worst-case free
   ~50 KB (est.). REQUIRED: never two concurrent TLS sessions (Hermes is
   plain ws — keep it that way); do not start a (re)handshake mid-upload
   if it can be sequenced; enable CONFIG_MBEDTLS_DYNAMIC_BUFFER to
   release the 16 KB in-content buffer between records (saves ~12–16 KB
   steady-state) and re-measure. Do NOT shrink IN_CONTENT_LEN blindly —
   16 KB is needed for cert chains and full-size records.
2. psram_alloc() in muse_link.cpp falls back to internal RAM
   (heap_caps_malloc_prefer ..., MALLOC_CAP_DEFAULT). Its largest user is
   the 128 KB subscribe line buffer: if a PSRAM allocation ever fails,
   the fallback attempts 128 KB internal and detonates the heap.
   REQUIRED at integration: buffers >16 KB must be PSRAM-STRICT
   (fail closed, drop the line — not the device).
3. aiw_muse_link stack = 24,576 B internal — the largest single stack in
   the system. REQUIRED: capture uxTaskGetStackHighWaterMark on hardware
   during a full turn cycle and right-size (every 4 KB saved is 8% of the
   worst-case margin). Same check for the Hermes client task when it lands.
4. CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 means ANY plain malloc
   <=16 KB silently lands in DRAM. REQUIRED integration rule: every new
   buffer >4 KB uses explicit caps (SPIRAM-prefer for data, DMA only
   where hardware needs it). Code-review checklist item, not a hope.
5. UI implementation budgets (from DESIGN_TOKENS, validated against the
   numbers above — they fit, hold them as hard caps): font+icon+shadow
   assets <=256 KB flash (plan ~170 KB) in the FAT partition, NOT the app
   slot; glyph/text cache <=64 KB PSRAM only; total UI SRAM <=4 KB;
   no full-screen intermediate buffer in SRAM ever; gradients row-banded.
   Addition from this audit: AA blending is PSRAM read-modify-write on
   the framebuffer — with flush already ~21 ms of the 40 ms frame,
   full-screen per-pixel effects will break 25 FPS; keep effects banded
   or cached as pre-blended sprites (in the 64 KB PSRAM cache budget).
6. muse-gadget-206 is UNMEASURED: no .elf/.bin/.map existed in its build
   dir at audit time (6.0.1 build incomplete). LVGL builds of this class
   run large; against its 2M slots it may be TIGHT. REQUIRED: the
   completed 6.0.1 build must produce an idf.py size report (image vs
   2,097,152 slot; DIRAM used/remain like §2) before the gadget is called
   flash-ready.
7. First-boot measurement plan (converts this audit's estimates into
   truth on the real unit): log heap_caps_get_free_size +
   largest_free_block for INTERNAL and SPIRAM at (a) boot, (b) post-WiFi
   join, (c) post-TLS handshake, (d) during a PTT upload, (e) after 1 h
   connected idle (leak/fragmentation check). TLS needs ~16 KB
   contiguous — largest-block is the number that predicts handshake OOM.
8. Power floor is set by choices, not physics: 20 s TLS keepalive + no
   idle disconnect + PM disabled → standby ~13–27 h per 400 mAh (est.).
   Acceptable for a desk/bench watch; if Duckets wants multi-day
   standby, the keepalive-when-asleep interval is the first knob.

## UNMEASURED (and what measures it)
- muse-gadget-206 image size + DRAM statics: a completed ESP-IDF 6.0.1
  build (its fix agent is mid-flight); then idf_size.py on its .map,
  same as §1/§2.
- Hermes client size/DRAM delta: its branch (~/aiwatchos-hermes) had no
  build directory and no sdkconfig at audit time — code not yet landed.
  Estimated +40–90 KB flash, low DRAM (plain ws). Measure at its first
  build; re-run this table at integration.
- Runtime free DRAM / fragmentation / stack high-waters: needs the
  physical watch (arriving today) — plan in red flag 7.
- Battery capacity and real current draw: no artifact in any repo states
  the cell; measure with the AXP2101 telemetry on device + a timed
  standby run. All §5 runtimes are per-400 mAh estimates until then.
- WiFi/LWIP buffer actual placement (PSRAM vs internal): TRY_ALLOCATE
  makes it runtime-dependent; the heap-by-caps dump in red flag 7
  answers it on first boot.
