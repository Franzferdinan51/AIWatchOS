# AIWatchOS perf audit — render/input pipeline (READ-ONLY phase)
Date: 2026-10-13 · Tree: ~/aiwatchos-flash, branch glass-fix @ 7607296 · Method: source read only (no builds, no serial). All line refs are to that tree.

## Headline verdict
The lag is not one bug; it is an additive frame budget. The main loop period is
**work + 40 ms** (the frame delay is tacked on AFTER render+flush, app_main.cpp:388,597),
so even a zero-cost frame caps at <25 fps, and a realistic frame (~40-80 ms of work)
lands at **8-14 fps with touch sampled once per period**. Every frame — idle or not —
repaints the full screen through the slowest pixel path in the codebase, re-hashes
412 KB of PSRAM, and issues ~11-12 I2C transactions on the bus touch shares.
Underneath all of it, PSRAM runs at 40 MHz and the CPU at 160 MHz, so every one of
those costs is roughly double what the same code would cost at the boards usual clocks.

## Frame anatomy (one loop iteration, app_main.cpp:416-598)
1. board.read_touch() — 1 I2C read (+5 ms in-loop retry on failure), main/app_main.cpp:421, touch_driver.cpp:181-199
2. board.read_buttons() — GPIO0 + AXP2101 poll_key = 1-2 I2C txn, hal.cpp:265-272
3. BootEscape + DisplayPower update (app_main.cpp:441-458)
4. Touch dispatch (app_main.cpp:484-501), app tick (504) — Hermes tick may take the client lock and do NVS writes (hermes_adapter.cpp:543-570)
5. App-switch brightness dip bookkeeping (app_main.cpp:509-523)
6. FULL render: shell backdrop gradient (full screen) -> app render -> status zone (backdrop slice repainted AGAIN + 5-txn read_power) -> link strip (app_main.cpp:528-564)
7. Band scan: FNV-1a hash of the ENTIRE framebuffer (band_flush.cpp:31-42)
8. Flush dirty bands only: per band, CPU byte-swap into a 20-row DMA bounce buffer, one chunk at a time, semaphore-serialized (display_driver.cpp:218-232)
9. vTaskDelay(40 ms) — app_main.cpp:597. All of it runs in the ESP main task (prio 1, CPU0, 3.5 KB stack; sdkconfig CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584, AFFINITY_CPU0).

## Ranked suspects

