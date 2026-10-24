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

