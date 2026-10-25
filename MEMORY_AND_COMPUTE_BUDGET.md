# Quake on the ESP32-P4 (Olimex ESP32-P4-PC): memory and compute budget

State of branch `olimex-esp32-p4-pc` (commit after `e7ce7e7`), 640x480 rendering, HDMI 640x480@60
via the LT8912B bridge, first 10 s of the attract-mode demo: **~49.7-50.0 fps = ~20.1 ms per
frame.** 60 fps needs 16.7 ms. All numbers below are from this board (chip revision v1.0,
360 MHz) unless marked as datasheet values.

Contents: 1. Chip and memory hierarchy (datasheet vs measured) - 2. Memory budget (where
everything lives) - 3. PSRAM traffic budget - 4. Compute budget per code area - 5. Where the
time goes over a frame - 6. What this means for 60 fps - 7. How the numbers were measured.

---

## 1. Chip and memory hierarchy

### Datasheet / configuration
| Level | Size | Organisation | Notes |
|---|---|---|---|
| HP CPU | 2 cores, 360 MHz | RISC-V RV32IMAFC + Xesppie (PIE 128-bit SIMD) | 400 MHz only on chip rev >= 3. In-order; measured best-case IPC ~0.93 on a tight loop |
| L1 I-cache | 16 KB **per core** | 4-way, 64-byte lines | (ROM API has separate CORE0/CORE1 ICache controls) |
| L1 D-cache | 64 KB, **one, shared by both cores** | 2-way, 64-byte lines, write-back | caches **both PSRAM and internal SRAM**; write-allocate |
| TCM ("HP SPM") | 8 KB | tightly coupled to the HP CPU | not a cache |
| HP L2MEM (internal SRAM) | 768 KB | | 256 KB of it is configured as the L2 cache (options: 128/256/512 KB); the rest is the app's internal RAM (~445 KB usable as DIRAM after ROM/reserved areas) |
| L2 cache | 256 KB (configured) | 8-way, 128-byte lines | only in front of external memory (PSRAM / flash) |
| LP SRAM | 32 KB | on the low-power bus | slow for the HP cores (see the LP RAM stack incident in PERF_LOG.md) |
| PSRAM | 32 MB | AP Memory 256 Mbit, generation 4, **x16 ("hex")**, 200 MHz DDR, fixed latency | peak **800 MB/s** (16 bit x 2 x 200 MHz); code and rodata also execute from PSRAM (XIP) |
| Flash | 16 MB | QIO, 40 MHz | only used at boot (code is copied to PSRAM) |

