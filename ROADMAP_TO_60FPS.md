# Compute vs. Memory Analysis & Optimization Candidates to Hit 60 FPS (`16.67 ms/frame` at `640x480`)

## 1. Are We Compute Bound or Memory Bound? (And How Much Compute Is Actually Hiding Memory Latency?)

We are **split almost 55% compute-bound / 45% memory-stall-bound** across the whole frame, and—crucially—a large part of our "compute" time in the hottest loop is **hiding memory latency in the background**.

Here is what the hardware performance counters (`mcycle`, `minstret`, and branch mispredicts) on the ESP32-P4's single-issue, in-order RISC-V cores show across a 20 ms frame (38 ms of active work across both cores; 60 fps = 16.67 ms wall time = 33.3 ms across both cores):

| Engine Phase | Active Time (Both Cores) | Instructions Retired (`IPC`) | Pure Compute Cycles | Memory Stall Cycles (Waiting on Cache/PSRAM) | What's Really Happening |
| :--- | ---: | ---: | ---: | ---: | :--- |
| **World Texture Spans** (`D_DrawSpans8Job`) | **13.9 ms** (35%) | `0.68` | **68%** (9.5 ms) | **32%** (4.4 ms) | **Compute hides L2 latency (~12 cyc) and FPU divide (~20 cyc).** In an all-SRAM test, each pixel takes **13.2 cycles** (12.6 instructions); in PSRAM it takes **18.6 cycles** (+5.4 cycles of memory wait per pixel). |
| **Alias Models & Gun** (`D_PolysetDrawSpans8_c1`) | **6.1 ms** (15%) | `0.54` | **54%** (3.3 ms) | **46%** (2.8 ms) | **Half compute, half memory.** Draws straight to the PSRAM framebuffer and PSRAM Z-buffer while reading skins from PSRAM. |
| **Core 0 Serial World Setup** (`R_ScanEdges`, `R_RenderWorld`) | **5.8 ms** (15%) | `0.44` | **44%** (2.6 ms) | **56%** (3.2 ms) | **Heavily memory-latency bound.** Pointer-chasing BSP nodes, surfaces, and active edge lists that exceed the 256 KB L2 cache. |
| **Surface-Cache Builds** (`R_DrawSurfaceBlock8_mip0`) | **3.9 ms** (10%) | `0.64` | **64%** (2.5 ms) | **36%** (1.4 ms) | **Mostly compute + writeback.** Blending lightmaps and writing texels to PSRAM surface cache (~16 surfaces/frame). |
| **World Z-Buffer Fill** (`D_DrawZSpansJob`) | **1.3 ms** (3%) | `0.45` | **45%** (0.6 ms) | **55%** (0.7 ms) | **Memory write-allocate bound.** Writing 16-bit Z values to PSRAM for model bounding boxes. |
| **All Other (Game, Sound, 2D, OS)** | **7.1 ms** (18%) | `~0.48` | **~48%** (3.4 ms) | **~52%** (3.7 ms) | Mixed game logic, entity relinking, audio, HUD. |
| **Total Active Work** | **38.1 ms** | **`0.51` avg** | **~21.9 ms (57%)** | **~16.2 ms (43%)** | Plus **~2.0 ms idle on Core 1** waiting for Core 0's serial phases. |

### How Much of Active Compute Is Actually Hiding Memory Latency?

About **3 to 4 ms of the 21.9 ms of "active compute" is simultaneously hiding memory/FPU latency**—which is why earlier attempts to simply delete ALU instructions from the span loop gave **0.0 fps gain**:

1. **FPU Division Overlap in `D_DrawSpans8Job`**: Every 16-pixel span segment issues a hardware `fdiv.s` (`1.0 / zi`) at the *start* of the segment. That `fdiv.s` takes ~20 cycles in the FPU pipeline while the integer ALU executes the 16 pixel texel address calculations and loads. If you cut the integer instructions in half, the CPU just stalls waiting for `fdiv.s` to finish.
2. **L2 Hit / Prefetch Overlap (12-cycle L1 miss -> L2 hit)**: Because we paired texel loads (`p0/p1`, `p2/p3`) and enabled L2 hardware prefetch (`autoload`), the 6–9 integer instructions per pixel (`srai`, `mul`, `add`, `sb`) execute *while* the L1/L2 cache hierarchy streams the next cache line in the background. When two experiments in `PERF_LOG.md` cut the inner loop from 9 instructions/pixel to 5 instructions/pixel (constant-`t` fast path and 32-pixel subdivision), **fps did not move at all (49.64 vs 49.88 fps)** because those deleted instructions were just filling the time between memory fetches.
3. **Why Both Matter for 16.6 ms (60 fps)**:
   - We are currently at **51.16 fps (`19.55 ms/frame`)**, so we need to shave **2.9 ms/frame** (`-15%`).
   - Deleting instructions *inside* memory-waiting loops won't help unless we also cut the memory wait, and cutting memory traffic alone only has a ~1.5–2.0 ms ceiling unless we also reduce serial Core 0 work and rebalance Core 1's 2.0 ms of idle time.

