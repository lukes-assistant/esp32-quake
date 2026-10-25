# Performance log — Quake on Olimex ESP32-P4-PC

Append-only record of every performance experiment: what was tried, how, and the result.
Newer entries go at the bottom; earlier entries are never edited.

## Method

- Board: Olimex ESP32-P4-PC Rev.C, ESP32-P4 chip rev v1.0, CPU 360 MHz, 32 MB PSRAM @ 200 MHz,
  16 MB flash (QIO). ESP-IDF v5.5.6. HDMI 1280x720@60 RGB888 via LT8912B, double-buffered.
- Render resolution 512x288 (scaled 2.5x by the PPA).
- Metric: the built-in benchmark (`CONFIG_QUAKE_BENCHMARK_SECONDS`), which measures the first
  N seconds of the attract-mode demo1 after boot: `BENCH attract demo: <frames> frames
  <s> seconds <fps> fps`, plus display-pipeline timings on core 1.
- Run-to-run variation: within ~0.3% (20 s window: 21.13 / 21.12 / 21.07 fps), so from
  2026-10-24 one run per experiment, 10 s window.
- Profiling (`CONFIG_QUAKE_PROFILE`): core 1 timer ISR samples core 0's PC at 2 kHz from the
  ASSIST_DEBUG PC-recording register; no measurable fps cost (sampling off: 21.07 / 21.06).

## 2026-10-24 — Baseline and first experiments

Commit 72813d9 (profiling infrastructure).

### Baseline profile (20 s window, 21.1 fps)
- Core 0 (game) is the bottleneck: busy throughout; waits for the display only ~0.1 ms/frame.
- Core 1 (display): palette convert ~8.0 ms + PPA scale ~22.4 ms per frame (~30 ms).
- Core 0 hot spots: D_DrawSpans8 27.5%, D_DrawZSpans 9.8%, R_DrawSurfaceBlock8_mip0 8.9%,
  R_LeadingEdge 3.0%, D_PolysetDrawSpans8 2.8%, R_DrawSurfaceBlock8_mip1 2.6%,
  R_RecursiveWorldNode 2.3%, SND_PaintChannelFrom8 1.7%. Span fill + z fill + surface-cache
  build together ~50%: memory-bound loops on PSRAM buffers.

### Baseline, 10 s window
- 220 frames, **21.96 fps**. Display: convert 7.91 ms, PPA 22.84 ms; game waited 0.16 ms/frame.
  (10 s is faster than 20 s because the opening of demo1 is lighter.)

### Experiment: PPA scaling disabled (diagnostic; HDMI output freezes)
- Skipped `ppa_do_scale_rotate_mirror` in display.c.
- **25.44 fps (+16%)**. The PPA's 2.76 MB/frame RGB888 writes to PSRAM slow core 0 by ~14%.
- Reverted (diagnostic only).

### Experiment: -O3 for the quakegeneric component (default -O2 from CONFIG_COMPILER_OPTIMIZATION_PERF)
- `target_compile_options(${COMPONENT_LIB} PRIVATE -O3)` in components/quakegeneric/CMakeLists.txt.
- **22.96 fps (+4.6%)**. Kept.

### Experiment: hot renderer code in internal RAM (with -O3)
- Linker fragment placing d_scan, d_edge, d_polyse, d_sprite, d_part, r_surf, r_edge, r_bsp,
  r_draw, r_alias, r_main, r_misc, r_light, snd_mix, mathlib in IRAM (`noflash`), ~61 KB.
- **23.21 fps (+1.1% vs -O3)**. Largest free internal block dropped 156 KB -> 95 KB.
- Instruction fetch from PSRAM-XIP is not a major cost. Backed out for now: internal RAM is
  more valuable for data.

### Experiment: L2 cache 512 KB (with -O3)
- `CONFIG_CACHE_L2_CACHE_512KB=y`.
- **Does not link**: `--enable-non-contiguous-regions discards section .sbss...`, i.e. the
  remaining 256 KB of L2MEM cannot hold IDF's static data. Not possible as things stand.

### Experiment: L2 cache 128 KB, Quake task stack in internal RAM (with -O3)
- `CONFIG_CACHE_L2_CACHE_128KB=y`; Quake's 200 KB task stack allocated with
  `heap_caps_calloc(..., MALLOC_CAP_INTERNAL)` instead of calloc (which put it in PSRAM).
  Largest free internal block 276 KB with the smaller cache.
- **21.25 fps**.

### Experiment: L2 cache 128 KB, Quake task stack in PSRAM (control for the above)
- **19.73 fps**.
- So: halving L2 (256 -> 128 KB) costs ~14% (22.96 -> 19.73); moving the stack to SRAM gains
  +7.7% (19.73 -> 21.25). The stack is hot because R_EdgeDrawing keeps `ledges[2400]` and
  `lsurfs[800]`, and R_ScanEdges its `basespans[3000]`, on the stack (~176 KB).
- Both reverted: keep L2 at 256 KB and get the hot stack data into SRAM another way.

### Other findings
- CPU runs at 360 MHz; 400 MHz on rev < 3 chips needs `ESP_FORCE_400MHZ_ON_REV_LESS_V3`
  (out of spec). Not used.
- The PPA's blend unit supports L8 (CLUT) input in hardware, but ESP-IDF 5.5 doesn't expose
  it ("TODO: Support CLUT" in hal/ppa_types.h). Could replace the CPU palette conversion.
- 8 KB TCM ("SPM", 0x30100000) usable via SPM_IRAM_ATTR / SPM_DRAM_ATTR.
- Engine objects are already built with the PIE SIMD extension enabled (Tag_RISCV_arch
  includes xesppie), so PIE assembly can go into the hot files; FreeRTOS saves PIE state
  lazily on context switch.

## 2026-10-24 — Step 1: cheap memory-placement and build changes

Current best before this step: -O3 for quakegeneric, 256 KB L2, 128 B lines: 22.96 fps
(re-measured 22.95 fps with the peak-usage instrumentation below).

### Instrumentation: peak edge/surface usage and Quake stack high-water mark
- r_main.c tracks the peak `edge_p - r_edges` and `surface_p - surfaces` per frame; the
  benchmark report prints them plus the Quake task's unused stack.
- 10 s window: peak edges 579 (of 2400), peak surfaces 223 (of 800); Quake stack 200 KB with
  20380 B never used, i.e. ~180 KB used, almost all of it R_EdgeDrawing's ledges/lsurfs
  (~128 KB) and R_ScanEdges' basespans (~48 KB).
- One 120 s window (demo1 + demo2; done once for buffer sizing, not to be repeated):
  peak edges 833, peak surfaces 346. 24.71 fps over 120 s.

