# Codebase Audit & New Optimization Candidates (From 52.58 FPS to 60+ FPS)

## 1. Progress Summary So Far

| Commit | Milestone / Optimization | Measured `demo1` FPS | Frame Time (`ms`) | Delta vs Baseline |
| :--- | :--- | ---: | ---: | ---: |
| `7cf290a` | Baseline (`640×480@60`, AXI-GDMA band copy) | `49.98 fps` | `20.01 ms` | — |
| `689f20d` | `DW-GDMA` contiguous band copy (`544 MB/s`) + scoped L2 hardware autoload (`surfcache` + `hunk`) | `51.16 fps` | `19.55 ms` | `+2.4%` |
| `04f2d8c` | **Candidate 4**: Compact `job_t` (`116 -> 68 B`) in internal SRAM + shrink `MAXWIDTH`/`MAXHEIGHT` (`640×480`) | `51.75 fps` | `19.32 ms` | `+3.5%` |
| `05e11a6` | **Bugfix**: Call `R_ComputeZRows()` after `R_RenderWorld()` so static entities (torches) populate `d_zrowmin/max` | `51.49 fps` | `19.42 ms` | `+3.0%` |
| `091a24f` | **Candidate 3**: `R_EmitEdge`/`R_RenderFace` non-blocking `fdiv.s` overlap + 1-cycle `fcvt.w.s rup` ceiling + `R_LeadingEdge` fast path + exact 2D surface-extent dynamic light culling in `R_MarkLights` (`-26%` surface rebuilds) | **`52.58 fps`** | **`19.02 ms`** | **`+5.2%`** |

To reach **60.0 fps (`16.67 ms/frame`)**, we need to shave **`2.35 ms/frame`** (`-12.4%`) from our current `19.02 ms/frame`.

---

## 2. Codebase Audit: Hidden Memory/FPU Latency, Wider-Word Ops, and Faster DMA

An instruction-level and symbol-table (`nm -u`) audit of all compiled object files in `quakegeneric` and `main` identified three categories of remaining bottlenecks:

### A. Loops Where Memory or FPU Latency Can Be Overlapped or Eliminated

1. **`R_DrawSurfaceBlock8_mip0..3` (`r_surf.c:314-506` & `r_surf_c1.c`) — Surface Cache Builder (~3.0 ms/frame across both cores)**
   - **What happens now**: Walks each 16-pixel (`mip0`), 8-pixel (`mip1`), or 4-pixel (`mip2`) row **backwards (`b = 15..0`) one byte at a time**, executing 16 individual byte loads `psource[b]`, 16 dependent `vid.colormap[(light & 0xFF00) + pix]` loads (from PSRAM hunk), and 16 individual byte stores `prowdest[b]`.
   - **Why it stalls**: Byte-by-byte backwards stores prevent full 32-bit word coalescing before the L1 store buffer, and each single-byte `psource[b]` load serializes address generation with the dependent `colormap` lookup.
   - **Fix**: Because `psource` (`texturemins` is always a multiple of 16) and `prowdest` (`surfcache_t.data` is 4-byte aligned and `surfwidth` is a multiple of 16/8/4 for `mip0..2`) are **always 32-bit aligned**:
     - Load **4 source texels at once** with a single 32-bit word load (`uint32_t s4 = *(const uint32_t *)&psource[b]`),
     - Issue 4 independent `colormap` byte loads in forward order (overlapping L1/L2 cache latency across the 4 lookups), and
     - Pack and store **4 lit texels at once** with a single 32-bit word store (`*(uint32_t *)&prowdest[b] = out4`), cutting source loads and surface-cache stores by **4×**.