---

## 2. Most Promising Optimization Candidates to Hit 16.6 ms (60 fps), Ranked in Plain English

To get from **19.55 ms (51.2 fps)** down to **16.67 ms (60.0 fps)**, we need **~2.9 ms** of wall-clock savings on Core 0 (the pacing core). Here are the strongest candidates in order of expected return:

### 1. Full Banded Rendering in Internal SRAM (Keep Z-Buffer, Alias Models, Particles & 2D Inside the SRAM Bands)
- **Expected savings**: **~1.0 to 1.4 ms/frame (+2.8 to +4.0 fps)**
- **How it works**: Right now, only the *world walls* are drawn into the 10 KB internal SRAM bands and DMA-copied to PSRAM. The world Z-buffer, monsters/weapons (alias models), particles, and HUD are still drawn later in the frame directly into PSRAM (paying 148 MB/s write-allocate stalls on PSRAM). Since we now have **107 KB of free internal SRAM** (and `DW-GDMA` copies bands at 544 MB/s), we can keep a 16-row Z-band (20 KB) in SRAM alongside the pixel band, bucket the recorded alias models/particles by band, and finish *each band completely* (world + Z + models + particles) in internal SRAM before `DW-GDMA` copies the finished pixels out.
- **Why it works**: Completely eliminates **all PSRAM Z-buffer reads and writes** (both world Z-fill and alias model Z-testing) and all alias model framebuffer writes in PSRAM. Diagnostics in `PERF_LOG.md` and `MEMORY_AND_COMPUTE_BUDGET.md` measured the upper bound of removing world Z + model pixel/Z PSRAM traffic at **+5.6% (~1.1 ms/frame)**, plus another **~0.3 ms** from eliminating the 1.2 MB PSRAM gun Z-buffer.

### 2. Convert Bands Directly from SRAM to the HDMI Framebuffer via PPA (Eliminate the 8-bit PSRAM Framebuffer Entirely)
- **Expected savings**: **~0.6 to 0.9 ms/frame (+1.7 to +2.5 fps)**
- **How it works**: Once a 16-row band (`640×16` = 10 KB) is completely drawn in internal SRAM, instead of `DW-GDMA` copying those 8-bit pixels to a 300 KB PSRAM `vid_buffer` and then having the PPA read that 300 KB `vid_buffer` (+ 150 KB dummy foreground) back out of PSRAM to convert to RGB888, kick the **PPA CLUT blend engine directly on the 10 KB SRAM band** (writing straight to the RGB888 HDMI scan-out buffer in PSRAM).
- **Why it works**: Saves **750 KB/frame (~38 MB/s) of PSRAM bandwidth** (300 KB band write + 300 KB PPA bg read + 150 KB PPA fg read) and frees **900 KB of PSRAM heap** (the three `vid_buffer` arrays). Diagnostic tests showed PPA PSRAM contention slows the CPUs by **4.4% (~0.85 ms/frame)**; cutting more than a third of the PPA's PSRAM traffic directly speeds up CPU cache misses on both cores.

