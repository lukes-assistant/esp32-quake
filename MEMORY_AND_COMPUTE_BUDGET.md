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
| L1 D-cache | 32 KB (configured; up to 64 KB), **one, shared by both cores** | 4-way, 64-byte lines, write-back | caches **both PSRAM (`0x4800_0000`) and internal SRAM (`0x4FF0_0000`)**; write-allocate |
| TCM ("HP SPM") | 8 KB (`0x3010_0000`) | tightly coupled to the HP CPU | uncached (`Cache_Address_Through_Cache == 0`); supports PIE 128-bit loads/stores |
| HP L2MEM (internal SRAM) | 768 KB (`0x4FF0_0000` cached, `0x8FF0_0000` uncached alias) | | 256 KB configured as L2 cache (options: 128/256/512 KB); ~445 KB usable as DIRAM. Uncached alias (`0x8FFx_xxxx`) bypasses L1D and does **not** support PIE 128-bit loads/stores |
| L2 cache | 256 KB (configured) | 8-way, 128-byte lines | only in front of external memory (PSRAM / flash); write-back, write-allocate |
| LP SRAM | 32 KB (`0x5010_8000`) | on the low-power APB/AHB bridge | 175 cycles/load (8 MB/s) from HP cores; faults (`MCAUSE=5`) on PIE 128-bit loads/stores |
| PSRAM | 32 MB (`0x4800_0000`) | AP Memory 256 Mbit, generation 4, **x16 ("hex")**, 200 MHz DDR, fixed 14-cycle read latency | raw data rate **800 MB/s** (16 bit x 2 x 200 MHz); code and rodata also execute from PSRAM (XIP) |
| Flash | 16 MB | QIO, 40 MHz | only used at boot (code is copied to PSRAM) |

### Measured (`main/membench.c`, `CONFIG_QUAKE_MEMBENCH=y`, core 0, 360.0 MHz)

#### A. Memory & cache hierarchy (CPU latency & bandwidth)
Latency = pointer chase through 128-byte-spaced nodes (random order, sequential order, and 4 independent random chains walked in lockstep).
Bandwidth = `rd32` / `wr32` (unrolled 32-bit scalar loads/stores), `rd128` / `wr128` (unrolled 128-bit PIE `esp.vld.128.ip` / `esp.vst.128.ip` vector loads/stores; writes include final `esp_cache_msync` write-back on cached regions), `line touch` (one 32-bit load per 64-byte L1 line), and `memcpy`.
Numbers in parentheses `(HDMI)` are measured with the 640x480@60 RGB888 MIPI-DSI/HDMI scan-out running (~55 MB/s PSRAM read traffic).

| Region | Random chase (cyc) | Seq chase (cyc) | 4-chain (cyc/load) | Read 32b (MB/s) | Read 128b PIE (MB/s) | Line touch (MB/s) | Write 32b (MB/s) | Write 128b PIE (MB/s) | memcpy (MB/s) |
|---|---|---|---|---|---|---|---|---|---|
| **TCM (`0x3010_0000`, 4 KB, uncached)** | 4.0 (4.0) | 4.0 | 3.0 | 396 | **1,874** (1,798) | 4,975 | 477 | **1,867** | 236 |
| **LP SRAM (`0x5010_8000`, 4 KB, uncached)** | **175.0** (175.4) | 175.0 | 175.0 | **8** | *faults* | 126 | **8** | *faults* | 4 |
| **SRAM in L1 (`0x4FFx`, 16 KB)** | **3.0** (3.0) | 3.0 | **1.5** | 639 | **5,076** (5,034) | **9,090** | 814 | 1,656 (1,732) | 441 |
| **SRAM at L1 edge (`0x4FFx`, 32 KB)** | **3.0** (3.0) | 3.0 | **1.5** | 640 | **5,098** (5,098) | **9,152** | 856 | **1,934** | 466 |
| **SRAM beyond L1 (`0x4FFx`, 96 KB, L2MEM hits)** | **12.1** (12.0) | 10.0 | 12.0 | 479 | **1,771** (1,771) | 2,093 | 640 | **1,258** | 349 |
| **SRAM uncached alias (`0x8FFx`, 16 KB)** | 12.0 (12.0) | 12.0 | 12.0 | 115 | *faults* | 1,768 | 120 | *faults* | 60 |
| **PSRAM in L1 (`0x48xx`, 16 KB)** | **3.0** (3.0) | 3.0 | **1.5** | 639 | **5,037** (5,036) | 8,962 | 292 | 361 (324) | 223 (211) |
| **PSRAM at L1 edge (`0x48xx`, 32 KB)** | **3.0** (3.0) | 3.0 | **1.5** | 634 | **5,098** (5,098) | **9,152** | 298 (269) | 370 (325) | 231 (212) |
| **PSRAM in L2 (`0x48xx`, 192 KB, L2 hits)** | **12.0** (12.0) | 10.0 | 12.1 | 480 | **1,765** (1,771) | 2,094 | 281 (257) | 353 (317) | 207 (192) |
| **PSRAM cold (`0x48xx`, 4 MB, L2 misses)** | **177.4** (204.7) | 156.1 (180.8) | 177.6 (204.4) | 187 (166) | **218** (193) | **237** (206) | 199 (169) | **200** (171) | 95 (83) |