2. **`D_DrawSpans8Job` (`d_jobs.c:248-314`) — World Span Rasterizer (35% of frame time)**
   - **What happens now**:
     - In the main 16-pixel loop, `pdest` is written via two separate 8-bit stores (`pdest[k] = p0; pdest[k+1] = p1;`).
     - In the tail loop (`count = 2..16`), `sstep = (snext - s) / (spancount - 1)` and `tstep = (tnext - t) / (spancount - 1)` execute **two blocking hardware integer divisions (`div`)** per span tail, followed by an un-paired 1-pixel-per-iteration byte fetch loop (`#pragma GCC unroll 0`) where every L2 cache hit stalls the CPU for 12 cycles with no overlapped second load.
   - **Fix**:
     - Replace the two integer divisions `/ (spancount - 1)` (where `spancount - 1` is always in `1..15`) with a **16-entry fixed-point reciprocal multiplication table** (`((int64_t)(snext - s) * recip16[spancount - 1]) >> 16` or `mulh`), saving ~60 cycles per span tail.
     - Unroll the tail loop by 2 (`while (spancount >= 2)`) with paired texel loads (`p0`, `p1`) so short spans overlap L2 hit latency just like the 16-pixel main loop.

3. **Accidental Software 64-bit `double` (`__adddf3`, `__muldf3`, `__subdf3`, `__ltdf2`) and `ceilf` Calls in Hot Loops**
   - **What happens now**: Inspecting undefined symbols (`nm -u *.obj`) revealed that because `cl.time` and `cl.oldtime` are `double`, GCC emits calls to **software 64-bit IEEE-754 double emulation routines** inside per-frame/per-element loops:
     - `R_DrawParticles` (`r_part.c:633, 648`): compares `float kill->die < double cl.time` for **every active particle** (up to 2,048/frame), calling `__extendsfdf2` + `__ltdf2` in software on every particle!
     - `R_PushDlights` (`r_light.c:126`): compares `l->die < cl.time` in software `double`.
     - `R_TextureAnimation` (`r_surf.c:210`) & `Turbulent8` (`d_scan.c:124`) & `R_GenTurbTile` (`r_surf.c:521`): computes `cl.time * 10` or `cl.time * SPEED` using software `__muldf3` + `__fixdfsi` on every animated or water surface!
     - `D_PolysetSetUpForLineScan` (`d_polyse.c:1038` & `d_polyse_c1.c`): calls external `newlib` `ceilf()` on every alias model triangle edge on Core 1 instead of the 1-cycle RISC-V `fcvt.w.s %0, %1, rup` instruction.
   - **Fix**: Snapshot `float cltime = (float)cl.time;` once before loops and replace `ceilf()` in `d_polyse.c` with `fcvt.w.s rup`.

---

### B. Operating on Wider Words (32-bit, 64-bit, and 128-bit PIE SIMD)

4. **`R_BuildLightMap` (`r_surf.c:132-186`) — Lightmap Scaling & Clamping**
   - **What happens now**: Clears, accumulates (`blocklights[i] += lightmap[i] * scale`), and bounds/shifts `blocklights[0 .. size-1]` (up to `18×18 = 324` 32-bit words) one element at a time using byte loads from `lightmap[i]`.
   - **Fix**: Load 4 lightmap bytes at once (`uint32_t lm4 = *(const uint32_t *)&lightmap[i]`) when aligned and unroll the accumulate and clamp loops 4× (or use 128-bit PIE SIMD `esp.vld.128.ip` / `esp.vst.128.ip`), cutting loop overhead and memory instructions by 4×.

5. **`R_MakeSky` (`r_sky.c:97-134`) & `R_GenSkyTile` (`r_sky.c:142-173`) — Sky Layer Compositing**
   - **What happens now**: Composites the `128×128` scrolling sky texture **one byte at a time** (`16,384` iterations of byte load, mask, OR, and byte store), even though `pnewsky` was declared `unsigned *` for 32-bit word operations.
   - **Fix**: When `(x + xshift) & SKYMASK <= 124`, load 4 sky bytes, 4 mask bytes, and 4 bottomsky bytes as **32-bit `uint32_t` words** and composite 4 pixels per instruction (`4,096` iterations instead of `16,384` — **4× faster**).

6. **`S_PaintChannels` / `Snd_WriteLinearBlastStereo16` (`snd_mix.c:32-271`) — Audio Mixing (1.5% of Core 0)**
   - **What happens now**: `paintbuffer[512]` (4 KB) is in `EXT_RAM_BSS_ATTR` (PSRAM), causing cold PSRAM cache misses during audio mixing on Core 0, and `Snd_WriteLinearBlastStereo16` writes clamped 16-bit stereo samples one `short` at a time.
   - **Fix**: Move the 4 KB `paintbuffer[512]` to internal SRAM and pack stereo pairs `(left & 0xFFFF) | (right << 16)` into a single 32-bit store.