| # | Suspect | Evidence (file:line) | Mechanism | Expected win | Measure on-device |
|---|---------|----------------------|-----------|--------------|-------------------|
| 1 | PSRAM at 40 MHz + CPU at 160 MHz (platform clocks capped) | sdkconfig: CONFIG_SPIRAM_SPEED_40M=y, SPIRAM_SPEED_80M not set; CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160 | Framebuffer writes, band-hash reads, glyph reads, byte-swap reads, and XIP code/rodata (SPIRAM_FETCH_INSTRUCTIONS/RODATA/XIP all on) all cross PSRAM at half the usual octal-DDR rate; CPU-bound loops (FNV, put_pixel, blend) run at 2/3 speed. Multiplies suspects 2-6. | ~1.5-2x on every PSRAM/CPU-bound stage | esp_clk_cpu_freq + spiram speed in boot log before/after; frame-time histogram (fix plan F0) idle-clock p50 |
| 2 | Full-screen repaint every frame via per-pixel put_pixel | battery_display.cpp:11-26 (backdrop via fill_rect per row); app.cpp:32-38 fill_rect -> hline -> app.cpp:16-22 put_pixel per pixel; status slice repainted twice (battery_display.cpp:33) | 205,820 bounds-checked single-pixel writes into PSRAM per frame for the backdrop ALONE, before the app draws anything. The fast fill_span path (app.cpp:50-58) exists but fill_rect/hline do not use it. Band-flush only saves QSPI traffic; render cost is paid in full every frame, idle or not. | 5-15 ms/frame | Frame-time histogram with render stage split (F0); idle clock face, backdrop-only A/B |
| 3 | ~11-12 I2C transactions per frame on the shared bus (2x read_power + poll_key) | battery_display.cpp:47 (every frame); clock_face.cpp:125 (every frame, clock); hermes_adapter.cpp:768 (every frame, Hermes); power_manager.cpp:97-126 = 5 separate txn per read_power, 50 ms timeout each (axp_rd, power_manager.cpp:~55); hal.cpp:267-271 poll_key per read_buttons | Serialized into the UI task before touch dispatch completes; each txn is ~0.1-0.3 ms wire+driver at 100-400 kHz plus bus-mutex waits, and any timeout costs up to 50 ms PER REGISTER. Also the prime contention source for the known ~9% touch INVALID_STATE bursts (same bus, plus hermes_link.cpp:248 read_power from the prio-5 pump task). | 2-5 ms/frame typical; removes worst-case 50-250 ms stalls; may cut touch failures | MDB health touch ok/fail counters over 300 s; frame p95/p99 before/after; i2c txn count via tpoll |
| 4 | Frame period = work + fixed 40 ms delay; no vsync/TE use | app_main.cpp:388, 597; TE is enabled in panel init (display_driver.cpp:80, cmd 0x35) but never waited on | Cadence is additive, not a deadline: 60 ms work -> 100 ms period (10 fps). Input latency = up to one full period before a tap is even sampled, plus render+flush before the response is visible (150-250 ms total worst case). | Sets a true 25-30 fps ceiling-bound cadence; cuts input latency ~40 ms | Timestamp MDB inject_tap -> first changed band in screenshot/log; frame period histogram |
| 5 | Flush path fully serialized: 20-row bounce, swap-then-wait per chunk | display_driver.cpp:43-44 (kBounceRows=20, 16,400 B), 218-232 (swap loop, draw_bitmap, xSemaphoreTake per chunk); pclk 40 MHz quad (display_driver.cpp:127) | Full frame = 26 chunks; CPU swap and DMA never overlap (single bounce buffer, wait per chunk). Wire floor for a full frame at 40 MHz quad = 411,640 B x 8 / 4 / 40 MHz = ~20.6 ms of pure data, plus per-chunk CASET/RASET command overhead x26; realistic full flush 35-60 ms. Min flush unit is a 125-row band (~9-15 ms) — a one-line status change pays a quarter-screen flush. | 10-30 ms on any frame with a dirty band | Flush-stage ms in F0 histogram; mdb screenshot wall time (411,640 B dumps) before/after |
| 6 | Text: linear glyph search + O(n^2) wrap measurement, per frame | font.cpp:139-145 find_glyph = linear scan of PSRAM records per char; called by text_width_aa (font.cpp:349) AND draw_text_aa (font.cpp:380) — centered text pays both; hermes_adapter.cpp:199-230 draw_wrapped re-measures every prefix char-by-char (text_width_aa per prefix) | A wrapped Hermes paragraph (~200 chars) = ~200 prefix measures x ~100 chars x ~100-record scans = order 10^6 PSRAM record touches PER RENDER, repeated every frame. Clock Display96 digits pay the same scan at the largest face. Glyphs themselves are RAM-resident (whole files loaded once, font.cpp:225-305 — no per-glyph FAT reads; that part is fine). | 2-10 ms/frame on text-heavy screens (Hermes worst) | Render-stage ms on Hermes screen (F0); wrap a fixed paragraph, count frame p50 |
| 7 | App-switch brightness dip stretches with frame period | app_main.cpp:517-522 (6 steps x 1 frame each) | Designed as ~240 ms at 40 ms frames; at the real ~100 ms period it is a ~600 ms fade on EVERY app switch — reads as pure lag to the user. | Perceived switch time 600 -> ~150 ms if steps are time-based or shortened | Stopwatch/MDB goto timing: switch command -> settled screenshot |
| 8 | Touch failure handling blocks the UI task; timeout is infinite | touch_driver.cpp:192 (vTaskDelay(5) + retry inside read), 285 (transaction_timeout_ms = -1) | A failed read costs >=5 ms in-loop; a wedged transaction on the shared bus can block the loop indefinitely (timeout -1). Poll cadence = frame cadence (app_main.cpp:421), so slow frames directly = sluggish touch. Stage-3 diagnostic esp_log_level_set("i2c_master", ESP_LOG_DEBUG) still in production init (touch_driver.cpp:309) — if effective under CONFIG_LOG_MAXIMUM_LEVEL=INFO it should be inert, but it must go. | Removes multi-ms/multi-frame input stalls | Touch fail counters + frame p99 during a 300 s soak; tap-to-response timing |
| 9 | Band hash reads the whole framebuffer every frame | band_flush.cpp:8-24, 31-42: 102,910 word iterations, FNV serial multiply chain, over PSRAM | ~412 KB PSRAM read per frame even when nothing changed and nothing will flush. The header comment claims "well under a millisecond" (band_flush.hpp:9-13) — that assumes fast PSRAM; at 40 MHz PSRAM + 160 MHz CPU, serial-dependent FNV is plausibly 2-5 ms. Still cheaper than flushing, but not free, and it is pure overhead on idle frames. | 2-5 ms/idle frame | F0 histogram scan-stage ms, idle clock |
| 10 | Occasional UI-path hitches: NVS writes + client-lock contention | hermes_adapter.cpp:543-570 (hermes_pump runs IN the UI tick: outbox/conversation NVS saves under LockGuard); hermes_link.cpp:409 pump task prio 5 vs main task prio 1; render holds the same client lock (hermes_adapter.cpp:373) | nvs_commit = flash erase/write, typically 10-100 ms, on the UI task when a turn settles or the outbox mutates; the prio-5 link task can also preempt mid-render. Explains sporadic hitches, not baseline lag. | Removes 10-100 ms sporadic hitches | Frame p99/max around a Hermes send/receive cycle |
| 11 | Log path cost per line | mdb_console.cpp:65-86: every log line is vsnprintf-ed, copied BYTE-BY-BYTE into the 32 KB ring under a spinlock, then forwarded to the real vprintf (USB-JTAG) | Cheap at INFO volume; becomes material if any per-frame/per-transaction logging is live. Audit found no per-frame ESP_LOG in the loop itself — listed as a guard, not a current cost. | Guard only | mdb logs volume over 60 s idle |