Key takeaways from the memory & cache table:
- **L1 D-cache hits (`3.0 cycles` = 8.3 ns, `1.5 cycles/load` with 4 chains) deliver `5.1 GB/s` with 128-bit PIE loads and `9.15 GB/s` line-touch rate**, regardless of whether the backing store is internal SRAM or PSRAM. Scalar 32-bit load loops (`640 MB/s`) are purely CPU instruction-issue bound.
- **L1 misses that hit in internal SRAM (`L2MEM`) or L2 cache cost `12.0 cycles` (33 ns) random / `10.0 cycles` sequential, delivering `1.77 GB/s` with 128-bit PIE loads.**
- **TCM (`0x3010_0000`) is uncached zero-wait-state RAM (`4.0 cycles` load latency, `1.87 GB/s` 128-bit PIE read and `1.87 GB/s` 128-bit PIE write).** (The earlier 12 MB/s write figure was an artifact of calling `esp_cache_msync` on an uncached TCM address; guarding with `Cache_Address_Through_Cache()` fixed it.)
- **PIE 128-bit vector loads/stores (`esp.vld.128.ip` / `esp.vst.128.ip`) are only wired to the HP CPU's L1D port and TCM port.** Executing a 128-bit PIE load/store against LP SRAM (`0x5010_8000`) or the uncached L2MEM alias (`0x8FF0_0000`) triggers a hardware `Load/Store access fault` (`MCAUSE = 5/7`).
- **LP SRAM (`0x5010_8000`) costs `175 cycles` per 32-bit load (`8 MB/s`)** across the LP bridge — as slow as a cold PSRAM miss, without any cache in front of it. Never place stacks or hot data in LP SRAM.
- **Cold PSRAM misses cost `177.4 cycles` (493 ns) random / `156.1 cycles` sequential (`204.7` / `180.8 cycles` with HDMI scan-out active), and `chase4` (4 independent random chains) also costs `177.6 cycles/load`.** The CPU + L2 cache subsystem serializes external PSRAM line fills (1 outstanding miss at a time), capping a single non-prefetched CPU stream at **218 MB/s (`rd128`) / 237 MB/s (`touch`)**.

#### B. PSRAM write-allocate penalty vs L2-warm writes (with hardware L2 counters)
Tested on a `128 KB` PSRAM buffer (`1,024` L2 lines of 128 B) using the hardware L2 cache access counters (`CACHE_L2_DBUS0_ACS_HIT_CNT_REG`, `MISS_CNT_REG`, `NXTLVL_RD_CNT_REG`):