---

### C. Faster DMA Opportunities (`DW-GDMA` & `PPA`)

7. **Banded Z-Buffer in Internal SRAM + Selective `DW-GDMA` / SRAM Z-Testing (Candidate 1)**
   - **What happens now**: World pixel spans (`D_DrawSpans8Job`) write into a 10 KB internal SRAM band (`640×16×1 B`) and copy out via `DW-GDMA` at `544 MB/s`, **but world Z-spans (`D_DrawZSpansJob`) still write directly into the 600 KB PSRAM `d_pzbuffer`**, paying cold PSRAM write-allocate stalls (`148 MB/s`) whenever `d_zrowmin[v] <= d_zrowmax[v]`.
   - **Fix**: Keep a 16-row Z-band (`640×16×2 B = 20 KB`, or double-buffered) in internal SRAM alongside `bandbuf`, or use `DW-GDMA` (`544 MB/s`) to burst-write only the active `[d_zrowmin .. d_zrowmax]` span of each band to PSRAM while the CPUs continue rendering in internal SRAM.

8. **Direct SRAM-Band-to-HDMI RGB888 Conversion via PPA (Candidate 2)**
   - **What happens now**: Every 16-row band (`10 KB`) is copied from internal SRAM to a 300 KB PSRAM `vid_buffer` via `DW-GDMA`, and then at the end of the frame the PPA reads that 300 KB `vid_buffer` (+ 150 KB dummy `A4` foreground) back from PSRAM to convert to RGB888 (`6.30 ms/frame` on the shared PSRAM bus).
   - **Fix**: Also point the PPA dummy foreground (`in_fg`, which has `fg_alpha_fix_val = 0` so its contents are completely ignored!) to a small buffer or convert finished bands directly from internal SRAM into `lcdbuf`, saving up to **450–750 KB/frame (`24–40 MB/s`)** of PSRAM bus contention.

---

## 3. Ranked New Candidates to Test Next

| Priority | Candidate | Target Files | Expected Gain | Target FPS |
| :--- | :--- | :--- | ---: | ---: |
| **1 (Next)** | **Candidate 7: Wider-Word & Latency-Overlap Pack**<br>• 32-bit 4-texel forward loads/stores in `R_DrawSurfaceBlock8_mip0..2` + 4× `R_BuildLightMap`<br>• Reciprocal multiply table + 2-pixel paired tail in `D_DrawSpans8Job`<br>• Eliminate software 64-bit `double` (`cl.time`) calls in `r_part.c`, `r_light.c`, `r_surf.c`, `d_scan.c` + replace `ceilf` with `fcvt.w.s rup` in `d_polyse.c`<br>• 32-bit 4-pixel word compositing in `R_MakeSky`<br>• Move 4 KB `paintbuffer` to internal SRAM + 32-bit stereo stores in `snd_mix.c` | `r_surf.c`<br>`d_jobs.c`<br>`r_part.c`<br>`d_polyse.c`<br>`r_sky.c`<br>`snd_mix.c` | **`+0.8 to +1.5 fps`**<br>(`-0.3 to -0.5 ms`) | **`~53.4 – 54.1 fps`** |
| **2** | **Candidate 8: Eliminate PPA Dummy Foreground PSRAM Reads / Optimize PPA Burst Contention**<br>• Investigate pointing `in_fg` in `convert_ppa` (`display.c`) or tiling PPA CLUT conversion so the 150 KB/frame dummy `A4` read comes from internal SRAM instead of PSRAM | `main/display.c` | **`+0.4 to +0.8 fps`**<br>(`-0.15 to -0.3 ms`) | **`~54.0 – 54.8 fps`** |
| **3** | **Candidate 1 + 2: Full SRAM Banded Z-Buffer & Entity/PPA Band Pipeline**<br>• Keep Z-buffer band in internal SRAM and finish world + Z + models per band before DMA/PPA transfer to HDMI scanout buffer | `r_edge.c`<br>`d_jobs.c`<br>`r_pipe.c`<br>`main/display.c` | **`+2.5 to +4.0 fps`**<br>(`-0.9 to -1.4 ms`) | **`~57.0 – 60.0+ fps`** |