### 3. Parallelize or Offload Core 0's Serial BSP Walk & Edge Setup (`R_RenderWorld` + `R_ScanEdges` = 5.8 ms on Core 0)
- **Expected savings**: **~0.6 to 1.0 ms/frame (+1.6 to +2.8 fps)**
- **How it works**: Core 1 sits **idle for ~2.0 ms every frame** while Core 0 walks the BSP tree (`R_RenderWorld`, 2.45 ms) and scans active edges (`R_ScanEdges`, 3.29 ms). Two concrete ways to attack this:
  1. **Split `R_RenderWorld` / entity clipping across both cores**, or have Core 0 walk the BSP leaves and push visible `msurface_t *` surfaces into an SRAM ring buffer while **Core 1 runs `R_RenderFace` / `R_ClipEdge`** (which is the bulk of `R_RenderWorld`).
  2. **Speed up `R_ScanEdges` on Core 0**: add software prefetch (`__builtin_prefetch(edge->next, 0, 1)`) in `R_StepActiveU` / `R_InsertNewEdges`, fast-path `R_LeadingEdge` for normal non-submodel surfaces, and shrink `MAXWIDTH`/`MAXHEIGHT` (`1280×1024` -> `640×480`) so edge tables pack tightly in SRAM.

### 4. Put the `jobs[]` Queue in Internal SRAM + Compact `job_t` to 64 Bytes
- **Expected savings**: **~0.3 to 0.5 ms/frame (+0.8 to +1.4 fps)**
- **How it works**: Right now `jobs[1024]` (~80 KB) lives in PSRAM. Every surface flush writes `job_t` structs on Core 0 (triggering cold PSRAM write-allocate misses at 148 MB/s) and reads them on Core 1. Compacting `job_t` to 64 bytes (one L1 cache line) for 512–768 jobs (32–48 KB) fits comfortably in our **107 KB of free internal SRAM**.
- **Why it works**: Turns ~350 job creations and claims per frame from 180-cycle PSRAM misses into 3-cycle L1 / 12-cycle SRAM hits.

### 5. Put Alias Model Rasterizer Scratch in the Unused 8 KB TCM (`0x3010_0000`) + 2-Pixel Paired Z-Testing
- **Expected savings**: **~0.3 to 0.5 ms/frame (+0.8 to +1.4 fps)**
- **How it works**:
  - Our memory benchmark showed the 8 KB TCM (`SPM_DRAM_ATTR`) has **2-cycle latency** and **4.5 GB/s** throughput, and **never evicts L1 D-cache lines**. Currently 7 KB of TCM is sitting completely unused. Placing the hot alias rasterizer tables (`a_spans`, `zspantable`, or `r_zrowmin`/`r_zrowmax`) in TCM avoids L1 cache thrashing against model skins.
  - Unrolling `D_PolysetDrawSpans8_c1` to process 2 pixels at a time with a single 32-bit Z-buffer load (`uint32_t z2 = *(uint32_t *)lpz`) halves the Z-buffer memory instructions during the 6.1 ms model pass.

### 6. Overlapped Hardware L2 Preload of the Next Surface Cache Block in `D_RunJob`
- **Expected savings**: **~0.2 to 0.4 ms/frame (+0.5 to +1.0 fps)**
- **How it works**: In `membench.c`, starting a non-blocking `Cache_Start_L2_Cache_Preload` (`<= 32 KB`) on block $i+1$ while the CPU computes on block $i$ boosted effective read bandwidth by **+37% (`208 -> 285 MB/s`)**. When a core starts drawing `jobs[i]` in `D_RunJob`, it can kick a non-blocking L2 preload for `jobs[i+2].cacheblock` (`width * height` bytes) so the next surface's texels are already in the 256 KB L2 cache when the core gets to it.

---

## 3. Summary: How These Add Up to 16.6 ms (60 fps)

| Step | Candidate | Frame Time | FPS |
| :--- | :--- | ---: | ---: |
| **Current State (`689f20d`)** | `DW-GDMA` bandcopy + scoped L2 autoload (`surfcache` + `hunk`) | **19.55 ms** | **51.2 fps** |
| **Candidate 4** | Compact `jobs[]` into internal SRAM + shrink `MAXWIDTH`/`MAXHEIGHT` | ~19.15 ms | ~52.2 fps |
| **Candidate 5 + 6** | 8 KB TCM hot scratch + 2-px alias Z-test + next-job L2 preload in `D_RunJob` | ~18.55 ms | ~53.9 fps |
| **Candidate 3** | Split `R_RenderWorld` (`R_RenderFace`) across both cores + `R_ScanEdges` fast path | ~17.75 ms | ~56.3 fps |
| **Candidate 1 + 2** | Full SRAM band pipeline (world + Z + models in SRAM, PPA converts bands directly from SRAM) | **~16.30 ms** | **~60.0 fps** |