| Write mode (`128 KB`, `wr128`) | Idle MB/s | HDMI MB/s | L2 hits | L2 misses | PSRAM 128 B line reads (`NXTLVL_RD`) | Traffic explanation |
|---|---|---|---|---|---|---|
| **Cold write + write-back** (`evict` then `wr128` + `msync`) | **148** | **140** | 6,158 | 93,164 | **1,024** (= 128 KB read!) | **2x PSRAM traffic**: every store to a cold line stalls to read 128 B from PSRAM (Read-For-Ownership) before writing 128 B back |
| **L2-warm write + write-back** (already in L2, `wr128` + `msync`) | **350** | **318** | 6,158 | **0** | **0** | **1x PSRAM traffic**: 0 PSRAM reads; only the 128 KB write-back to PSRAM occurs |
| **L2-warm write (no write-back)** (stays dirty in L2) | **1,536** | **1,536** | 6,144 | **0** | **0** | **0x PSRAM traffic**: pure L1/L2 cache store speed |

- Writing from the CPU directly to a cold PSRAM buffer (like the 300 KB 8-bit framebuffer or 600 KB Z-buffer) pays a **2.36x bandwidth penalty (`148 MB/s` vs `350 MB/s`)** because the L2 cache is write-allocate with no full-line write-combining bypass. Rendering into an L1/L2-resident SRAM band buffer and DMA-copying it out avoids all Read-For-Ownership traffic.

#### C. Hardware prefetch (`autoload`), software prefetch, cache wrap-around, and manual preload
Tested over a cold `4 MB` PSRAM buffer:

| Mechanism & exact configuration | Touch MB/s | `rd32` MB/s | `rd128` MB/s (HDMI) | Seq chase (cyc) | Notes / hardware register behavior |
|---|---|---|---|---|---|
| **Autoload OFF** (`L1=off, L2=off`) | 237 | 187 | **218** (193) | 156.1 (180.7) | Default at boot (`0x00000002`); 32,768 L2 line reads for 4 MB |
| **L1D Autoload only** (`L1=on, L2=off, trig=miss+hit`) | 284 | 227 | **233** (203) | 156.1 (180.9) | Prefetches 64 B L1 lines; helps `touch`/`rd32` but limited by L2 miss latency on `rd128` |
| **L2 Autoload (`trig=miss`, `order=asc`)** | 278 | 278 | **278** (240) | **142.8** (165.1) | Prefetches next 128 B L2 line on L2 miss (+27.5% `rd128`) |
| **L2 Autoload (`trig=hit`, `order=asc`)** | **284** | **284** | **284** (246) | 156.1 (180.7) | Prefetches next 128 B L2 line on L2 hit (+30.3% `rd128`) |
| **L2 Autoload (`trig=miss+hit`, `order=asc`)** | **284** | **284** | **284** (246) | **142.8** (165.1) | **Best streaming setting**: +30.3% `rd128` (+48.7% `rd32`) and lowest sequential latency |
| **L1D + L2 Autoload (`L1=on, L2=on, trig=miss+hit`)** | **284** | **284** | **284** (246) | 145.9 (170.0) | Identical bandwidth to L2-only autoload; L2 autoload alone is sufficient |
| **L2 Autoload on descending scan (`order=desc` vs `asc`)** | - | - | **284** vs 213 | - | `order=1` (`desc`) gives the full +33% boost on backward scans; `order=0` (`asc`) gives 0% on backward scans |
| **L2 Autoload step (`CACHE_AUTOLOAD_STEP` bits `[6:5]`)** | - | - | - | - | **Hardwired to `0` (1 line) on ESP32-P4 v1.0**: writing `2<<5` to `CACHE_L2_CACHE_AUTOLOAD_CTRL_REG` reads back `0` |
| **SW `__builtin_prefetch(p + 128..512, 0, 3)`** | - | - | **217** (off) / **284** (on) | - | **0% effect**: GCC RISC-V target without `Zicbop` emits no hardware prefetch instruction |
| **Cache wrap-around (`CACHE_L1_DCACHE_WRAP` / `L2_CACHE_WRAP`)** | - | - | - | - | **Do NOT enable** unless MSPI Flash & PSRAM MR8 are reconfigured to matching wrap lengths; setting `CACHE_L2_CACHE_WRAP` while PSRAM `BurstLen=2048B` rotates L2 line words and corrupts XIP code |
| **Manual L2 Preload (`Cache_Start_L2_Cache_Preload`, 128 KB in 32 KB chunks)** | - | - | **357** (299) preload -> **1,756** read | - | Size reg `CACHE_L2_CACHE_PRELOAD_SIZE_REG` is **16-bit (`<= 32 KB` per call)**; `exc=0x0`, 4,096 L2 hits / 0 misses on subsequent read |
| **Manual L1D Preload (`Cache_Start_L1_DCache_Preload`, 16 KB in 8 KB chunks)** | - | - | **341** (cold) / **7,475** (L2-warm) -> **5,063** read | - | Size reg `CACHE_L1_DCACHE_PRELOAD_SIZE_REG` is **14-bit (`<= 8 KB` per call)**; `exc=0x0` |
| **Overlapped async L2 Preload (blk $i+1$) + CPU `2x rd128` (blk $i$)** | - | - | **285** (242) vs **208** (180) no-preload | - | **+37% effective throughput** when ping-pong preloading 16 KB blocks ahead of CPU compute |