### Ideas from three AI-written notes added by the user, checked against this checkout
- Confirmed: d_scantable/zspantable are EXT_RAM_BSS_ATTR (PSRAM); D_DrawZSpans uses `double zi`
  (4 soft-float calls per span: __extendsfdf2, 2x __muldf3, __fixdfsi); CACHE_SIZE is 32;
  MAXHEIGHT/MAXWIDTH are 1024/1280; GCC emits no hardware-loop (esp.lp) instructions in
  D_DrawSpans8; IDF's default L2 line for 256 KB is 64 B.
- Built objects already include the A extension (zaamo/zalrsc) and xesppie; Zba/Zbb are absent.

### Experiment: 64 B L2 cache lines (IDF default for 256 KB) instead of 128 B
- **19.10 fps (-17% vs 22.95)**. Palette convert on core 1 also slowed (11.1 vs 8.0 ms).
  PSRAM favours long bursts; 128 B lines are clearly better. Reverted to 128 B.

### Experiment: R_EdgeDrawing/R_ScanEdges arrays static (internal SRAM), shrunk
- `ledges`, `lsurfs` (R_EdgeDrawing) and `basespans` (R_ScanEdges) made `static`, so they are
  in internal .bss instead of on the PSRAM stack. NUMSTACKEDGES 2400 -> 1200,
  NUMSTACKSURFACES 800 -> 448 (peaks 833 / 346 seen), MAXSPANS 3000 -> 1200 (MAXSPANS only
  sets how often spans are flushed). ~87 KB of internal RAM.
- First attempt also cut the Quake stack to 32 KB internal: **boot loop** (HP_SYS_HP_WDT_RESET
  right after Host_Init, no panic message). The stack overflowed during startup:
  `-fstack-usage` showed big local arrays in COM_LoadPackFile (131 KB), R_AliasDrawModel
  (88 KB, per frame), SV_SendClientDatagram (64 KB), R_RenderWorld (40 KB), Host_Loadgame_f
  (33 KB), D_PolysetDraw (33 KB, per frame), CL_ParseServerInfo (33 KB),
  R_DrawSolidClippedSubmodelPolygons (18 KB), Con_CheckResize (16 KB), D_DrawSprite (12 KB).
- With the stack back at 200 KB in PSRAM: **23.74 fps (+3.4% vs 22.95)**. Internal RAM free
  92 KB (largest block 45 KB).

### Experiment: big local arrays made static (PSRAM bss), Quake stack 32 KB internal
- The arrays listed above became `static EXT_RAM_BSS_ATTR` (engine is single-threaded and
  none of these functions recurse). Quake task stack 32 KB in internal SRAM (16 KB unused).
- **23.75 fps** — same as with the 200 KB PSRAM stack: once the big arrays are off the stack,
  stack placement doesn't matter. Kept, because it frees the PSRAM stack and keeps the
  per-frame alias arrays (finalverts/auxverts/spans) in fixed places that can later move to
  SRAM. Internal RAM free 59 KB (largest block 31 KB).

### Experiment: d_scantable / zspantable in internal RAM (removed EXT_RAM_BSS_ATTR)
- **23.80 fps (+0.2%, noise)** for 8 KB of internal RAM. Reverted.

### Experiment: `float zi` instead of `double zi` in D_DrawZSpans
- Removes 4 soft-float calls per span (__extendsfdf2, 2x __muldf3, __fixdfsi); float's 24-bit
  mantissa is ample for the 16 z bits stored.
- **24.47 fps (+3.0% vs 23.75)**. Kept.

### Experiment: -fsingle-precision-constant for quakegeneric
- Unsuffixed FP constants (e.g. `0.5`) were promoting float expressions to soft-float double
  all over the renderer and client. Variables declared double (e.g. `realtime`) keep their
  precision.
- **24.87 fps (+1.6% vs 24.47)**. Kept.

### Experiment: remaining hot-path doubles -> float
- `double` locals in R_LeadingEdge, R_RecursiveWorldNode and R_BmodelCheckBBox; FloorDivMod
  and its caller in D_PolysetSetUpForLineScan (+ floor -> floorf); ceil -> ceilf in r_draw.c,
  d_polyse.c, d_sprite.c. (The original Quake ran its renderer with the x87 in 24-bit
  precision mode, Sys_LowFPPrecision, so float matches its real precision.)
- No soft-double calls left in hot D_/R_ functions (only in per-frame/rare ones such as
  R_EntityParticles, R_RotateBmodel, R_ViewChanged).
- **25.63 fps (+3.1% vs 24.87)**. Kept. Note: the game now waits 1.6 ms/frame for the display
  (core 1 ~30 ms/frame vs game ~39 ms/frame), so the display pipeline is close to becoming
  the bottleneck.

### Experiment: Zba/Zbb/Zbs for quakegeneric (-march=rv32imafc_zicsr_zifencei_zba_zbb_zbs_xesppie)
- Built and linked, and GCC used min/max/shNadd in D_DrawSpans8, but the board panicked at
  runtime: **Guru Meditation, Illegal instruction**. This ESP32-P4 (rev v1.0) does not
  implement Zba/Zbb (one of the user-supplied notes claimed it did). Reverted.

### Experiment: code/rodata from flash instead of PSRAM (SPIRAM_XIP_FROM_PSRAM off), flash 80 MHz
- Idea from a user-supplied note: free the PSRAM bus from instruction fetches by executing
  from flash (separate bus). Flash raised from 40 to 80 MHz QIO for this, which on the P4
  requires minimum chip rev > v0.0 (set CONFIG_ESP32P4_REV_MIN_100; the chip is v1.0).
- **24.57 fps (-4.1% vs 25.63)**. Flash fetches are slower than PSRAM via L2. Reverted
  (XIP from PSRAM on, flash 40 MHz, min rev v0.0).

Commit 739680e: step 1 changes kept so far (-O3, single-precision constants, static SRAM
edge/surface/span arrays, big stack arrays static in PSRAM with a 32 KB internal stack, hot
doubles -> float). Re-measured **25.64 fps** (from 21.96 baseline: +17%).

### Experiment: colormap copy in internal SRAM (vid.colormap -> heap_caps_malloc INTERNAL)
- **25.88 fps (+0.9%)** for ~16.6 KB of internal RAM. Not a strong win; internal RAM is
  needed for banding. Reverted.

### Internal RAM audit before banding
- `idf.py size-components`: libquakegeneric uses 197 KB of internal .bss. Largest internal
  symbols: ledges 38 KB, lsurfs 29 KB, cl_static_entities 23.5 KB, basespans 19 KB,
  dma_buffer (audio, main/audio.c) 16 KB, cl_temp_entities 11.8 KB, cl_efrags 10 KB.