Your estimates vs. reality: the L1 data cache is 64 KB (shared), not 16 KB per core (that is
the instruction cache); TCM is *not* 1-cycle (it's slower than an L1 hit, below); internal
SRAM is accessed through the same L1 D-cache, ~12 cycles on an L1 miss; PSRAM is x16 at
200 MHz DDR (800 MB/s peak), but its effective latency on a miss is ~0.5 us, much higher than
the device's raw access time, because the miss goes through L1 + L2 + the MSPI controller and
fills a 128-byte line.

### Measured (main/membench.c, `CONFIG_QUAKE_MEMBENCH=y`, core 0, 360 MHz)
Latency = random pointer chase through 128-byte-spaced nodes (dependent loads). Bandwidth =
unrolled 32-bit loads / stores over the buffer (writes include the final write-back),
memcpy = newlib memcpy within the region. "Idle" = before the display starts; "HDMI" = with
the 640x480@60 RGB888 scan-out running (55 MB/s of PSRAM reads by the display DMA).

| Region | Latency idle | Latency HDMI | Read MB/s | Write MB/s | memcpy MB/s |
|---|---|---|---|---|---|
| TCM 4 KB | 4.0 cyc = 11 ns | 4.0 cyc | 394 | 180 | 236 |
| Internal SRAM 16 KB (L1 hits) | 3.0 cyc = 8.3 ns | 3.5 cyc | 632 | 1116 | 582 |
| Internal SRAM 96 KB (> L1) | 11.9 cyc = 33 ns | 14.2 cyc | 482 (412) | 709 (395) | 373 (199) |
| PSRAM 32 KB (L1 hits) | 3.0 cyc = 8.3 ns | 5.8 cyc | 632 (356) | 975 (479) | 587 (344) |
| PSRAM 192 KB (L2 hits) | 12.1 cyc = 34 ns | 11.9 cyc | 476 (352) | 655 (438) | 374 (257) |
| PSRAM 4 MB (misses) | **177 cyc = 492 ns** | **207 cyc = 576 ns** | 190 (167) | 202 (171) | 98 (84) |
| GDMA (AXI) SRAM -> PSRAM, 10 KB transfers | | | | 138 (both) | |

(Numbers in brackets: with HDMI scan-out running.) Reading the table:
- **An L1 hit costs 3 cycles whatever the memory behind it.** An L1 miss costs ~12 cycles if
  the line is in internal SRAM or in the L2; **~180-210 cycles if it has to come from PSRAM**
  -- 15x more. Everything about performance on this chip is about the L1/L2 hit rate for
  PSRAM data.
- The read/write figures for L1 hits are loop-bound (one 32-bit access per cycle at best =
  1.44 GB/s); 128-bit PIE loads could go higher, but no hot loop here is load-bandwidth bound.
- **TCM is slower than an L1 hit** (4 cycles, writes 180 MB/s); it isn't a speed tier for
  the CPU (code in TCM ran 21% slower, PERF_LOG.md). Useful for small data that must not
  evict the L1, or for DMA-coherent scratch.
- A single core streaming from PSRAM gets ~190 MB/s: with ~1 outstanding miss, 128 B per
  ~0.5 us is ~260 MB/s theoretical. memcpy PSRAM->PSRAM manages only ~90 MB/s (read misses
  plus write-allocate line fills plus write-backs).
- HDMI scan-out (a DMA master) costs the CPU ~15-20% on PSRAM misses and halves the
  effectiveness of the shared L1 for the test (the other core's display task and interrupts
  use the same L1).

---

## 2. Memory budget

### Internal SRAM (DIRAM 445 KB) -- deliberately placed since PERF_LOG "memory layout step 1"
| What | Size | |
|---|---|---|
| IDF code that must stay in IRAM (ISRs, flash driver, ...) | 74 KB | was ~111 KB; FreeRTOS, heap, logging moved to PSRAM |
| Initialised data | 29 KB | |
| .bss total | 181 KB | |
| - edge renderer: span buffers `basespans` (2 x 19 KB) | 38 KB | written by core 0's edge scan, read by both cores' span jobs |
| - edge renderer: `ledges` (1200 edges) / `lsurfs` (449 surfaces) | 38 + 29 KB | peak use 606 / 231 in demo1 |
| - band buffers (2 x 16 rows x 640 8-bit pixels) | 20 KB | the world is drawn here, then DMA'd to PSRAM |
| - job flags, edge lists, z-row ranges, rasteriser tables, core-1 copies | ~25 KB | jobdone 8 KB, newedges/removeedges 8 KB, d_zrowmin/max 4 KB, skintable, blocklights, normals |
| - job worker stack (static, see LP RAM incident) | 8 KB | |
| - IDF/driver .bss (USB host, LWIP, FreeRTOS, ...) | ~40 KB | |
| Heap, free at run time | **~105 KB** (largest block 53 KB) | task stacks (game task 32 KB, display 8 KB, ...), USB/I2S/SD DMA buffers come from here; keep > ~40 KB free |
| TCM (8 KB) | unused | |
| LP RAM (32 KB) | unused, excluded from the heap | |
| L2 cache | 256 KB | |

### PSRAM (32 MB)
| What | Size | Hot per frame? |
|---|---|---|
| Code (XIP) + rodata | 0.94 + 0.21 MB | hot code lives in the L1 I-caches/L2 |
| Static .bss in PSRAM | 2.1 MB | pipeline frame records 603 KB (x2 frames), job queue 254 KB, server state 140 KB, cl_entities 110 KB, model table 100 KB, alias vertex scratch 64 KB x2, ... |
| Quake hunk (all game data: world model, textures, models, sounds, zone) | 16 MB | world model of e1m3 alone: ~880 KB (nodes 118, surfaces 337, edges 101, surfedges 100, vertexes 87, leafs 81 KB) |
| Surface cache (lit, mip-mapped texture blocks) | 4.0 MB | per-frame working set 240-620 KB, peaks 1.3 MB |
| 8-bit frame buffers x3 (rendering / finishing / displayed) | 0.9 MB | yes |
| z-buffers x2 (16-bit) | 1.2 MB | partly (only rows/ranges models need) |
| View model z-buffer (32-bit, generation-tagged) | 1.2 MB | only the gun's pixels |
| HDMI RGB888 display buffers x2 | 1.8 MB | yes (PPA writes, display reads) |
| Free | 3.8 MB | (2 MB more is used by the profiler when enabled) |

---

## 3. PSRAM traffic budget (at ~50 fps)

| Stream | Per frame | MB/s | Master |
|---|---|---|---|
| HDMI scan-out, RGB888 640x480 at 60 Hz | 921 KB per refresh | **55** | DSI bridge DMA |
| PPA palette conversion: read 8-bit 300 KB + dummy foreground 150 KB, write RGB888 921 KB | 1.37 MB | **68** | PPA / 2D-DMA |
| Band copy-out of the world (8-bit, 432 rows) | 276 KB | **14** | GDMA |
| CPU cache misses (texture reads, world data, models, z, code, 2D...): estimate | ~3-4 MB | **~150-200** | both cores |
| **Total** | | **~290-340** | of 800 MB/s peak |

The CPU figure is an estimate: ~2.8 G stalled (non-retiring) cycles per 10 s on the two cores
(cycle and instruction counters, PERF_LOG), assuming ~2/3 of them are PSRAM line fills of
128 B at ~190 cycles, plus write-backs. What we know for sure from diagnostics (each one
removes a stream and measures the frame rate):
- no PPA conversion: +4.4%; 16-bit display buffers (scan-out 37 instead of 55 MB/s, 600 KB
  conversion writes): +2.7% (works with YUV422, branch `yuv-display`, but the 2-stage
  conversion's driver overhead currently costs more);
- world z-fill to SRAM: +2.8%; model pixel/z writes to SRAM: +2.8%; surface builds to SRAM:
  +1%; texture reads from SRAM: +5%; frame-buffer writes (before the SRAM bands): +8%.
- Moving a stream from CPU stores to DMA (z bands) did *not* help: the shared PSRAM
  bandwidth/contention, not the CPU's store latency, was the cost.

---

## 4. Compute budget per code area

PC-sampling profile of both cores at 2 kHz (tools/profile_report.py), grouped by function,
converted to milliseconds per frame (1 frame = 20.12 ms at 49.7 fps). Each core has
20.1 ms per frame.

| Code area | core 0 ms | core 0 | core 1 ms | core 1 | both, ms |
|---|---|---|---|---|---|
| World texture spans (D_DrawSpans8Job) | 6.69 | 33% | 7.18 | 36% | **13.9** |
| Alias models: monsters and view model (rasteriser, transforms, clipping) | 0.16 | 1% | 5.89 | 29% | **6.1** |
| Surface-cache building (lightmaps x textures into cache blocks) | 2.32 | 12% | 1.58 | 8% | **3.9** |
| Edge sorting / span generation (R_ScanEdges & co., D_DrawSurfaces) | 3.29 | 16% | - | - | **3.3** |
| BSP walk / edge emission (R_RenderWorld & co.) | 2.45 | 12% | - | - | **2.5** |
| World z-buffer fill (only where models/particles need it) | 0.50 | 2.5% | 0.77 | 4% | 1.3 |
| RTOS, interrupts, libc (critical sections, memcpy...) | 0.79 | 4% | 0.59 | 3% | 1.4 |
| 2D: console, status bar (mostly the console scrolling up at demo start) | 0.14 | 1% | 0.79 | 4% | 0.9 |
| Client / game / demo playback | 0.53 | 3% | 0.06 | 0.3% | 0.6 |
| Sound mixing | 0.45 | 2% | 0.13 | 1% | 0.6 |
| Particles / sprites | 0.16 | 1% | 0.42 | 2% | 0.6 |
| Display path: PPA, DMA, cache maintenance | 0.35 | 2% | 0.22 | 1% | 0.6 |
| Frame setup (view, z-row ranges, dynamic lights) | 0.39 | 2% | 0.01 | 0% | 0.4 |
| Frame pipeline / job system | 0.17 | 1% | 0.18 | 1% | 0.4 |
| Model lighting | 0.12 | 0.6% | - | - | 0.1 |
| Other (maths library, small functions) | 1.48 | 7% | 0.33 | 2% | 1.8 |
| **Idle (waiting for work)** | **0.14** | **1%** | **1.96** | **10%** | **2.1** |
| **Total** | 20.1 | | 20.1 | | 40.2 |

How busy and how efficient the work is (hardware counters, PERF_LOG "how much time is spent
waiting"): over a frame core 0 retires ~0.52 instructions per cycle and core 1 ~0.50; ~44% of
core 0's cycles and ~40% of core 1's busy cycles retire nothing (memory waits; branch
mispredicts are < 2%). Per area:
| Area | IPC | cycles retiring nothing |
|---|---|---|
| World texture spans | 0.67-0.69 | 31-33% (18.6 cycles / 12.6 instructions per pixel; 13.2 cycles with everything in SRAM) |
| Surface-cache building | 0.64 | 36% |
| Alias models | 0.54 | 46% |
| World z-buffer fill | 0.45 | 55% |
| Core 0's serial code (BSP walk, edge sorting, game) | ~0.44 | ~56% |

Work units per frame (demo1): ~276 k world pixels (128 k core 0, 148 k core 1, ~65 pixels per
span), ~16 surface-cache builds (84 k texels, 14 of them due to dynamic lights), ~350 jobs,
27 bands, ~600 edges and ~230 surfaces in the edge list, ~10-20 alias models plus the gun.

---

## 5. Where the time goes over a frame

The frame is a two-stage pipeline (r_pipe.c): core 0 runs the game and the world of frame N
(both cores draw its spans through the job queue), then records the rest; core 1 draws frame
N's models, sprites, particles and 2D and hands it to the display while core 0 already starts
frame N+1.

```
core 0: | game N+1 | setup | BSP walk N+1 | edge scan N+1 (+spans) .... | end | record |
core 1: | models/particles/2D of N | spans of N+1 (jobs)      ....  | idle  |
display:  PPA converts N (6 ms, DMA)  ......  scan-out at 60 Hz continuously
```

Core 0 is the bottleneck (idle < 1%); core 1 is idle ~10%. Core 0's phases (share of the
frame) and how idle core 1 is during each (profile tagged with core 0's phase, PERF_LOG):
host/game 8%, BSP walk 11% (core 1 24% idle), edge scan 65% (core 1 3% idle), end of world
4%, entity record 5%, particles 3.5%, setup 2%. Core 1's idle time is spread over the serial
phases; there is no single large block of it left.

---

## 6. What this means for 60 fps

- Total work is ~38 ms of core time per frame (40.2 minus ~2.1 idle); two cores at 60 fps
  have 33.4 ms. **Even perfect balancing (+5%) leaves ~14% of work to remove.**
- The single largest cost is the world texture spans (13.9 ms, 35% of all core time). Their
  instruction count is near the minimum for the inner loop (9 instructions per pixel); what's
  left is memory (texture reads that miss L1/L2, ~29% of their time) and the per-pixel
  dependency chain. Cutting instructions (tested twice) didn't change the frame rate.
- Alias models are the second largest (6.1 ms, on core 1) and the least efficient after the
  serial code (46% stalls): they read skins and z and write pixels in PSRAM.
- PSRAM is the shared bottleneck: ~300 MB/s of the 800 MB/s peak is used, but a miss costs
  ~0.5 us, and the display path alone (scan-out + PPA, ~125 MB/s) slows the cores' misses.
- Ideas with measured upper bounds still open: full tiling (models, z and 2D in SRAM bands,
  PPA converting bands straight from SRAM: removes the 8-bit PSRAM frame buffer, z traffic
  and 450 KB of PPA reads per frame, upper bound ~6-8%); a lean PPA/2D-DMA driver for the
  YUV422 display path (~+2%); job queue in SRAM; better balancing of core 0's serial phases.
- Hardware limits of this chip revision: 360 MHz (rev 3: 400 MHz, +11%), no RGB565->RGB888 in
  the DSI bridge, no YUV output from the PPA, 3 TX/2 RX 2D-DMA channels.

---

## 7. How the numbers were measured

- Frame rate: first 10 s of the attract demo after boot (`CONFIG_QUAKE_BENCHMARK_SECONDS=10`),
  one run per experiment; BENCH lines on the console.
- Profile: `CONFIG_QUAKE_PROFILE=y` (+ `_CORE1`): timer interrupts sample each core's PC from
  the ASSIST_DEBUG PC-recording registers at 2 kHz; `tools/profile_report.py <log> build/quake.elf
  --core 0|1`. Area grouping: regular expressions over function names.
- Stalls: mcycle / minstret / mhpmcounter8 (mispredicts) read around the hot functions and
  per core over the window (diagnostic builds, PERF_LOG).
- Memory: `CONFIG_QUAKE_MEMBENCH=y` runs main/membench.c at boot (latency by pointer chasing,
  bandwidth by unrolled loads/stores, memcpy, GDMA copies), before and after the display
  starts.
- Upper bounds ("if this cost nothing"): diagnostic builds that redirect a memory stream to a
  small SRAM buffer (wrong picture, right timing), listed in PERF_LOG.md.
- Picture checks: `CONFIG_QUAKE_SCREENSHOTS=N` dumps frames over the console;
  `tools/screenshots.py` turns them into PNGs.