## Area notes
1. FLUSH: dirty-band (4 bands) over a PSRAM framebuffer; flush = bands only, but band = 125 rows minimum. pclk 40 MHz quad matches the Espressif CO5300 component default macro (managed_components/espressif__esp_lcd_co5300/include/esp_lcd_co5300.h:117-135). The vendor BSP (/tmp/bsp206/esp32_s3_touch_amoled_2_06.c:437) uses the SH8601 macro family — its pclk value lives in that component, not in the BSP copy, so "vendor uses a faster clock" is UNVERIFIED from local sources; treat 80 MHz as a bench experiment, not a known-good. No double buffering; single bounce buffer + per-chunk completion wait (correct for banding, serial for speed).
2. CLOCKS: CPU 160 MHz (240 available), PSRAM octal DDR at 40 MHz (80 MHz config exists, unset), flash 80M. PM/DFS off — clocks are static, so these are pure caps, likely from the earlier power pass.
3. RENDER LOOP: main task, prio 1, CPU0 (shares with WiFi/lwIP defaults). No TE/vsync sync anywhere. Idle clock face still repaints + re-hashes everything each iteration; only the flush is skipped when hashes match. The status-zone mini-clock updates at minute granularity but is redrawn (and its band re-flushed) whenever ANY status pixel changes.
4. TEXT: fonts/glyphs fully PSRAM-resident after one-time load (good); costs are the linear find_glyph, double measurement of centered text, per-pixel blend RMW into PSRAM (app.hpp:75-80 put_pixel_blend), and Hermes draw_wrapped O(n^2). NOTE vs older notes: Hermes text now routes through draw_text_aa (hermes_adapter.cpp:189-192) — the legacy 5x7 path survives only as the assets-missing fallback (font.cpp:359-372) and in the frame card.
5. INPUT: touch sampled once per loop iteration in the UI task; BOOT is a plain GPIO read (hal.cpp:269, zero-latency); PWR goes through the AXP2101 I2C poll (same frame cost as #3). BootEscape timing is frame-quantized, so escape/PTT hold thresholds inherit frame jitter.
6. BLOCKERS: weather fetch runs in its own service task (app_main.cpp:379) — not on the UI path. On the UI path: I2C power reads (#3), NVS saves (#10), the touch retry (#8). Mutexes: I2C bus (touch/PMU/RTC/codec), Hermes client lock.
7. IDLE: asleep = loop skips tick/render/flush and just delays (app_main.cpp:467-478) — good. AWAKE idle = full repaint + full hash + ~12 I2C txn per iteration, ~25x/s. The only thing idle saves is QSPI traffic.

## Measurement plan (hands-on phase, in order)
- F0 (instrument first): add frame-stage timing to the firmware (render ms, hash ms, flush ms, frame period; rolling avg + p95) exposed in `mdb health`. Every fix below is proven against these numbers, not feel.
- Clocks: boot-log CPU/PSRAM freq lines + F0 idle-clock p50 before/after each clock change (one at a time).
- Flush: time 20 consecutive `mdb screenshot` dumps (411,640 B each) + F0 flush ms, per pclk/bounce variant.
- Input: scripted `mdb tap` at a known target -> screenshot diff timestamp = tap-to-visible latency; touch ok/fail counters from `mdb health` over a 300 s soak for the touch bar.
- I2C: F0 frame p95/p99 + touch fail rate before/after power-read caching.

## Top-3 fixes for the hands-on phase
1. **Raise the platform clocks** — CPU 160->240 MHz, PSRAM 40->80 MHz DDR (sdkconfig), with the resource gate re-checked (DIRAM/PSRAM headroom + a thermal/brownout watch via mdb health). Expected: every CPU/PSRAM stage shrinks ~1.5-2x. Proof: F0 idle-clock frame p50 and full-flush ms before/after; bar: full flush <= ~25 ms, idle frame work <= ~10 ms.
2. **Cache power state; stop per-frame I2C** — read_power at 1 Hz (or on change) into a cached PowerStatus consumed by status bar / clock / Hermes; poll_key at most once per frame is inherent, keep, but read_power must leave the render path (battery_display.cpp:47, clock_face.cpp:125, hermes_adapter.cpp:768). Expected: ~11-12 txn/frame -> ~1-2; removes 50 ms-timeout stall class from the UI path and the worst bus contention against touch. Proof: frame p95/p99 drop; touch fail counters over 300 s soak fall toward zero (shared win with the touch session).
3. **Make the frame period a deadline and idle frames nearly free** — replace work+40 ms delay with a fixed-period schedule (delay = period - work, floor 0); skip the backdrop/status repaint when the band hash of the previous frame already proves no change is impossible — practically: render apps into the fb only when state changed OR 1 Hz heartbeat for the clock seconds/minutes, keeping the hash as the flush gate. As a cheap first cut inside this fix: route fill_rect/hline through fill_span (app.cpp) and repaint the backdrop ONCE per frame (drop the status-zone double repaint). Proof: F0 idle-clock frame work p50 <= ~5 ms and tap-to-visible latency (mdb tap -> screenshot diff) <= ~80 ms.
Runner-ups, in order: time-box the dip (<=150 ms total), double-buffer/larger bounce (40-60 rows) + optional pclk 80 MHz experiment, glyph index (codepoint->record offset table built at load) + single-pass centered text + wrap-measure cache, touch retry out of the UI task + finite transaction timeout.

## Reference cross-check (added 2026-10-13, web + sibling-repo sources)

| Reference | What it does (verified) | Our delta | Feeds suspect |
|---|---|---|---|
| **muse-gadget-206 — SAME board + panel** (github.com/Franzferdinan51/muse-gadget-206; read from the Mac clone ~/proj/muse-gadget-206) | CPU **240 MHz** (`esp32/sdkconfig.defaults`: CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y); PSRAM **80 MHz** (`esp32/devices/sdkconfig.muse-waveshare-s3-206`: CONFIG_SPIRAM_SPEED_80M=y); pclk = stock CO5300 macro, **40 MHz — same as ours** (`boards/board_waveshare_s3_206.c:197-198`). Flush (`boards/muse_lcd_bands.c:44-54,100-123`): LVGL partial bands enter a queue; each band is copied into one of **two internal chunk buffers** and sent by a dedicated flow with ISR completion counting (`chunk_done`, `s_chunk_free` semaphore) — DMA of chunk N **overlaps** rendering/copying of chunk N+1. | We run 160 MHz / 40 MHz PSRAM and a **single** bounce buffer with a strict swap->send->wait serialization (display_driver.cpp:218-232). Same wire clock; roughly half the memory/CPU speed and none of the overlap. | #1 (clocks — now PROVEN same-board, not speculative), #5 (pipelining is the flush fix, not pclk) |
| **Vendor BSP panel clock** (waveshare/esp32_s3_touch_amoled_2_06 v2.0.0 uses SH8601 macros) | esp-iot-solution `esp_lcd_sh8601` CHANGELOG v1.0.0: "Increase pclk_hz to 40MHz in default panel IO configuration" — the SH8601 family default IS 40 MHz (github.com/espressif/esp-iot-solution, components/display/lcd/esp_lcd_sh8601/CHANGELOG.md). Espressif CO5300 macro also 40 MHz (our managed_components header). | **No delta.** Our 40 MHz QSPI is the family-standard value on every reference. | CORRECTS #5/area-note 1: drop "raise pclk to 80 MHz" from the experiment list — no reference project in this panel family runs the QSPI faster; the flush win is double-buffering + async queueing (gadget-206 pattern), plus the CPU-side swap cost falling with #1. |
| **InfiniTime** (github.com/InfiniTimeOrg/InfiniTime, `src/displayapp/LittleVgl.cpp`) | LVGL draws into **two 4-line internal buffers** (`lv_disp_buf_init(&disp_buf_2, buf2_1, buf2_2, LV_HOR_RES_MAX * 4)`); flush_cb receives **only LVGL-invalidated areas** and calls `lv_disp_flush_ready` right after the DMA draw; a rounder_cb expands to full-screen ONLY when a full refresh is explicitly requested (scroll transitions). Render+flush cost is proportional to the changed area; nothing full-frame happens on an idle watchface. (Different panel — ST7789 240x240 — but the discipline is the point, and it runs on a 64 MHz nRF52832.) | Our minimum flush unit is a 125-row band, our render unit is the whole screen every frame, and our change detection is a post-hoc hash over 412 KB instead of invalidation tracked at draw time. | #2, #9: the end-state architecture is dirty-REGION rendering (track damage where drawing happens), with the band hash as an interim step — not a permanent per-frame tax. |
| **ZSWatch** (github.com/ZSWatch/ZSWatch) | Zephyr + LVGL with the same invalidation-driven partial-refresh model; screens redraw what changed, the display sleeps between events. Current hardware is nRF5340 + 240x240 GC9A01 (30 MHz SPI per its README) — pattern corroboration only, NOT clock evidence for our panel; its ESP32-S3 AMOLED variant was not source-verified in this pass. | Same architectural delta as InfiniTime. | #2, #4 (event-driven refresh vs fixed poll-render loop) |
| **TE/vsync on this panel family** (pulse-ware-hub/jarvis_esp32-s3-touch-amoled-1.75, THIRD_PARTY_NOTICES.md — a CO5300 1.75" sibling project) | Its esp_lcd_co5300 / esp_lvgl_adapter modifications are explicitly credited as: "QSPI clock, SPI interrupt core, **TE synchronisation**, DMA block transfers" — i.e. a working project on this panel family invested in TE-synced flushing. Our init already enables TE (display_driver.cpp:80) but nothing waits on it. | TE sync is proven-in-family and available to us for free (signal already enabled); it eliminates tearing and lets flush start at scanout boundaries instead of arbitrary phase. | #4 (adopt: gate band flush on TE when the panel is mid-frame) |

### Net effect on the fix order
Fix 1 (clocks) is upgraded from "expected" to **reference-proven on identical hardware**: gadget-206 ships 240 MHz / 80 MHz PSRAM for this exact board. Fix 3s flush component changes shape: copy gadget-206s muse_lcd_bands pattern (band queue + 2 chunk buffers + ISR completion) rather than enlarging the single bounce or touching pclk. The dirty-region end-state (InfiniTime/ZSWatch model) stays the overhaul-stage architecture goal; the band hash remains the bridge.

## Open threads (stopped here by design — audit delivered for the hands-on phase)
- ZSWatch ESP32-S3 AMOLED variant: driver config not source-verified (current ZSWatch mainline is nRF5340/GC9A01); cited for refresh-discipline pattern only.
- SH8601 macro pclk confirmed via the esp-iot-solution changelog (default raised to 40 MHz) rather than the macro header itself; consistent with the CO5300 macro and gadget-206s shipped config, so confidence is high.
- All frame-time figures in this audit are code-derived estimates; F0 instrumentation (frame-stage timing in mdb health) is step 1 of the hands-on phase and replaces them with measurements.

## QUALITY KNOBS (owner-authorized 2026-10-13: a SMALL visual-quality reduction is acceptable for real speed)
ADOPTION GATE (owner condition): every knob below carries an ESTIMATED saving from code math only. No knob is adopted until the hands-on phase measures its frame-ms saving with the F0 stage timing (A/B, same screen, same state) and the measured saving is real. Hero text — clock Display96 digits, Hermes Pair40 pairing code — keeps full quality under every knob unless a measurement says the saving is large AND the owner approves the visible change.

| # | Feature (cost today) | Cheaper setting | Expected saving (ESTIMATE — measure before adopting) | Visible difference |
|---|---|---|---|---|
| Q1 | **Text AA = 4-bit (16 levels)**, per-pixel blend RMW into PSRAM: glyph nibble unpack + put_pixel_blend reads dst, blends, writes (font.cpp:196-217 draw_glyph; app.hpp:75-80 put_pixel_blend). Paid for EVERY glyph pixel of EVERY string, EVERY frame — and small UI text (Micro 15 / Caption 16) is most of the text pixels on most screens. | **Threshold-hybrid for small faces** (Micro/Caption/Body): nib == 0 skip, nib >= 12 -> plain put_pixel (no dst read, no blend math), only mid nibbles (1..11) take the full blend. Keeps most edge smoothing; only true edge pixels pay blend cost. Stronger variant if measurement demands: full 1-bit (on/off) for Micro only. | Blend-path pixels on small text drop ~50-70% (most glyph coverage is 0 or near-full); small-text render cost down an estimated 30-50%. | Slightly crisper/harsher edges on the smallest text; at 15-18 px on a 2.06-inch panel, minimal at arm's length. Zero change to hero faces (Display/Title/Pair keep full 4-bit blend). |
| Q2 | **Backdrop gradient recomputed every frame**: draw_shell_backdrop walks all 502 rows computing vgrad_color per 4-row band and filling via the per-pixel path, then the status zone repaints its slice a second time (battery_display.cpp:11-26,33). The backdrop NEVER changes at runtime — it is a static image recomputed ~25x/s. | **Bake once**: render the backdrop into a dedicated PSRAM buffer at boot (410x502x2 = 412 KB of the 8 MB PSRAM); per frame, blit it into the framebuffer with a fast span/memcpy path. | Removes the single largest per-frame render item: an estimated 5-15 ms/frame (the whole of suspect #2's backdrop share). | **None.** Pixel-identical output; the same gradient, computed once instead of forever. (Quality-free — should not even need the quality budget, just the measurement.) |
| Q3 | **Launcher tiles recomposited every frame**: rounded-rect fills + vertical gradients + AlphaSprite shadow blends (fill_round_rect_vgrad, stroke_round_rect per-pixel blends, shadow_tile/pill/card sprites — app.cpp:99-240; font.cpp:417-430) for a screen whose tiles are static except selection/scroll state. | **Cache pre-rendered tiles**: render each tile (normal + selected = 2 variants) ONCE into PSRAM bitmaps at launcher init / theme change; per frame, blit tiles (memcpy-speed) and draw only dynamic bits on top (labels can bake in too — app names are static; badges/counts draw live). | Launcher frame render from full compositing to ~9 blits: estimated 3-8 ms/frame while the launcher is foreground. | None in steady state. During scroll, tiles move as rigid bitmaps (visually they already do — nothing inside a tile animates). |
| Q4 | **Card gloss gradients recomputed per frame** (Hermes bubbles, notification cards): fill_round_rect_vgrad recomputes band colors + span fills every render (app.cpp:210-240). | Bake static card backgrounds like Q3 when a card's size+colors are fixed for its lifetime (chat bubbles: bake per width-class on first use, cache in PSRAM, blit after). Live/typing surfaces keep the dynamic path. | 1-4 ms/frame on card-heavy screens (Hermes chat, notifications). | None (identical pixels, cached). Memory cost: a handful of card bitmaps in PSRAM. |
| Q5 | **Icon/sprite blending per frame**: icons draw through the same per-pixel blend path (font.cpp:401-415 draw_icon); the weather icon face is 64 px; the Muse avatar sprite blends per pixel per frame while visible. | **Pre-rendered icon cache**: first draw of (icon, face, color) renders into a small PSRAM cache entry; later draws blit. Recurring colors are few (token palette), so the cache stays tiny. | 0.5-2 ms/frame on icon-heavy screens (weather, launcher glyphs, Muse). | None. |
| Q6 | **Cosmetic animation frame rate**: anything animated (avatar idle motion, Hermes busy indicator, analog second hand on the alternate clock face) inherits the full loop rate and forces full repaint + band flush per step. | **Decouple cosmetic animation to 10-15 fps** (update every 2nd-3rd loop iteration) while touch sampling and content updates stay at full rate; analog second hand steps at 1 Hz instead of sweeping (decorative on the alternate face; the primary digital face has no seconds). | Avoids full repaint+flush cycles on otherwise-idle animated screens: up to ~half the flush traffic on those screens. | Decorative motion slightly less smooth. No effect on interaction latency or content. |
| Q7 | **App-switch dip duration** (suspect #7): 6 brightness steps, one per frame — ~600 ms at the real frame period. | Time-box the dip to <=150 ms wall time (fewer/coarser steps: 100 -> 30 -> 100 in 3 steps, timed not frame-counted). | Perceived switch latency down ~450 ms per switch. | The fade is a touch less gradual. A feel knob, not a frame-ms knob — measure by stopwatch/MDB goto timing, not F0. |

### Suggested evaluation order in the hands-on phase
Q2 first (zero visible difference, largest estimated saving — pure waste removal), then Q1 (biggest text win, tiny visible cost, hero faces exempt), then Q3/Q4/Q5 (the caching family — same bake-and-blit machinery as Q2, build it once), Q6/Q7 last (feel knobs, judged on-device by the owner). Each lands only with its F0 A/B number recorded next to it in this file.

## TRANSITION PATH — the owner's #1 acceptance test
Owner's exact words: "Visual lag like a line that scrolls when it opens app — it's not fluid, very slow and artifacty." This section is the audit of THAT symptom. Acceptance: opening an app must show NO visible band sweep on the glass.

### Why the sweep happens (mechanism, from the code)
Render is monolithic: the whole frame is drawn into the PSRAM framebuffer first (app_main.cpp:528-564), hashed (band_flush.cpp:31-42), then flushed band-by-band top to bottom (app_main.cpp:583-590), each band swap-copied and DMA-sent strictly serially (display_driver.cpp:218-232). The panel displays each band the moment it lands in GRAM, while its own scanout keeps running unsynchronized (TE is enabled in init — display_driver.cpp:80, cmd 0x35 — but nothing ever waits on it, and no TE GPIO is configured in the panel IO). So during every full-screen change, the boundary between the new frame (bands already flushed) and the old frame (bands not yet reached) is a physical line that rolls down the glass over the whole flush duration. That rolling line IS the owner's symptom, exactly.

### The numbers (code-derived estimates; F0 replaces them with measurements)
- Panel refresh period: this AMOLED class scans at ~60 Hz -> **~16.7 ms** per scanout (confirm exact rate from the CO5300 datasheet in the hands-on phase; nothing in our init sets a frame-rate command).
- Per-band DRAW time: draw is not banded — full-frame render happens before any flush: backdrop 5-15 ms (suspect #2) + app render 2-15 ms (text-heavy worst, suspect #6) + hash 2-5 ms (suspect #9). During draw the glass still shows the old frame — draw time adds latency, not sweep.
- Per-band FLUSH time: a band is 125-126 rows = ~102,910 B. Wire floor at 40 MHz quad = ~5.1 ms; plus the CPU byte-swap of ~51,455 PSRAM pixels and 7 chunk semaphore round-trips (20-row bounce) -> **~9-15 ms per band** realistic.
- Total app-open frame: draw (~10-30 ms) + 4-band flush (**~35-60 ms**) = the sweep is on the glass for 2-4 full panel scanouts. Invisible is impossible at that duration — any flush longer than one scan period shows a boundary somewhere.
- The dip makes it worse, additively: an app switch runs the 6-step brightness dip over 6 FRAMES (app_main.cpp:517-522), each frame re-rendering and re-flushing, so the "open" is a ~600 ms sequence of rolling boundaries at varying brightness instead of one.

### The hard constraint (physics, not tuning)
A full frame is 411,640 B. At 40 MHz quad QSPI the wire floor is **20.6 ms > 16.7 ms panel period**. No amount of pipelining at the current clock fits a full-screen flush inside one scanout, and a beam-chase (start flushing just behind the scan position, same direction) needs the flush to write rows FASTER than scanout reads them: flush = ~41 us/row vs scanout ~33 us/row at 60 Hz — we lose that race at 40 MHz too. Conclusions: (a) at current clocks the sweep can only be HIDDEN, not outrun; (b) outrunning/chasing needs both a faster wire AND TE sync — and no reference in this panel family runs the wire faster (see cross-check: BSP/SH8601, CO5300 macro, gadget-206 all sit at 40 MHz), so that path is speculative until a bench test proves the panel tolerates more.

### TE / region-flush support in the references
- esp_lcd + this panel: our init already turns TE on (0x35); using it requires the panel's TE signal routed to a GPIO and the flush gated on it — the vendor BSP (waveshare/esp32_s3_touch_amoled_2_06 v2.0.0, /tmp/bsp206) does NOT configure TE sync (NULL callbacks, plain LVGL flush), and gadget-206's muse_lcd_bands does not use TE either — it wins by pipelining, at partial-band sizes. The one in-family TE user found is the Jarvis CO5300 1.75-inch project (credited modifications: QSPI clock, TE synchronisation, DMA block transfers). Hands-on phase must first check whether the TE pin is even routed on this board (schematic/BSP pin list) before planning around it.
- Region flush: gadget-206 flushes LVGL bands (partial regions) through its queue; InfiniTime flushes only invalidated areas. We already flush only changed bands in steady state (band hash) — but an app-open changes everything, so region flushing does not help THIS symptom; masking does.

### Ranked transition fixes (hands-on phase)
- **T1 — Mask the switch (do first; works at current clocks).** On app change: send brightness 0 as a single panel command (instant, co5300_set_brightness — display_driver.cpp:305-310), THEN compose and flush the full new frame while the panel is dark, then restore brightness. The sweep still takes 35-60 ms but happens invisibly; perceived transition = a clean ~50-80 ms blink instead of a 600 ms rolling fade. This REPLACES the 6-frame dip (suspect #7/Q7) — audit confirms the dip is additive to the sweep, so it goes. If the owner wants a fade, make it 2 brightness steps around the dark point, time-boxed <=150 ms total.
- **T2 — Shrink the flush itself** (the structural fixes already ranked: clocks 240/80M per gadget-206, double-buffered band pipeline per muse_lcd_bands, backdrop bake Q2). Target: full-frame flush <= ~21 ms (wire floor) so the masked blink gets shorter and in-app partial flushes (1 band, ~5 ms at speed) fit inside one scan period unmasked.
- **T3 — Chase with TE, only if the pin is routed AND a bench test shows the panel stable above 40 MHz QSPI.** Highest ceiling (true invisible unmasked transitions), highest risk, no reference support in-family. Ranked last on purpose.
- **T4 — Keep + refine changed-band flushing** for in-app updates (typing echo, clock minute, Hermes streaming text): those are 1-band flushes that CAN fit one scan period once T2 lands; the overhaul's dirty-region work shrinks them further.

### Acceptance measurement
The sweep is a GLASS phenomenon — MDB screenshots dump the framebuffer and cannot see it (same epistemics as the green bar). Proof = the owner's eyes / a phone video of the glass during app opens, before vs after T1. Proxy metric on the bench: F0 flush-ms for the app-open frame (target: fully inside the T1 dark window; dark window total <= ~100 ms), plus dip code deleted or time-boxed.

---

## RESULTS (perf-wave hands-on, 2026-10-13) — measured on-device via `mdb perf`

Baseline = original build (160 MHz CPU / 40 MHz PSRAM, 6-frame dip, single
bounce buffer). Final = perf-wave close-out build (branch `perf-wave`).

| Metric | Baseline | Final | How |
|---|---|---|---|
| Clock idle loop period | 87.6 ms (11.4 fps) | 60.3 ms (16.6 fps) | clocks + render trims |
| Render (clock) | 34.3 ms | 17.0 ms | clocks, fill spans, power cache |
| Band hash | 13.4 ms | 7.5 ms | clocks (240/80) |
| Full-frame flush | 232–263 ms | ~26 ms avg / 42–46 ms on switch frames | double-buffered pipeline |
| Switch launcher→Hermes | 915 ms | 99–109 ms | dip deleted, clocks, flush pipeline |
| Switch Hermes→launcher | 1199 ms | 99 ms | same |
| Switch Hermes→Muse / Muse→clock | — | 118 ms / 69 ms | same |
| Idle clock flushes | every content change re-flushed via full repaint loop | 0 flush frames in a 20 s static window | band hash (unchanged mechanism, now reachable) |
| Image size | 1,363,904 B | 1,367,296 B (+0.25%) | gate OK |
| DIRAM | ~120,959 B | 123,783 B (+2.3%) | gate OK |

Fixes landed, in audit order: clocks 240 MHz / PSRAM 80 MHz (gadget-206
board config; QSPI pclk left at the family-standard 40 MHz); 1 Hz
AXP2101 power cache in `Board::read_power()` (was 5 I2C reads × several
call sites per frame); fill/hline/vline/fill_rect direct span writes
(was per-pixel bounds-checked `put_pixel`); two 20-row DMA bounce
buffers with ISR completion counting (swap of chunk N+1 overlaps DMA of
chunk N; flush stays synchronous for the caller).

### Transition outcome — deviation from T1, on purpose
T1 proposed masking the switch at brightness 0. Once the flush reached
~26 ms the mask became unnecessary complexity: the close-out instead
DELETES the 6-frame brightness dip entirely (owner-reported flicker on
the perf build). Brightness is never touched on a switch. The panel
holds the outgoing app's last flushed frame in GRAM while the loop
composes the incoming app's first frame in full in the framebuffer;
one band flush then swaps it in. Bench proof via the perf counters:
every measured switch window shows exactly **1 flush frame** (the
composed switch frame) and `switch_ms` of 69–118 ms = one
render+hash+flush. No clear-to-black exists in the switch path.
The `dark_ms_last` perf field was removed with the dip.

### Also in the close-out
- Hermes footer: `HOLD BOTTOM` hint + hold-countdown lines re-anchored
  to `font_line_height(kBody)` (23 px) instead of the stale 14 px 5x7
  assumption; hint line box now ends exactly at the safe line (488).
  Verified by host render with real font assets: hint ink rows
  470–482, nothing drawn at/below 488.
- Touch: panel-IO transaction timeout −1 (forever) → 50 ms; the
  temporary stage-3 `i2c_master` DEBUG log forcing removed. (The ~9%
  INVALID_STATE burst issue itself remains the separate deep-fix
  session's scope.)
- Perf instrumentation slimmed to the shipping diagnostics readout
  (`mdb perf` + one `mdb health` line); per-stage timers kept — they
  are the bench proof for every number above.

### Deferred (not in this branch)
Change-driven rendering (skip render when nothing changed — the loop
still repaints every frame; band hash already prevents re-FLUSHING, so
this is an idle-power optimisation, not a fluidity fix), text glyph
index + wrap cache, Q1 threshold-hybrid AA. All remain valid follow-ups
from the ranked lists above; none was needed to hit the acceptance
numbers.