### Experiment: cl_static_entities, cl_temp_entities, cl_efrags and audio dma_buffer -> PSRAM
- EXT_RAM_BSS_ATTR on all four (~61 KB). An upstream comment said keeping the cl_ tables
  internal was worth 0.5 fps in an older build.
- **25.62 fps (vs 25.64: no change)**. Free internal RAM 59 KB -> 121 KB (largest block
  45 KB). Kept, to make room for band buffers.

## 2026-10-24 — Step 2: banded rendering (world, models and particles per band in SRAM)

### Design (components/quakegeneric/quakegeneric/source/r_band.c)
- R_ScanEdges already walks rows top to bottom. At the last row of each band it flushes the
  spans (D_DrawSurfaces), then R_BandFinish draws entities, view model and particles for the
  band and copies the band's pixels to the real framebuffer. Band framebuffer and z-buffer
  are static arrays in internal SRAM; d_viewbuffer/d_pzbuffer are biased by the band's top
  row and zspantable is updated, so all existing drawing code works unchanged. The z-buffer
  never touches PSRAM.
- Clipping: alias models via r_refdef.aliasvrect; particles and sprite spans check the band
  rows; before the last band, R_DrawParticles only draws (the last band also runs physics).
  World state (modelorg, r_entorigin, currententity) is saved and restored around the
  per-band entity pass, since D_DrawSurfaces depends on it.
- Bugs found on the way: (1) crash (Load access fault in xTaskGenericNotify; draw_task_handle
  overwritten): models that R_AliasCheckBBox "trivially accepts" (fully on screen) skip
  aliasvrect clipping and wrote rows outside the band. Fixed by disabling trivial accept in
  banded frames. (2) With 48-row bands plus the profiler, USB HID enumeration of the gaming
  keyboard/mouse failed with ESP_ERR_NO_MEM (internal RAM), aborting; the board then sat in
  the GDB stub and needed a JTAG reset (openocd -f board/esp32p4-builtin.cfg -c "init; reset
  run; shutdown") before it could be flashed again.

### Results (current best without bands: 25.62 fps)
- 48-row bands (6 bands, 72 KB SRAM): **24.57 fps (-4%)**.
- 32-row bands (9 bands, 48 KB SRAM): **22.73 fps (-11%)**. Profile: D_DrawZSpans fell from
  ~10% to 4%, but per-band repeats of model work appeared: R_AliasPreparePoints 6.4%,
  memcpy (band copy-out) 3.6%, D_DrawParticle 2.0%, RecursiveLightPoint 1.5%, sin/cos.
- 32-row bands + per-band model culling (R_AliasCheckBBox rejects models whose projected
  bbox doesn't touch the band; view model too): **24.23 fps (-5.4%)**.
  R_AliasPreparePoints 2.4%. Remaining costs: memcpy 4.0%, D_DrawParticle 2.4%, extra
  span flushes; core 0 now idles 4.1% (esp_cpu_wait_for_intr) waiting for the display.

### Conclusion
- Banding works but nets negative here. It only speeds up z/pixel writes (~5% of the frame);
  the dominant costs, D_DrawSpans8 (~24%) and surface-cache building (~13%), read textures
  from PSRAM and are untouched. Even removing all band overheads would leave only a few %
  gain, for ~50-70 KB of internal RAM that USB also needs. Parked on branch
  `banding-experiment` (commit below); main branch reverted to non-banded.

## 2026-10-24 — Step 3: display pipeline

### Experiment: RGB565 HDMI frame buffers, DSI bridge converts to RGB888
- The LT8912B needs RGB888 over DSI, so the BSP forced RGB888 frame buffers (2.76 MB each,
  166 MB/s of continuous scan-out from PSRAM, and 2.76 MB/frame of PPA writes). ESP-IDF's DPI
  panel has separate in_color_format (frame buffer) and out_color_format (DSI). Patched the
  vendored BSP's HDMI path to use RGB565 frame buffers with out_color_format RGB888 (removed
  its `#error`), set CONFIG_BSP_LCD_COLOR_FORMAT_RGB565, and made display.c follow the
  configured format (PPA outputs RGB565, no colour conversion).
- **35.73 fps (+39% vs 25.62)**. PPA 22.2 -> 11.1 ms/frame, palette convert 8.5 -> 6.2 ms;
  game waits 0.02 ms/frame for the display. PSRAM bandwidth was the shared bottleneck.
- Picture correctness (colours, stability) not yet confirmed on the monitor.

- **Correction: invalid result.** The user reported the HDMI screen was solid blue (audio
  still working). On chips before rev v3.0 the DSI bridge cannot convert RGB565 to RGB888:
  in hal/esp32p4/include/hal/mipi_dsi_brg_ll.h (the `CHIP_SUPPORT_MIN_REV < 300` branch),
  set_output_color_format writes the same `raw_type` field as set_input_color_format, so the
  bridge read the RGB565 buffers as RGB888. The 35.73 fps is therefore not valid (scan-out
  was not the intended stream). Reverted (RGB888 frame buffers again).
- Checked alternative: YUV422 frame buffers (16 bpp; on rev < 3 the bridge's YUV input uses
  a separate field, `data_in_type`, so YUV422 -> RGB888 conversion would work). But the PPA
  can't produce YUV422 on this chip: hal/esp32p4/include/hal/ppa_ll.h says "YUV422 not
  supported by PPA SRM hardware before P4 ECO5". Not pursued.

### Experiment: HDMI 1280x720 at 30 Hz (DPI pixel clock 80 -> 40 MHz, same timing)
- New BSP option CONFIG_BSP_LCD_HDMI_HALF_REFRESH halved dpi_clock_freq_mhz (LT8912B follows
  the incoming MIPI timing; its pclk_mhz is only used for its test pattern). 29.99 Hz,
  22.2 kHz line rate (CEA 720p30 is 22.5 kHz).
- **30.66 fps (+19% vs 25.74)**, PPA 22.2 -> 15.5 ms, convert 8.5 -> 7.2 ms. Confirms that
  HDMI scan-out bandwidth (166 MB/s at RGB888 60 Hz) is a major cost for the game core.
- But the user's monitor showed a **blank screen** (doesn't accept this timing). Reverted.

### Experiment: HDMI 720x400@70 (VGA text mode timing), DPI clock from the APLL
- Added a 720x400 mode to the vendored LT8912B/BSP: 28.322 MHz (APLL; the I2S audio clock is
  XTAL on rev < 3 chips, so the APLL is free), 900x449 total, -H +V sync. Quake rendered
  480x320 (scales 1.5x / 1.25x), vid.aspect set for a 4:3 display.