#### D. Dual-core PSRAM contention vs CPU + DMA concurrency
- **Both CPU cores streaming cold PSRAM simultaneously (`4 MB` per core)**:
  - `line touch`: Core 0 `142 MB/s` + Core 1 `142 MB/s` = **284 MB/s total** (`247 MB/s` with HDMI active) — identical to 1 core with L2 autoload.
  - `rd128`: Core 0 `111 MB/s` + Core 1 `111 MB/s` = **223 MB/s total** (`195 MB/s` with HDMI active).
  - `wr128`: Core 0 `80 MB/s` + Core 1 `80 MB/s` = **160 MB/s total** (`139 MB/s` with HDMI active).
- **CPU `rd128` (cold PSRAM) + AXI-GDMA (`SRAM -> PSRAM`) running concurrently**:
  - CPU `160 MB/s` + AXI-GDMA `160 MB/s` = **321 MB/s combined** (`276 MB/s` with HDMI active) — unlike two CPUs sharing the L2 miss queue, AXI-GDMA accesses the MSPI AXI port independently of the L2 cache controller, increasing aggregate PSRAM utilization.

#### E. All 4 DMA engines on ESP32-P4 & exact settings to approach datasheet peak (`800 MB/s`)
ESP32-P4 has **four distinct DMA architectures**. Benchmarking all four reveals why standard `AXI-GDMA` topped out at ~172 MB/s while **DW-GDMA** and **2D-DMA (`DMA2D` / PPA)** reach **557–616 MB/s (70%–77% of the 800 MB/s PSRAM theoretical peak)** and **698–807 MB/s** in internal SRAM:

| DMA Engine & Configuration | `SRAM -> PSRAM` (MB/s) | `PSRAM -> SRAM` (MB/s) | `PSRAM -> PSRAM` (MB/s) | `SRAM -> SRAM` (MB/s) | Notes & Hardware Settings |
|---|---|---|---|---|---|
| **1. AHB-GDMA** (`esp_async_memcpy_install_gdma_ahb`, burst 4/8/16/32 B) | 34 (29) | 12 (11) | 9 (8) | 146 (146) | 32-bit AHB bus bridge; unsuitable for PSRAM bulk transfers |
| **2. AXI-GDMA** (`esp_async_memcpy_install_gdma_axi`, burst `16 B`) | 172 (164) | 105 (89) | 70 (59) | 163 (163) | Requires `cfg.backlog >= count` to avoid queue overflow |
| **2. AXI-GDMA** (`esp_async_memcpy_install_gdma_axi`, burst `32 B`) | 172 (172) | 130 (112) | 101 (85) | 163 (163) | |
| **2. AXI-GDMA** (`esp_async_memcpy_install_gdma_axi`, burst `64 B`) | **172** (172) | **171** (153) | **133** (113) | 163 (163) | Max burst size = 64 B; limited outstanding AXI requests |
| **2. AXI-GDMA direct link** (`gdma_new_axi_channel`, 10 KB / 32 KB pre-mounted) | **154** / **169** | - | - | - | Used by `main/bandcopy.c` (10,240 B = 16-row Quake band) |
| **3. DW-GDMA** (`dw_gdma_new_channel`, 64-bit, `out_req = 1`, `burst_items = 64`, `len = 16`) | 435 (374) | 223 (209) | 205 (171) | 420 (399) | Synopsys DesignWare AXI DMAC; `DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS` (no descriptor list in RAM!) |
| **3. DW-GDMA** (`dw_gdma_new_channel`, 64-bit, `out_req = 4..16`, `burst_items = 64`, `len = 16`) | **557** (484–544) | **337** (284–326) | **228** (195–200) *(456 MB/s rd+wr!)* | **698** (668–680) | **3.24x faster than AXI-GDMA (`SRAM->PSRAM`)**; `num_outstanding_requests >= 4` pipelines 128 B AXI bursts into MSPI |
| **3. DW-GDMA fixed-src fill (`memset`)** (`src = FIXED`, `dst = INCREMENT`, `out_req = 16`) | **616** (540) | - | - | **807** (778) | **77% of theoretical 800 MB/s PSRAM peak!** Zero CPU cycles |
| **4. 2D-DMA + PPA Fill** (`ppa_do_fill`, `ARGB8888`, burst `16 B` / `64 B` / `128 B`) | **325** / **535** / **602** (529) | - | - | - | `PPA_DATA_BURST_LENGTH_128` hits **602 MB/s** writing 1.2 MB to PSRAM |
| **4. 2D-DMA + PPA Fill** (`ppa_do_fill`, burst `128 B`, `RGB888` 3B / `RGB565` 2B) | **523** (477) / **348** (348) | - | - | **239** (`RGB565`) | |
| **4. 2D-DMA + PPA SRM** (`ppa_do_scale_rotate_mirror` 1:1, burst `128 B`, `ARGB8888` / `RGB888` / `RGB565`) | - | - | **94** / **77** / **62** out (**187** / **154** / **123** rd+wr) | - | `RGB565 -> RGB888` conversion: `85 MB/s` out (`142 MB/s` rd+wr) |
| **4. 2D-DMA + PPA Blend** (`ppa_do_blend`, burst `128 B`, `L8+A4 -> RGB888` 640x480) | - | - | **176** (164) out (**264** / **246** rd+wr, **5.23 ms**) | - | Used by `main/display.c` for hardware CLUT8 palette conversion |

Why **DW-GDMA (`557–616 MB/s`)** and **2D-DMA PPA-Fill (`602 MB/s`)** beat the CPU (`218–284 MB/s`) and `AXI-GDMA` (`172 MB/s`) by **2x–3.5x**:
1. **Pipelined Outstanding AXI Requests (`num_outstanding_requests = 4..16`)**: The CPU's L2 cache issues 1 PSRAM line request at a time and waits the full ~60-cycle MSPI command + 14-cycle read latency before issuing the next request. `DW-GDMA` keeps 4 to 16 128-byte AXI burst transactions (`width = DW_GDMA_TRANS_WIDTH_64`, `burst_len = 16`, `burst_items = 64`) in flight simultaneously, saturating the MSPI controller's queue. (Note: `DW-GDMA`'s AXI data bus on ESP32-P4 is 64 bits wide; setting `DW_GDMA_TRANS_WIDTH_128` clamps to 64 bits in hardware.)
2. **128-Byte Bursts (`PPA_DATA_BURST_LENGTH_128` / `DW-GDMA` 64b x 16)**: `AXI-GDMA` is capped at 64-byte bursts (`max_data_burst_size = 64`), whereas `2D-DMA` and `DW-GDMA` issue full 128-byte bursts matching the L2/MSPI burst granularity.
3. **Zero Descriptor Overhead in `DW-GDMA` Contiguous Mode**: Unlike `AXI-GDMA` (which requires a linked list of 4 KB descriptors in RAM), `DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS` transfers up to `4M` 64-bit items (`32 MB`) in a single hardware block register write (`block_ts` is 22 bits). One of the 4 `DW-GDMA` channels is used by the MIPI-DSI DPI bridge, leaving **3 free `DW-GDMA` channels** for the application.

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