- APLL locked at 28322012 Hz; LT8912B reported HDMI output enabled. **35.19 fps** (PPA 8.4 ms,
  convert 6.5 ms) — but the user's monitor reported **no video input**. Unclear whether the
  monitor rejects 720x400 or the APLL-clocked DPI path is at fault.
- User decision: target **640x480@60** from now on and optimise for that. 720x400 removed.

## 2026-10-25 — New target: 640x480@60, Quake rendered at 640x480 (no scaling)

User decision: optimise for HDMI 640x480@60 with Quake rendering 640x480 natively.

### Setup
- Vendored LT8912B/BSP: new 640x480 mode. Pixel clock 160/6 = 26.667 MHz from PLL_F160M
  (exact integer divider, no APLL); totals widened to 848x524 so the line rate (31.45 kHz)
  and frame rate (60.01 Hz) match VGA; -H -V sync, VIC 1, 4:3. LT8912B detected V total
  0x20c = 524 (with the 720x400 attempt it still reported 741, the 720p total — that mode's
  DPI timing apparently never reached the bridge).
- display.c: when Quake renders at the display resolution, core 1 converts the 8-bit frame
  straight into the RGB888 frame buffer (4 pixels -> 3 aligned 32-bit stores via a 32-bit
  palette); no RGB565 intermediate and no PPA. The PPA path remains for scaled modes.
- quakegeneric.h: 640x480 render size for this mode; vid_null.c sets vid.aspect from the
  displayed shape (1.0 here and for 512x288 at 16:9).

### Baseline for the new target
- **18.50 fps** at 640x480 (185 frames / 10 s). Core 1 convert 15.6 ms/frame; game never
  waits for the display. Peak edges 598, surfaces 230.
- Pixel throughput 5.7 Mpx/s vs 3.8 Mpx/s at 512x288 -> 720p (+50% per pixel), thanks to
  ~55 MB/s scan-out instead of ~166 MB/s and no PPA traffic.

### Profile at 640x480 (18.63 fps)
- D_DrawSpans8 37.7%, R_DrawSurfaceBlock8_mip0 14.4%, D_DrawZSpans 12.2%,
  D_PolysetDrawSpans8 5.3%, R_GenerateSpans 1.9%, R_DrawSurfaceBlock8_mip1 1.8%,
  R_RecursiveWorldNode 1.6%, S_PaintChannels 1.5%. Pixel fill ~70% of core 0's time.
- Core 1 (palette convert) busy 15.8 of ~53.7 ms per frame. 60 fps needs ~3.2x.
- (A CRC-based render-correctness check was started but dropped at the user's request:
  too heavy.)

### Diagnostics at 640x480 (measurement-only builds, reverted right after)
- World z writes disabled (D_DrawZSpans returns at once; models lose occlusion, the gun
  vanished): **22.13 fps** vs 18.63 -> z-buffer writes cost ~8.5 ms/frame (~16%).
- World span pixels written to an SRAM scratch row instead of the framebuffer:
  **20.02 fps** -> framebuffer writes cost ~3.8 ms/frame (~7%).
- So an all-SRAM tiled renderer could save at most ~23% of core 0's time; the rest of
  D_DrawSpans8 (~30% of the frame) is texel fetch and arithmetic.

### Experiment: 4 MB surface cache (-surfcachesize 4096; default ~1.33 MB at 640x480)
- **18.64 fps (no change)**. The cache isn't thrashing; surfaces are rebuilt because their
  lighting animates (lightstyles/dynamic lights). Reverted.

### Experiment: colormap copy in internal SRAM, at 640x480
- **18.70 fps (+0.3%, noise)**. Reverted.

### Experiment: world z-buffer fill on core 1 (d_zjob.c)
- At the start of each D_DrawSurfaces flush, core 0 queues one job per surface (span list +
  the surface's 1/z gradients, from surf_t; background uses 0,0,-0.9) and wakes a worker task
  on core 1 (priority 6: above the display conversion, below audio). Core 0 draws the
  pixels, then waits for the worker before the span lists are reused, so models always
  depth-test against a complete z-buffer. The job copy of D_DrawZSpans takes the gradients
  as parameters (same arithmetic, same z values). Queue of 1024 jobs; overflow falls back to
  the inline z pass. Handshake: worker notifies exactly once per start; only core 0 tracks
  "started" (avoids a stale notification making a later wait return early).
- **20.45 fps (+9.8% vs 18.63)**. Core 1 convert 17.4 ms (was 15.6; it now shares the core).
  Internal RAM free 120 -> 100 KB (16 KB job queue + 4 KB task stack). Kept.

### Experiment: textured world spans shared between both cores (d_jobs.c, replaces d_zjob.c)
- Each D_DrawSurfaces flush now has two phases. Phase 1: core 0 builds surface caches and
  gradients as before, but queues normal textured surfaces as "colour jobs" (span list +
  cacheblock/cachewidth + all D_CalcGradients/z gradients) instead of drawing them; core 1
  meanwhile does the z jobs. Phase 2: both cores draw colour jobs, claimed with an atomic
  counter (__atomic_fetch_add; the core has the A extension). Sky, water and background
  surfaces stay on core 0. D_DrawSpans8Job is D_DrawSpans8 with the gradients passed in
  (same arithmetic). Colour job queue (1024 x ~80 B) in PSRAM.
- Surface cache made 3x larger (PSRAM), since a block now has to survive until the end of
  its flush rather than just until its spans are drawn.
- First run **hung**: ulTaskNotifyTake(pdTRUE) clears the whole notification count, so when
  both "start z" and "start colour" were pending the worker consumed both and then waited
  forever. Fixed with pdFALSE (take one at a time).
- **23.36 fps (+14% vs 20.45)**. Core 1 is now close to saturated: palette convert takes
  26.9 ms of wall time (preempted by the job worker); the game waits 0.37 ms/frame for it.

### Diagnostic: palette conversion skipped on core 1 (screen frozen for that run)
- **28.25 fps (+21% vs 23.36)**: core 1's 15.6 ms/frame RGB888 conversion is now the main
  limiter of the shared span drawing. Worth moving off the CPU (PPA blend unit has CLUT/L8
  hardware on hw_ver1; IDF's driver has it commented out, register value 4 for L8) or
  making much cheaper.

### Experiment: palette conversion reading 32-bit source words, 8 pixels per iteration
- **23.37 fps (no change)**; convert time unchanged. The loop is memory-bound. Reverted.

### Experiment: palette conversion in the PPA blending engine (L8 + CLUT), with self-test
- The PPA blender has CLUT hardware on hw_ver1; ESP-IDF 5.5's driver has L8 commented out.
  Copied esp_driver_ppa into components/ (overrides IDF's; needs `idf.py reconfigure` the
  first time) and patched it: allow L8 as the background colour mode (register value 4),
  plus ppa_blend_set_bg_clut() to load the 256-entry ARGB palette (CLUT RAM at PPA base +
  0x400 in memory-access mode).
- display.c: blend with bg = the 8-bit frame as L8, fg = same buffer as A8 with alpha fixed
  at 0, out = RGB888 into the LCD frame buffer. The palette is snapshotted and the CLUT
  reloaded only when the palette changed.
- Startup self-test converts a test pattern with the PPA and compares it with the CPU
  conversion (±1 per channel allowed); on any mismatch it falls back to the CPU. Result:
  **0 bad bytes** (bit-exact), "Palette conversion: PPA (hardware CLUT)".
- **26.46 fps (+13% vs 23.36)**. Conversion now 7.4 ms in hardware; core 1 is free for
  span jobs meanwhile.

### Profile after PPA conversion (26.45 fps), core 0
- D_DrawColorJobs (core 0's share of span drawing) 34.4%, R_DrawSurfaceBlock8_mip0 17.1% +
  mip1 2.2% (surface-cache building, core 0 only), D_PolysetDrawSpans8 7.1%,
  D_PolysetScanLeftEdge 2.2%, R_GenerateSpans 2.0%, R_RecursiveWorldNode 2.0%, idle 2.3%.

### Experiment: colour jobs published progressively (core 1 starts drawing during the walk)
- Instead of two phases, each colour job is published (atomic release store of the count)
  as soon as its surface cache and gradients are ready; core 1 draws them after its z jobs
  while core 0 keeps building surface caches. Core 0 closes the queue at the end and helps.
  Waiting for an unpublished job is a spin on an internal variable.
- **27.14 fps (+2.6% vs 26.46)**. Kept.

### Experiment: surface-cache building on core 1 only (D_CacheSurfaceDeferred + build jobs)
- Core 0's walk only looks up/allocates the cache block and computes gradients; core 1
  draws the block (R_DrawSurface) before marking the colour job ready.
- **26.98 fps (vs 27.14: no gain)**: core 1 became the bottleneck (builds serialised there).

### Experiment: surface building on both cores (r_surf_c1.c)
- r_surf.c compiled a second time with every global/function renamed (_c1) gives core 1 an
  independent R_DrawSurface; core 0 builds a block itself (with the original copy) when core
  1 has >= 2 builds pending, and publishes that job as already built.
- **27.67 fps (+2.0% vs 27.14)**. Kept. Need per-core utilisation to see the new balance.

### Tool: core-1 PC sampling (CONFIG_QUAKE_PROFILE_CORE1)
- A second gptimer ISR on core 0 samples core 1's PC (PROF1 lines; profile_report.py --core 1).
- First attempt gave a garbage core-0 profile: both sampling ISRs ran at exactly 2 kHz, so
  each always caught the other core inside its own sampling ISR. Fixed by giving the core-1
  timer a period 37 us longer.
- At 27.3 fps: core 1 idle ~42%, and spinning in the build loop while core 0 walked.

### Experiment: core 1 does z first, then interleaves in-order builds with colour jobs
- **26.28 fps (worse)**: core 0 saw a build backlog, built more itself, then spun 11% in
  D_JobsFinish waiting for core 1's in-order builds. Replaced.

### Experiment: independent jobs (build + draw, either core, any order)
- Each colour job builds its block with the claiming core's own copy of the surface code,
  then draws. No ordering. **27.60 fps** (profiling build); core 1 still idle 44%: every
  flush was synchronous, so core 1 had nothing to do while core 0 scanned between flushes.

### Experiment: pipelined job queue across the frame
- One job queue per frame; every surface of every flush becomes a job (textured: build +
  colour + z; sky/water/background: drawn by core 0, z-only job). R_ScanEdges alternates
  between two span buffers and only waits (helping meanwhile) for the jobs of the flush that
  last used the buffer it switches to; D_JobsEndFrame completes everything before the
  entity pass. Core 1 sleeps when the queue is empty (lost-wakeup-safe notification; frame
  generation counter so stale notifications can't start a frame), instead of spinning and
  starving the display task.
- **28.84 fps** (profiling build; +4.5% vs 27.60). Core 1: colour 32%, builds 13%, z 11%,
  idle 39%. Core 0's remaining serial work (models, edges, game logic, HUD) is now what
  leaves core 1 idle.

- Without profiling: **29.03 fps** (from 18.6 at the start of the 640x480 work: +56%).


### Experiment: sparse world z-buffer fill (r_zrows.c)
- The world z-buffer is only read by alias models (including cl.viewent), sprites and
  particles drawn after the world pass. Before R_EdgeDrawing, R_ComputeZRows computes per-row
  [d_zrowmin[v], d_zrowmax[v]] ranges from each alias model's Z-clipped frame bounding box
  (8 corners + 12 edges clipped at ALIAS_Z_CLIP_PLANE, so cl.viewent at r_origin gets its
  tight screen box instead of triggering a full-screen fallback), sprite spheres and live
  particle squares. D_DrawZSpansJob and D_DrawZSpans clip spans to [d_zrowmin[v], d_zrowmax[v]]
  and advance izi by skip * izistep for bit-exact z values inside the range.
- **32.46 fps (+11.8% vs 29.03)** (325 frames / 10.01 s). Kept.

### Experiment: local restrict variables and 32-bit loads/stores in R_DrawSurfaceBlock8_mip0..3
- Cached vid.colormap and step globals in locals and packed 4 pixels per 32-bit load/store in
  r_surf.c (and r_surf_c1.c).
- **32.49 fps (+0.1% vs 32.46, noise)**. With z-spans now sparse, core 1 has even more idle
  headroom during R_ScanEdges and already absorbs surface builds without stalling core 0.
  Reverted.

### Experiment: branchless 8-pixel loop, pipelined fdiv.s and paired loads in D_DrawSpans8Job
- Disassembly of D_DrawSpans8Job showed that sharing the inner loop between 8-pixel blocks
  and the tail caused GCC -O3 to unroll 8 iterations with 7 conditional exit branches inside
  every 8-pixel block (tying up s1..s7 with constants 1..6), while fdiv.s (z = 0x10000 / zi)
  stalled on the immediately following fmul.s, and each lbu was immediately followed by sb.
- Split the full 8-pixel loop (count > 8) from the tail (#pragma GCC unroll 0), issued the
  next block's fdiv.s via inline assembly before the 8 integer texel fetches so division runs
  in parallel in the FPU, paired texel loads (p0/p1, p2/p3, p4/p5, p6/p7) to hide load-use
  latency, and skipped fdiv.s/snext/tnext when the remaining tail is 1 pixel.
- **34.52 fps (+6.3% vs 32.46)** (346 frames / 10.02 s). Kept.
## 2026-10-25 — (continued after a session break; state at HEAD 81ceb7f re-measured 34.54 fps)

### Profile at 34.3 fps (both cores)
- Core 0: D_DrawSpans8Job 24.4%, R_DrawSurfaceBlock8_mip0 10.5% (+mip1 1.4%), D_PolysetDrawSpans8
  9.0%, R_GenerateSpans 3.5%, D_DrawZSpansJob 3.3%, R_LeadingEdge 2.8%, D_PolysetScanLeftEdge
  2.7%, R_RecursiveWorldNode 2.4%, idle 2.1%, CL_RelinkEntities 2.0%, Draw_ConsoleBackground
  1.7%, S_PaintChannels 1.6%.
- Core 1: idle 45.5%, D_DrawSpans8Job 31.9%, R_DrawSurfaceBlock8_mip0_c1 12.6%, z 3.7%.

### Experiment: 4 span buffers (in PSRAM) instead of 2 (in SRAM)
- So core 0 can scan further ahead of the job queue. **34.08 fps (no gain)**; buffer-switch
  waits aren't the limiter. Reverted.

### Experiment: core 0 only helps with jobs inside the range it waits for (D_JobsWaitRange)
- Idea: when switching span buffers core 0 was claiming fresh jobs, stealing work core 1
  would do. **34.35 fps (no gain)**. Reverted.

### Checked against an AI-written list of ideas supplied by the user
- "Aliasing reloads in R_DrawSurfaceBlock8 (restrict locals + 32-bit stores)": already
  tried, +0.1% (noise), reverted (see above). Same idea for D_PolysetDrawSpans8 not yet tried.
- "Branch bloat / fdiv.s stalls in D_DrawSpans8Job": already done (commit 81ceb7f, +6.3%).
- "Offload Turbulent8 / D_DrawSkyScans8 / D_DrawSolidSurface to jobs": none of them is in
  either core's top 25 (<0.7% each). Little to gain.
- "Parallel alias rasterisation": agreed, ~15% of core 0, the largest serial chunk left.

### Experiment: hottest loops in the 8 KB TCM (SPM_IRAM_ATTR)
- D_RunJob (span + z loops inlined, 1800 B), R_DrawSurfaceBlock8_mip0/mip1 (+_c1 copies),
  D_PolysetDrawSpans8, R_LeadingEdge, R_GenerateSpans, R_StepActiveU: ~5.2 KB of 8 KB.
- Ran fine on both cores, but **27.35 fps (-21% vs 34.54)**. Executing from TCM is slower
  than from PSRAM via the L1 instruction cache (hot loops already hit the I-cache), possibly
  also contended between the two cores. Reverted.

### Experiment: local copies of globals in D_PolysetDrawSpans8 (alias model spans)
- acolormap, r_zistepx, r_lstepx, a_*step*, skinwidth copied to locals (byte stores could
  alias them, forcing reloads every pixel). Bit-exact.
- **35.06 fps (+1.5% vs 34.54)**. Kept.

### Experiment: alias span packages (D_PolysetDraw's spans[]) back in internal SRAM
- **35.02 fps (no gain)** for 33 KB of internal RAM. Reverted (stays in PSRAM).

### Experiment: hardware cache prefetch (autoload) for PSRAM
- At boot, L1 D, L2 and L1 I autoload were all **disabled** (cache_ll_*_is_*_autoload_enabled
  printed 0). The ROM has Cache_Config/Enable_{L1_DCache,L2_Cache}_Autoload (4 regions each,
  miss/hit/both trigger, step direction); its header says not to call them from SDK apps,
  but they are in this chip's ROM table. Prefetched lines are clean, so DMA buffers stay
  correct as long as drivers write back/invalidate around transfers (they do).
- L2 autoload over all of PSRAM (0x48000000-0x4c000000), miss trigger, positive order:
  **36.53 fps (+4.2% vs 35.06)**.
- Plus L1 D-cache autoload with the same region: **35.95 fps** (worse than L2 alone; likely
  pollutes the L1). Dropped.
- L2 only, trigger on miss and hit: **36.74 fps (+4.8% vs 35.06)**. Kept.

### Experiment: L1 I-cache autoload (both cores) on top of L2 autoload
- **36.53 fps (no gain vs 36.74)**. Reverted.

### Observed: occasional hang (not yet diagnosed)
- The user saw the board hang (picture frozen). A 4-minute serial capture of the current build
  reproduced it shortly after demo2 started: Fps lines simply stop, no panic or reboot.
  JTAG attach (OpenOCD) and opening the serial port both reset the chip, so the hung state
  couldn't be inspected. Suspects: the job system's cross-core waits, the PPA blend never
  completing (display task holding the frame mutex), or cache autoload. The user asked to
  set this aside and focus on performance for now.

### Experiment: 16-pixel perspective subdivision in the span jobs (was 8)
- One perspective-correct division per 16 pixels instead of 8, as the original DOS/Win Quake
  did by default (d_subdiv16 with the asm D_DrawSpans16). Slightly different texel mapping
  from the 8-pixel C path, same as the original game's look.
- **37.77 fps (+2.8% vs 36.74)**. Kept.

### Statistics: surface-cache builds (new BENCH line)
- Per frame in the 10 s window: **33.9 builds, 222k texels, 31.0 of them because a dynamic
  light touches the surface** (dlit surfaces are rebuilt every frame in Quake's renderer).

### Diagnostic: surface-cache build stores (R_DrawSurfaceBlock8_mip0) to an SRAM scratch row
- **41.09 fps (+8.6% vs 37.83)**: the builds' PSRAM writes (write-allocate) are a large cost.

### Experiment: build surface-cache blocks in 16-row bands in SRAM, then memcpy each band
- R_DrawSurface reordered to draw one band of block rows (all columns) into a 4 KB SRAM band
  buffer (per core copy) and copy the band (contiguous in the cache block) to PSRAM.
- **37.69 fps (no gain vs 37.83)**: contiguous CPU stores into PSRAM cost the same as the
  scattered ones, so the cost is the PSRAM write traffic itself (likely write-allocate line
  fills), not the store pattern. A DMA copy (esp_async_memcpy, no line fills, async) could
  avoid it but needs careful cache invalidation; upper bound ~8%. Not pursued for now.
  Reverted.

### Experiment: alias models drawn by both cores, each clipped to half the screen
- r_alias.c, d_polyse.c and r_aclip.c are compiled a second time (r_alias_c1.c,
  d_polyse_c1.c, r_aclip_c1.c) with every global/function renamed by alias_c1_names.h,
  including the shared state they write (currententity, modelorg, r_refdef, the rasteriser
  error terms) and Mod_Extradata (core 1 gets a read-only version; core 0 loads every model
  in a prepass so the cache LRU is only touched by core 0). entity_t.trivial_accept became a
  per-copy static. R_DrawEntitiesAndViewModel (r_main.c) culls, lights (R_LightPoint isn't
  thread-safe) and loads the alias models + view model on core 0, then both cores draw the
  list clipped via r_refdef.aliasvrect; models wholly in the other half are rejected in
  R_AliasCheckBBox. Sprites are drawn by core 0 afterwards.
- First run: **crash** on core 1 (Load access fault in R_AliasPrepareUnclippedPoints_c1).
  R_AliasC1Wait used a bare task notification, but the game task also receives notifications
  from the d_jobs worker; a stale one let core 0 clear r_alias_split while core 1 was still
  drawing, so core 1 took the unclipped path. Fixed with an explicit done flag. (The panic
  left the board in the GDB stub; recovered with an OpenOCD JTAG reset.)
- Top/bottom split: **37.45 fps (no gain vs 37.83)**: the view model (the biggest model)
  sits low on the screen, so core 1 got almost all its pixels.
- Left/right split: **38.76 fps (+2.5%)**. Kept. Possible sub-pixel seam where models are
  clipped at the centre line (same clipping as at screen edges); not visible in the log.

## 2026-10-25 — Stability: two concurrency bugs fixed, cache autoload removed

The multi-core work had introduced crashes/hangs (the user saw a hang). Found with 4-minute
runs through all three demos (diagnostic runs, not benchmarks) plus temporary debug code
(frame-buffer guard zones, surface-cache list validation at phase checkpoints, and a hang
reporter printing both cores' PCs, task states and job-queue state; all removed again).

### Bug 1: surface-cache block freed while a queued job still used it (crash)
- Repeated crash in D_SCAlloc (`*sc_rover->owner = NULL`, owner = 0x13020202: four 8-bit
  pixels) in demo3. Guard zones after the frame/z buffers stayed intact; validating the
  surface cache's block list showed the first damaged header right after the world pass.
- Cause: a dynamically lit surface reuses its existing cache block and queues a rebuild job.
  Later in the same frame core 0 allocates a block for another surface; if the allocator's
  rover reaches that existing block, it frees it and writes a new block header into it; the
  pending rebuild then writes texels over that header. (The original renderer built each
  surface immediately, so blocks used earlier in a frame were never still in use.)
- Fix: surfcache_t.usedframe is stamped when D_CacheSurfaceDeferred returns a block; before
  D_SCAlloc frees a block used this frame, it waits (helping) for all queued jobs
  (D_JobsWaitAll).

### Bug 2: deadlock in D_JobsWaitRange (hang)
- The first version of bug 1's fix hung after ~1 minute. The hang reporter showed the game
  task runnable (spinning), the worker asleep, all published jobs claimed, queue open.
- Cause: core 1 can claim a job index before core 0 publishes it and then sleep; core 0 only
  wakes it at the end of a flush. Waiting for that job from inside the flush (the allocator
  wait) spun forever. Fix: D_JobsWaitRange wakes the worker before waiting.

### Cache autoload removed
- With the two bugs fixed and autoload off: 4 minutes, all three demos, no faults.
- Whole-PSRAM L2 autoload: crash within seconds after a level load (load fault in
  R_DrawSurface). Likely prefetched lines of level data that the SD card's DMA was still
  writing stay stale after the driver's invalidate. It was probably also behind the user's
  original hang.
- Autoload restricted to CPU-only buffers (surface cache, z/frame buffers): +~2%, but one
  unexplained reboot in demo3; adding code/rodata/PSRAM .bss: Instruction access fault in
  demo3. The ROM header says not to call these functions from applications; on this chip it
  isn't safe. Removed entirely.
- Clean build (fixes, no autoload, no debug code): **37.05 fps**, stable for 4 minutes.

### Profile at the stable 36.7 fps (both cores)
- Core 0: D_DrawSpans8Job 22.8%, R_DrawSurfaceBlock8_mip0 10.4%, idle 5.0%, D_PolysetDrawSpans8
  3.9%, D_DrawZSpansJob 3.5%, R_GenerateSpans 3.5%, R_LeadingEdge 2.7%, R_RecursiveWorldNode
  2.7%, memcpy 2.1%, CL_RelinkEntities 2.0%, Draw_ConsoleBackground 1.8% (console sliding up
  during the first seconds of the window), FreeRTOS critical sections ~2.5%.
- Core 1: D_DrawSpans8Job 31.6%, idle 31.3%, R_DrawSurfaceBlock8_mip0_c1 12.8%, alias ~10%.

### Experiment: issue 8 texel loads before any store in the span loop (memory-level parallelism)
- **36.83 fps (no gain)**: the cache doesn't overlap misses. Reverted.

### Experiment: FreeRTOS tick 1000 Hz (was 100)
- **36.79 fps (no gain)**. Reverted.

### Experiment: adaptive left/right split column for the alias pass
- Split column moved each frame towards equal drawing times on both cores.
- **37.33 fps (+0.8%, too small)**. Reverted.

### Experiment: reuse dynamically lit surface-cache blocks when their lights are unchanged
- Quake rebuilds every dynamically lit surface every frame. Each cache block now keeps a hash
  of the dynamic lights it was built with (index, origin, radius, minlight: everything
  R_AddDynamicLights reads), and a lit block is reused if the same lights hit it again (plus
  the usual texture/lightstyle checks). Same inputs, so the same texels: output unchanged.
- Builds per frame 34.4 -> **16.9** (texels 226k -> 89k; dlight-caused 31.5 -> 14.1).
- **39.38 fps (+6.3% vs 37.05)**. Kept.

### Profile at 39.1 fps (both cores)
- Core 0: D_DrawSpans8Job 25.3%, idle 5.6%, R_DrawSurfaceBlock8_mip0 5.0%, D_PolysetDrawSpans8
  4.4%, R_GenerateSpans 4.0%, D_DrawZSpansJob 3.9%, R_LeadingEdge 3.4%, R_RecursiveWorldNode
  3.1%, CL_RelinkEntities 2.3%, memcpy 2.1%, Draw_ConsoleBackground 1.8%, S_PaintChannels 1.7%,
  __ieee754_sqrt 0.75%.
- Core 1: D_DrawSpans8Job 34.4%, idle 34.3%, surface builds 7%, alias ~11%, z 4.9%.

### Experiment: float versions of libm (sqrt/sin/cos/atan2/atan/floor/ceil/fabs/pow) + -fno-math-errno
- mathlib.h maps them to sqrtf etc. for the whole engine (the double versions are software
  routines on this single-precision FPU; -fno-math-errno lets sqrtf become fsqrt.s). Game and
  client maths now run in single precision, which is what Quake's data is anyway.
- **41.17 fps (+4.5% vs 39.38)**. Kept.

### Profile at 40.9 fps (after float libm)
- Core 0: D_DrawSpans8Job 26.7%, idle 5.9%, R_DrawSurfaceBlock8_mip0 5.3%, D_PolysetDrawSpans8
  4.6%, R_GenerateSpans 4.4%, D_DrawZSpansJob 4.3%, R_LeadingEdge 3.5%, R_RecursiveWorldNode
  3.3%, memcpy 2.3%, CL_RelinkEntities 2.3%. Core 1: spans 36.9%, idle 31.1%.

### Diagnostics (wrong picture, not kept)
- Span writes to a dummy SRAM line instead of the PSRAM framebuffer: **44.33 fps (+7.7%)**:
  upper bound for anything that speeds up framebuffer writes.
- Span texel reads from an 8 KB SRAM table instead of the surface cache: **43.30 fps
  (+5.7%)**. (A first try with a 32 KB static table ran at 20 fps; probably it pushed some
  internal allocation into PSRAM. Ignored.) So the span loop is mostly instruction-bound:
  ~9 instructions per pixel (srai, srai, mul, add, add, lbu, sb, 2x add).
- Core-0 phase timing (Sys_FloatTime around phases, ms/frame, 300-frame block in demo1):
  host before rendering 1.1, R_SetupFrame..R_ComputeZRows 0.85, world (R_EdgeDrawing) 19.0,
  entities 5.8, particles 1.0, 2D+VID_Update ~0.6, sound 0.4.
- Core-1 idle by core-0 phase (profiler samples on core 1 tagged with core 0's phase):

  | core 0 phase | share of time | core 1 idle in it |
  |---|---|---|
  | outside R_RenderView (host, client, 2D, sound) | 12.1% | 82% |
  | R_RenderWorld (BSP walk, edge emission) | 10.6% | 97% |
  | R_ScanEdges (spans published as they're made) | 39.3% | 4% |
  | D_JobsEndFrame (drain the queue) | 15.9% | 14% |
  | entities (alias split L/R) | 17.9% | 17% |
  | particles + R_SetupFrame etc. | 4.3% | 95% |

  Core 1 idles almost entirely while core 0 does serial work (~27% of the frame). Perfect
  balance of the current total work would give ~50 fps; 60 needs both less work and that.

### Experiment: entity_t.model/msgtime moved next to forcelink (one cache line for the
### per-slot scan in CL_RelinkEntities)
- **41.46 fps (+0.7%, noise level)**. Reverted.

### Tool: screenshots over the console (CONFIG_QUAKE_SCREENSHOTS=N)
- After the benchmark window, every 100th presented frame is printed as hex SHOT lines
  (8-bit pixels + palette); tools/screenshots.py writes PNGs. Lets me check the picture for
  render-order changes without the user's monitor (screenshots only after the measured window,
  so benchmarks are unaffected). Reference shots at 41.2 fps look right (world, monsters,
  view model, status bar).

## 2026-10-25 — Frame pipelining: core 1 finishes frame N while core 0 starts N+1

### Upper bounds first (diagnostics)
- No alias models or particles drawn at all: **50.59 fps** (vs 41.17). Without the view model
  only: 43.46 fps. So models + particles cost ~4.6 ms of wall time per frame, and core 1 was
  idle during ~27% of the frame (host work, BSP walk; see the phase table above).

### Design (r_pipe.c, view_c1_names.h, d_part_c1.c; draw.c split into record/execute)
- After the world of frame N is done (D_JobsEndFrame), core 0 no longer draws the models,
  particles and 2D itself: it records them and hands the record to core 1 at VID_Update, then
  goes on with frame N+1 (game, client, BSP walk, edges, spans with the job queue). Core 1
  (the job worker task) draws the record: all alias models (the existing core-1 copy of the
  alias code), particles (new core-1 copy of d_part.c), the recorded 2D calls, the palette,
  and calls QG_DrawFrame.
- Recorded: alias items with entity snapshots (lighting is computed by core 0 in the prepass
  as before), particle positions and colours, Draw_* calls (translation tables copied,
  Draw_CachePic done at record time), the palette. The core-1 drawing code reads its own
  copy of the view state (view_c1_names.h renames r_origin, vpn, xcenter, d_viewbuffer,
  d_pzbuffer, zspantable, ... in the core-1 compiles), captured when the frame is recorded.
- Buffers: 3 frame buffers (core 1 finishing, core 0 rendering, display showing) and 2
  z-buffers alternating per frame (vid.numpages = 3, so the status bar's incremental redraw
  still works).
- Memory core 1 reads but doesn't own (model data, pics) lives in the cache/hunk: every
  Cache_Alloc/Free/Flush and Hunk alloc/free waits for the pending frame (R_PipeSync), as
  do CL_ClearState and immediate palette changes.
- Frames that can't be deferred (underwater warp; sprites, which only core 0 can draw) wait
  for the pending frame and draw models/particles on core 0 as before (split L/R with core 1);
  their 2D and present are still recorded. In demo1's first 10 s, 361 of 432 frames deferred.
- Bug found on the way: the view model skips the bbox test in R_DrawAliasItems and inherited
  the previous model's static alias_trivial_accept; if that was 1, the gun was projected
  without clipping (huge coordinates: store fault, or core 1 rasterising forever). The split
  path forced it to 0, hiding it. Now always 0 for the view model, as in the original.
- Quake's hunk reduced from 20 MB to 16 MB: the profiling build (2 MB of sample buffers) ran
  out of PSRAM for the surface cache with the new buffers.
- Tooling: bench.sh now resets the board over JTAG and retries when flashing fails (the
  board sits in the panic handler after a crash).

### Result
- **43.17-43.26 fps (+5% vs 41.17)**. Screenshots look right (models, particles, view model,
  status bar). 4-minute run through all three demos: no faults.
- Profile: core 0 idle 3.6% (was 5.9%), core 1 idle 14.1% (was 31%). The cores are now
  fairly balanced; the frame costs ~42.5 ms of core time, 60 fps needs 33.4 ms. Core 1's alias
  span drawing (D_PolysetDrawSpans8_c1) is 14.1% of its time; Draw_ConsoleBackground +
  Draw_Character (console scrolling up at the start of the demo) 3.9%.

