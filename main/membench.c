// Memory hierarchy & DMA micro-benchmark (CONFIG_QUAKE_MEMBENCH): latency, bandwidth,
// hardware cache counters, prefetch/preload/wrap settings, and all 4 DMA engines on ESP32-P4:
// - CPU memory levels: TCM (0x3010_0000), LP SRAM (0x5010_8000), L1D (16 KB / 32 KB),
//   L2MEM internal SRAM (cached 0x4FF0_0000 vs non-cached alias 0x8FF0_0000),
//   L2 cache (128 KB / 192 KB warm PSRAM), and cold PSRAM (0x4800_0000, 4 MB).
// - Cold write-allocate (read-for-ownership + writeback = 2x PSRAM traffic) vs L2-warm writeback (1x).
// - Hardware L1D & L2 cache access counters (hits, misses, next-level 128 B reads & writes).
// - Hardware prefetch (L1D & L2 autoload: trigger miss/hit/both, order asc/desc, step register),
//   software __builtin_prefetch, cache wrap-around mode, and hardware manual preload
//   (standalone L2/L1D preload in hardware-valid chunks + overlapped CPU compute + L2 preload).
// - Dual-core concurrent PSRAM streaming + CPU & DMA concurrency.
// - All 4 DMA architectures:
//   1. AXI-GDMA (esp_async_memcpy burst 16/32/64 B + direct pre-mounted descriptor links)
//   2. AHB-GDMA (esp_async_memcpy burst 4/8/16/32 B)
//   3. DW-GDMA (Synopsys AXI DMAC contiguous block mode, varying outstanding requests/burst/width + fixed-src memset)
//   4. 2D-DMA + PPA (ppa_do_fill, ppa_do_scale_rotate_mirror, ppa_do_blend across burst lengths & pixel formats)
// Results are printed as "MEMBENCH" lines; see MEMORY_AND_COMPUTE_BUDGET.md.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_async_memcpy.h"
#include "esp_private/gdma.h"
#include "esp_private/gdma_link.h"
#include "esp_private/dw_gdma.h"
#include "hal/gdma_types.h"
#include "hal/dma_types.h"
#include "hal/cache_ll.h"
#include "driver/ppa.h"
#include "esp32p4/rom/cache.h"
#include "soc/cache_reg.h"
#include "soc/spi_mem_s_reg.h"
#include "sdkconfig.h"
#include "quakegeneric.h"

#if CONFIG_QUAKE_MEMBENCH

#define LINE 128	//L2 line; also >= the 64-byte L1 line

static inline uint32_t cycles(void) { uint32_t v; __asm__ volatile ("csrr %0, mcycle" : "=r"(v)); return v; }

static double mhz=CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;	//measured in membench_run
static double mbs(double bytes, uint32_t cyc) { return cyc ? bytes/(cyc/mhz) : 0; }	//bytes per us = MB/s

static uint32_t rng=12345;
static uint32_t rnd(void) { rng=rng*1103515245u+12345u; return rng>>8; }

static void writeback(uint8_t *p, size_t n) {
	if (!Cache_Address_Through_Cache((uint32_t)p)) return;
	esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}
//write back and drop a buffer from L1 and L2, so the next access misses all the way to memory
static void evict(uint8_t *p, size_t n) {
	if (!Cache_Address_Through_Cache((uint32_t)p)) return;
	esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_INVALIDATE|ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

//---- hardware L1D / L2 cache performance counters (DBUS0 + DBUS1)

typedef struct {
	uint32_t l1_hit, l1_miss, l1_conflict;
	uint32_t l2_hit, l2_miss, l2_nxt_rd, l2_nxt_wr;
} cache_cnt_t;

static void cnt_start(void) {
	uint32_t l1_ena = CACHE_L1_DBUS0_CNT_ENA | CACHE_L1_DBUS1_CNT_ENA;
	uint32_t l1_clr = CACHE_L1_DBUS0_CNT_CLR | CACHE_L1_DBUS1_CNT_CLR;
	REG_WRITE(CACHE_L1_CACHE_ACS_CNT_CTRL_REG, l1_ena | l1_clr);
	REG_WRITE(CACHE_L1_CACHE_ACS_CNT_CTRL_REG, l1_ena);

	uint32_t l2_ena = CACHE_L2_DBUS0_CNT_ENA | CACHE_L2_DBUS1_CNT_ENA;
	uint32_t l2_clr = CACHE_L2_DBUS0_CNT_CLR | CACHE_L2_DBUS1_CNT_CLR;
	REG_WRITE(CACHE_L2_CACHE_ACS_CNT_CTRL_REG, l2_ena | l2_clr);
	REG_WRITE(CACHE_L2_CACHE_ACS_CNT_CTRL_REG, l2_ena);
}

static cache_cnt_t cnt_read(void) {
	cache_cnt_t c;
	c.l1_hit      = REG_READ(CACHE_L1_DBUS0_ACS_HIT_CNT_REG)      + REG_READ(CACHE_L1_DBUS1_ACS_HIT_CNT_REG);
	c.l1_miss     = REG_READ(CACHE_L1_DBUS0_ACS_MISS_CNT_REG)     + REG_READ(CACHE_L1_DBUS1_ACS_MISS_CNT_REG);
	c.l1_conflict = REG_READ(CACHE_L1_DBUS0_ACS_CONFLICT_CNT_REG) + REG_READ(CACHE_L1_DBUS1_ACS_CONFLICT_CNT_REG);
	c.l2_hit      = REG_READ(CACHE_L2_DBUS0_ACS_HIT_CNT_REG)      + REG_READ(CACHE_L2_DBUS1_ACS_HIT_CNT_REG);
	c.l2_miss     = REG_READ(CACHE_L2_DBUS0_ACS_MISS_CNT_REG)     + REG_READ(CACHE_L2_DBUS1_ACS_MISS_CNT_REG);
	c.l2_nxt_rd   = REG_READ(CACHE_L2_DBUS0_ACS_NXTLVL_RD_CNT_REG)+ REG_READ(CACHE_L2_DBUS1_ACS_NXTLVL_RD_CNT_REG);
	c.l2_nxt_wr   = REG_READ(CACHE_L2_DBUS0_ACS_NXTLVL_WR_CNT_REG)+ REG_READ(CACHE_L2_DBUS1_ACS_NXTLVL_WR_CNT_REG);
	return c;
}

//---- latency: pointer chase through one node per 128-byte line

//link the n lines of buf into one cycle, in random or address order
static void **chain(uint8_t *buf, int n, int random) {
	int *order=malloc(n*sizeof(int));
	if (!order) return NULL;
	for (int i=0; i<n; i++) order[i]=i;
	if (random) for (int i=n-1; i>0; i--) { int j=rnd()%(i+1); int t=order[i]; order[i]=order[j]; order[j]=t; }
	for (int i=0; i<n; i++) *(void **)(buf+order[i]*LINE)=buf+order[(i+1)%n]*LINE;
	void **start=(void **)(buf+order[0]*LINE);
	free(order);
	return start;
}

//cycles per dependent load; `cold` = evict the buffer first and walk it once (every load misses)
static double chase(uint8_t *buf, size_t size, int random, int cold) {
	int n=size/LINE;
	void **p=chain(buf, n, random);
	if (!p) return 0;
	int steps;
	if (cold) { evict(buf, size); steps=n; }
	else { writeback(buf, size); for (int i=0; i<n; i++) p=(void **)*p; steps=n<4096 ? 32768 : n; }
	uint32_t c0=cycles();
	for (int i=0; i<steps; i++) p=(void **)*p;
	uint32_t c1=cycles();
	__asm__ volatile ("" :: "r"(p));
	return (double)(c1-c0)/steps;
}

//four independent random chains walked together: if the memory system can serve misses in
//parallel, this costs less per load than one chain
static double chase4(uint8_t *buf, size_t size, int cold) {
	size_t q=size/4/LINE*LINE;
	void **p[4];
	for (int k=0; k<4; k++) if (!(p[k]=chain(buf+k*q, q/LINE, 1))) return 0;
	int steps=q/LINE;
	if (cold) evict(buf, size);
	else { writeback(buf, size); for (int i=0; i<steps; i++) for (int k=0; k<4; k++) p[k]=(void **)*p[k]; if (steps<1024) steps=8192; }
	void **a=p[0], **b=p[1], **c=p[2], **d=p[3];
	uint32_t c0=cycles();
	for (int i=0; i<steps; i++) { a=(void **)*a; b=(void **)*b; c=(void **)*c; d=(void **)*d; }
	uint32_t c1=cycles();
	__asm__ volatile ("" :: "r"(a), "r"(b), "r"(c), "r"(d));
	return (double)(c1-c0)/(steps*4);
}

//---- bandwidth kernels; each returns cycles for one pass over [p, p+n)

static uint32_t rd32(const uint8_t *p, size_t n) {
	const uint32_t *q=(const uint32_t *)p, *e=(const uint32_t *)(p+n);
	uint32_t sum=0, c0=cycles();
	for (; q<e; q+=8) sum+=q[0]+q[1]+q[2]+q[3]+q[4]+q[5]+q[6]+q[7];
	uint32_t c1=cycles();
	__asm__ volatile ("" :: "r"(sum));
	return c1-c0;
}

static uint32_t rd128(const uint8_t *p, size_t n) {
	const uint8_t *e=p+n;
	uint32_t c0=cycles();
	while (p<e)
		__asm__ volatile ("esp.vld.128.ip q0,%0,16\n esp.vld.128.ip q1,%0,16\n esp.vld.128.ip q2,%0,16\n esp.vld.128.ip q3,%0,16\n"
				"esp.vld.128.ip q4,%0,16\n esp.vld.128.ip q5,%0,16\n esp.vld.128.ip q6,%0,16\n esp.vld.128.ip q7,%0,16" : "+r"(p) :: "memory");
	return cycles()-c0;
}

//one load per 64-byte L1 line: the CPU does almost nothing, so this is the line fill rate
static uint32_t touch(const uint8_t *p, size_t n) {
	const volatile uint32_t *q=(const volatile uint32_t *)p, *e=(const volatile uint32_t *)(p+n);
	uint32_t sum=0, c0=cycles();
	for (; q<e; q+=64) sum+=q[0]+q[16]+q[32]+q[48];
	uint32_t c1=cycles();
	__asm__ volatile ("" :: "r"(sum));
	return c1-c0;
}

//stores, including writing the dirty lines back at the end
static uint32_t wr32(uint8_t *p, size_t n) {
	uint32_t *q=(uint32_t *)p, *e=(uint32_t *)(p+n), v=n;
	uint32_t c0=cycles();
	for (; q<e; q+=8) { q[0]=v; q[1]=v; q[2]=v; q[3]=v; q[4]=v; q[5]=v; q[6]=v; q[7]=v; }
	writeback(p, n);
	return cycles()-c0;
}

static uint32_t wr128(uint8_t *p, size_t n) {
	uint8_t *s=p, *e=p+n;
	uint32_t c0=cycles();
	while (p<e)
		__asm__ volatile ("esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16\n"
				"esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16" : "+r"(p) :: "memory");
	writeback(s, n);
	return cycles()-c0;
}

//stores without trailing writeback (measures pure store-into-cache speed when lines stay in L1/L2)
static uint32_t wr128_no_wb(uint8_t *p, size_t n) {
	uint8_t *e=p+n;
	uint32_t c0=cycles();
	while (p<e)
		__asm__ volatile ("esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16\n"
				"esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16\n esp.vst.128.ip q0,%0,16" : "+r"(p) :: "memory");
	return cycles()-c0;
}

static uint32_t cpy(uint8_t *p, size_t n) {
	uint32_t c0=cycles();
	memcpy(p+n/2, p, n/2);
	writeback(p+n/2, n/2);
	return cycles()-c0;
}

static uint32_t cpy32(uint8_t *p, size_t n) {
	uint32_t *d=(uint32_t *)(p+n/2), *s=(uint32_t *)p, *e=(uint32_t *)(p+n/2);
	uint32_t c0=cycles();
	while (s<e) { d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; d+=4; s+=4; }
	return cycles()-c0;
}

//MB/s of a kernel; small buffers are warmed and repeated (cache hits), large ones start cold
static double run(uint32_t (*k)(const uint8_t *, size_t), uint8_t *p, size_t n, int cold, double scale) {
	uint32_t c=0;
	int reps=cold ? 1 : 16;
	if (cold) evict(p, n); else k(p, n);
	for (int r=0; r<reps; r++) c+=k(p, n);
	return mbs((double)n*reps*scale, c);
}
#define RUN(k, p, n, cold) run((uint32_t (*)(const uint8_t *, size_t))(k), p, n, cold, 1.0)

static void region(const char *what, uint8_t *p, size_t n, int cold) {
	uintptr_t addr = (uintptr_t)p;
	int pie_ok = Cache_Address_Through_Cache((uint32_t)addr) || (addr >= 0x30100000 && addr < 0x30102000);
	printf("MEMBENCH %-28s %7u B: latency random %6.1f, sequential %6.1f, 4 chains %6.1f cycles/load\n",
			what, (unsigned)n, chase(p, n, 1, cold), chase(p, n, 0, cold), chase4(p, n, cold));
	double r32=RUN(rd32, p, n, cold), r128=pie_ok ? RUN(rd128, p, n, cold) : 0, t=RUN(touch, p, n, cold);
	double w32=RUN(wr32, p, n, cold), w128=pie_ok ? RUN(wr128, p, n, cold) : 0;
	double cp=run((uint32_t (*)(const uint8_t *, size_t))(pie_ok ? cpy : cpy32), p, n, cold, 0.5);
	printf("MEMBENCH %-28s %7u B: read 32-bit %6.0f, 128-bit %6.0f, line touch %6.0f | write 32-bit %6.0f, 128-bit %6.0f | memcpy %6.0f MB/s%s\n",
			what, (unsigned)n, r32, r128, t, w32, w128, cp, pie_ok ? "" : " (PIE 128b unsupported on bus)");
}

//---- PSRAM write-allocate vs L2-warm writeback + hardware cache counters

static void write_allocate_bench(uint8_t *p) {
	const size_t n = 128 * 1024;	//fits in 256 KB L2
	// 1. Cold write-allocate + writeback: evict first, then wr128 (which writes + flushes to PSRAM)
	evict(p, n);
	cnt_start();
	uint32_t c_cold = wr128(p, n);
	cache_cnt_t cc_cold = cnt_read();

	// 2. Warm L2 write + writeback: warm in L2 via rd128, then wr128 (writes in cache + flushes to PSRAM)
	evict(p, n);
	rd128(p, n);
	cnt_start();
	uint32_t c_warm_wb = wr128(p, n);
	cache_cnt_t cc_warm_wb = cnt_read();

	// 3. Warm L2 write without writeback (stays dirty in L2)
	rd128(p, n);
	cnt_start();
	uint32_t c_warm_nowb = wr128_no_wb(p, n);
	cache_cnt_t cc_warm_nowb = cnt_read();
	writeback(p, n);

	printf("MEMBENCH PSRAM write-allocate vs warm (%u KB = %u L2 lines):\n", (unsigned)(n/1024), (unsigned)(n/128));
	printf("MEMBENCH   cold write + writeback : %6.0f MB/s | L2 hit %6lu, miss %6lu | PSRAM 128B rd %5lu, wr %5lu (2x traffic: RFO + WB)\n",
			mbs(n, c_cold), (unsigned long)cc_cold.l2_hit, (unsigned long)cc_cold.l2_miss,
			(unsigned long)cc_cold.l2_nxt_rd, (unsigned long)cc_cold.l2_nxt_wr);
	printf("MEMBENCH   L2-warm write + WB     : %6.0f MB/s | L2 hit %6lu, miss %6lu | PSRAM 128B rd %5lu, wr %5lu (1x traffic: WB only)\n",
			mbs(n, c_warm_wb), (unsigned long)cc_warm_wb.l2_hit, (unsigned long)cc_warm_wb.l2_miss,
			(unsigned long)cc_warm_wb.l2_nxt_rd, (unsigned long)cc_warm_wb.l2_nxt_wr);
	printf("MEMBENCH   L2-warm write (no WB)  : %6.0f MB/s | L2 hit %6lu, miss %6lu | PSRAM 128B rd %5lu, wr %5lu (0x PSRAM traffic)\n",
			mbs(n, c_warm_nowb), (unsigned long)cc_warm_nowb.l2_hit, (unsigned long)cc_warm_nowb.l2_miss,
			(unsigned long)cc_warm_nowb.l2_nxt_rd, (unsigned long)cc_warm_nowb.l2_nxt_wr);
}

//---- prefetch: hardware autoload, software __builtin_prefetch, cache wrap, and manual preload

static void autoload_cfg(int l1, int l2, int trigger, int order, uint8_t *p, size_t n) {
	struct l1_dcache_l2_autoload_config c={ .gid=0, .order=order, .trigger=trigger, .ena0=1, .addr0=(uint32_t)p, .size0=n };
	Cache_Disable_L1_DCache_Autoload();
	Cache_Disable_L2_Cache_Autoload();
	if (l1) { Cache_Config_L1_DCache_Autoload(&c); Cache_Enable_L1_DCache_Autoload(); }
	if (l2) { Cache_Config_L2_Cache_Autoload(&c); Cache_Enable_L2_Cache_Autoload(); }
}

// descending 128-bit read to test descending hardware autoload
static uint32_t rd128_desc(const uint8_t *p, size_t n) {
	const uint8_t *q = p + n - 128;
	uint32_t c0 = cycles();
	while (q >= p) {
		const uint8_t *t = q;
		__asm__ volatile ("esp.vld.128.ip q0,%0,16\n esp.vld.128.ip q1,%0,16\n esp.vld.128.ip q2,%0,16\n esp.vld.128.ip q3,%0,16\n"
				"esp.vld.128.ip q4,%0,16\n esp.vld.128.ip q5,%0,16\n esp.vld.128.ip q6,%0,16\n esp.vld.128.ip q7,%0,16" : "+r"(t) :: "memory");
		q -= 128;
	}
	return cycles() - c0;
}

// software __builtin_prefetch + 128-bit read at configurable prefetch distance
static uint32_t rd128_swpf_128(const uint8_t *p, size_t n) {
	const uint8_t *e = p + n;
	uint32_t c0 = cycles();
	while (p < e) {
		__builtin_prefetch(p + 128, 0, 3);
		__asm__ volatile ("esp.vld.128.ip q0,%0,16\n esp.vld.128.ip q1,%0,16\n esp.vld.128.ip q2,%0,16\n esp.vld.128.ip q3,%0,16\n"
				"esp.vld.128.ip q4,%0,16\n esp.vld.128.ip q5,%0,16\n esp.vld.128.ip q6,%0,16\n esp.vld.128.ip q7,%0,16" : "+r"(p) :: "memory");
	}
	return cycles() - c0;
}

static uint32_t rd128_swpf_256(const uint8_t *p, size_t n) {
	const uint8_t *e = p + n;
	uint32_t c0 = cycles();
	while (p < e) {
		__builtin_prefetch(p + 256, 0, 3);
		__asm__ volatile ("esp.vld.128.ip q0,%0,16\n esp.vld.128.ip q1,%0,16\n esp.vld.128.ip q2,%0,16\n esp.vld.128.ip q3,%0,16\n"
				"esp.vld.128.ip q4,%0,16\n esp.vld.128.ip q5,%0,16\n esp.vld.128.ip q6,%0,16\n esp.vld.128.ip q7,%0,16" : "+r"(p) :: "memory");
	}
	return cycles() - c0;
}

static uint32_t rd128_swpf_512(const uint8_t *p, size_t n) {
	const uint8_t *e = p + n;
	uint32_t c0 = cycles();
	while (p < e) {
		__builtin_prefetch(p + 512, 0, 3);
		__asm__ volatile ("esp.vld.128.ip q0,%0,16\n esp.vld.128.ip q1,%0,16\n esp.vld.128.ip q2,%0,16\n esp.vld.128.ip q3,%0,16\n"
				"esp.vld.128.ip q4,%0,16\n esp.vld.128.ip q5,%0,16\n esp.vld.128.ip q6,%0,16\n esp.vld.128.ip q7,%0,16" : "+r"(p) :: "memory");
	}
	return cycles() - c0;
}

static void prefetch_bench(uint8_t *p, size_t n) {
	static const char *const trig[]={"miss", "hit", "miss+hit"};
	uint32_t l1_orig=REG_READ(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG), l2_orig=REG_READ(CACHE_L2_CACHE_AUTOLOAD_CTRL_REG);

	// 1. Hardware autoload combinations (L1 on/off, L2 on/off, trigger miss / hit / miss+hit)
	for (int mode=0; mode<4; mode++)
		for (int t=0; t<3; t++) {
			if (mode==0 && t>0) continue;
			if (mode==1 && t==1) continue; // keep L1-only concise (miss & miss+hit)
			autoload_cfg(mode&1, mode&2, t, 0, p, n);
			double tr=RUN(touch, p, n, 1), r=RUN(rd32, p, n, 1);
			evict(p, n);
			cnt_start();
			uint32_t c128 = rd128(p, n);
			cache_cnt_t cc = cnt_read();
			double r128 = mbs(n, c128);
			double seq=chase(p, n, 0, 1);
			printf("MEMBENCH autoload L1 %-3s L2 %-3s trig %-8s: touch %6.0f, rd32 %6.0f, rd128 %6.0f MB/s | seq %5.1f cyc | L2 hit %5lu miss %5lu PSRAM_rd %5lu\n",
					mode&1 ? "on" : "off", mode&2 ? "on" : "off", mode ? trig[t] : "-",
					tr, r, r128, seq, (unsigned long)cc.l2_hit, (unsigned long)cc.l2_miss, (unsigned long)cc.l2_nxt_rd);
		}

	// 2. Descending order autoload test (order=1 vs order=0 on descending scan)
	for (int ord=0; ord<2; ord++) {
		autoload_cfg(0, 1, 2, ord, p, n);
		evict(p, n);
		uint32_t cd = rd128_desc(p, n);
		printf("MEMBENCH autoload L2 order %-4s on descending rd128 (%u KB): %6.0f MB/s\n",
				ord ? "desc" : "asc", (unsigned)(n/1024), mbs(n, cd));
	}

	// 3. Test whether CACHE_AUTOLOAD_STEP (bits [6:5]) is writable on ESP32-P4 silicon
	autoload_cfg(0, 1, 2, 0, p, n);
	uint32_t before_step = REG_READ(CACHE_L2_CACHE_AUTOLOAD_CTRL_REG);
	REG_WRITE(CACHE_L2_CACHE_AUTOLOAD_CTRL_REG, (before_step & ~(3u << 5)) | (2u << 5));
	uint32_t after_step = REG_READ(CACHE_L2_CACHE_AUTOLOAD_CTRL_REG);
	printf("MEMBENCH autoload step bits [6:5] test: wrote step=2 -> ctrl reg before=0x%08lx after=0x%08lx (step field=%lu)\n",
			(unsigned long)before_step, (unsigned long)after_step,
			(unsigned long)((after_step >> 5) & 3u));

	Cache_Disable_L1_DCache_Autoload();
	Cache_Disable_L2_Cache_Autoload();
	REG_WRITE(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG, l1_orig);
	REG_WRITE(CACHE_L2_CACHE_AUTOLOAD_CTRL_REG, l2_orig);

	// 4. Software __builtin_prefetch test (with L2 autoload OFF and ON)
	for (int hw_l2=0; hw_l2<2; hw_l2++) {
		autoload_cfg(0, hw_l2, 2, 0, p, n);
		double base = RUN(rd128, p, n, 1);
		double pf128 = RUN(rd128_swpf_128, p, n, 1);
		double pf256 = RUN(rd128_swpf_256, p, n, 1);
		double pf512 = RUN(rd128_swpf_512, p, n, 1);
		printf("MEMBENCH sw __builtin_prefetch (L2 autoload %-3s): none %6.0f | +128B %6.0f | +256B %6.0f | +512B %6.0f MB/s\n",
				hw_l2 ? "on" : "off", base, pf128, pf256, pf512);
	}
	Cache_Disable_L1_DCache_Autoload();
	Cache_Disable_L2_Cache_Autoload();
	REG_WRITE(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG, l1_orig);
	REG_WRITE(CACHE_L2_CACHE_AUTOLOAD_CTRL_REG, l2_orig);

	// 5. Manual hardware preload (chunked within hardware register size limits!)
	// L2 preload size register is 16-bit (max 32 KB per call); L1D preload size register is 14-bit (max 8 KB per call).
	const size_t l2_total = 128 * 1024, l2_chunk = 32 * 1024;
	evict(p, l2_total);
	cnt_start();
	uint32_t c0 = cycles();
	for (size_t off = 0; off < l2_total; off += l2_chunk) {
		uint32_t a = Cache_Start_L2_Cache_Preload((uint32_t)(p + off), l2_chunk, 0);
		while (!Cache_L2_Cache_Preload_Done()) ;
		Cache_End_L2_Cache_Preload(a);
	}
	uint32_t c1 = cycles();
	uint32_t l2_exc = REG_READ(CACHE_L2_CACHE_SYNC_PRELOAD_EXCEPTION_REG);
	// Now read the preloaded 128 KB once with rd128 (without re-evicting!) to verify it is warm in L2
	uint32_t c_read_l2 = rd128(p, l2_total);
	cache_cnt_t cc_l2 = cnt_read();
	printf("MEMBENCH preload PSRAM -> L2 (%u KB in %u KB chunks, exc=0x%lx): preload %6.0f MB/s -> then rd128 %6.0f MB/s (L2 hit %lu, miss %lu)\n",
			(unsigned)(l2_total/1024), (unsigned)(l2_chunk/1024), (unsigned long)l2_exc,
			mbs(l2_total, c1 - c0), mbs(l2_total, c_read_l2),
			(unsigned long)cc_l2.l2_hit, (unsigned long)cc_l2.l2_miss);

	// L1D preload from cold PSRAM (16 KB in 8 KB chunks)
	const size_t l1_total = 16 * 1024, l1_chunk = 8 * 1024;
	evict(p, l1_total);
	uint32_t c2 = cycles();
	for (size_t off = 0; off < l1_total; off += l1_chunk) {
		uint32_t a = Cache_Start_L1_DCache_Preload((uint32_t)(p + off), l1_chunk, 0);
		while (!Cache_L1_DCache_Preload_Done()) ;
		Cache_End_L1_DCache_Preload(a);
	}
	uint32_t c3 = cycles();
	uint32_t l1_exc = REG_READ(CACHE_SYNC_L1_CACHE_PRELOAD_EXCEPTION_REG);
	uint32_t c_read_l1_cold = rd128(p, l1_total);
	printf("MEMBENCH preload cold PSRAM -> L1D (%u KB in %u KB chunks, exc=0x%lx): preload %6.0f MB/s -> then rd128 %6.0f MB/s\n",
			(unsigned)(l1_total/1024), (unsigned)(l1_chunk/1024), (unsigned long)l1_exc,
			mbs(l1_total, c3 - c2), mbs(l1_total, c_read_l1_cold));

	// L1D preload from L2-warm PSRAM (16 KB in 8 KB chunks)
	// First warm 64 KB in L2, then touch upper 32 KB so lower 16 KB is evicted from L1D but stays in L2
	rd128(p, 64 * 1024);
	uint32_t c4 = cycles();
	for (size_t off = 0; off < l1_total; off += l1_chunk) {
		uint32_t a = Cache_Start_L1_DCache_Preload((uint32_t)(p + off), l1_chunk, 0);
		while (!Cache_L1_DCache_Preload_Done()) ;
		Cache_End_L1_DCache_Preload(a);
	}
	uint32_t c5 = cycles();
	uint32_t c_read_l1_warm = rd128(p, l1_total);
	printf("MEMBENCH preload L2-warm -> L1D    (%u KB in %u KB chunks): preload %6.0f MB/s -> then rd128 %6.0f MB/s\n",
			(unsigned)(l1_total/1024), (unsigned)(l1_chunk/1024),
			mbs(l1_total, c5 - c4), mbs(l1_total, c_read_l1_warm));

	// 7. Overlapped background L2 preload + CPU compute on block i while preloading block i+1
	const size_t stream_n = 1024 * 1024, blk = 16 * 1024;
	// (a) Baseline without preload: for each 16 KB block, read + do 4 passes of ALU/vector work in L2
	evict(p, stream_n);
	uint32_t c_no_ovl0 = cycles();
	for (size_t off = 0; off < stream_n; off += blk) {
		rd128(p + off, blk);
		rd128(p + off, blk);
	}
	uint32_t c_no_ovl = cycles() - c_no_ovl0;

	// (b) Overlapped: start preload of block 0, then in loop preload block i+1 while CPU runs rd128 twice on block i
	evict(p, stream_n);
	uint32_t c_ovl0 = cycles();
	uint32_t pa = Cache_Start_L2_Cache_Preload((uint32_t)p, blk, 0);
	while (!Cache_L2_Cache_Preload_Done()) ;
	Cache_End_L2_Cache_Preload(pa);
	for (size_t off = 0; off < stream_n; off += blk) {
		if (off + blk < stream_n)
			pa = Cache_Start_L2_Cache_Preload((uint32_t)(p + off + blk), blk, 0);
		rd128(p + off, blk);
		rd128(p + off, blk);
		if (off + blk < stream_n) {
			while (!Cache_L2_Cache_Preload_Done()) ;
			Cache_End_L2_Cache_Preload(pa);
		}
	}
	uint32_t c_ovl = cycles() - c_ovl0;
	printf("MEMBENCH overlapped L2 preload + 2x rd128 (%u KB in %u KB blocks): without preload %6.0f MB/s, with async L2 preload %6.0f MB/s (effective PSRAM stream)\n",
			(unsigned)(stream_n/1024), (unsigned)(blk/1024), mbs(stream_n, c_no_ovl), mbs(stream_n, c_ovl));
}

//---- both cores streaming from PSRAM at the same time

static volatile int dual_go, dual_done;
static volatile uint32_t dual_cyc;
static uint8_t *dual_buf;
static size_t dual_n;
static uint32_t (*dual_kernel)(const uint8_t *, size_t);

static void dual_task(void *arg) {
	while (!dual_go) ;
	dual_cyc=dual_kernel(dual_buf, dual_n);
	dual_done=1;
	vTaskDelete(NULL);
}

static void dual_bench(uint8_t *p0, uint8_t *p1, size_t n) {
	static const struct { const char *name; uint32_t (*k)(const uint8_t *, size_t); } ks[]={
		{"line touch", touch}, {"read 128-bit", rd128}, {"write 128-bit", (uint32_t (*)(const uint8_t *, size_t))wr128}};
	for (int i=0; i<3; i++) {
		evict(p0, n); evict(p1, n);
		dual_go=dual_done=0; dual_buf=p1; dual_n=n; dual_kernel=ks[i].k;
		xTaskCreatePinnedToCore(dual_task, "mb1", 3072, NULL, 5, NULL, 1);
		vTaskDelay(2);
		dual_go=1;
		uint32_t c=ks[i].k(p0, n);
		while (!dual_done) ;
		double a=mbs(n, c), b=mbs(n, dual_cyc);
		printf("MEMBENCH both cores, PSRAM %-14s: core 0 %6.0f + core 1 %6.0f = %6.0f MB/s\n", ks[i].name, a, b, a+b);
	}
}

//---- DMA Engine 1 & 2: AXI-GDMA and AHB-GDMA via esp_async_memcpy + direct AXI-GDMA descriptor links

static volatile int dma_left;
static bool IRAM_ATTR dma_cb(async_memcpy_handle_t h, async_memcpy_event_t *e, void *arg) { dma_left--; return false; }

static void gdma_async_memcpy_bench(const char *engine_name, int is_axi, int burst_sz, uint8_t *ps, size_t psn, uint8_t *s, size_t n) {
	const int count = 32;	//32 * 32 KB = 1 MB total per direction
	async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
	cfg.backlog = count + 4;
	cfg.dma_burst_size = burst_sz;
	async_memcpy_handle_t h = NULL;
	esp_err_t err = is_axi ? esp_async_memcpy_install_gdma_axi(&cfg, &h) : esp_async_memcpy_install_gdma_ahb(&cfg, &h);
	if (err != ESP_OK || !h) {
		printf("MEMBENCH dma %-8s burst %2d B: install failed (%d)\n", engine_name, burst_sz, (int)err);
		return;
	}
	double res[4] = {0};
	for (int m = 0; m < 4; m++) {
		dma_left = count;
		size_t xfer = (m == 3) ? (n / 2) : n;
		uint32_t c0 = cycles();
		for (int i = 0; i < count; i++) {
			uint8_t *dst = (m == 0) ? (ps + i * n) : (m == 2) ? (ps + psn / 2 + i * n) : (m == 3) ? (s + n / 2) : s;
			uint8_t *src = (m == 0 || m == 3) ? s : (ps + i * n);
			if (esp_async_memcpy(h, dst, src, xfer, dma_cb, NULL) != ESP_OK) {
				dma_left--;
			}
		}
		while (dma_left > 0) ;
		uint32_t c1 = cycles();
		res[m] = mbs((double)count * xfer, c1 - c0);
	}
	printf("MEMBENCH dma %-8s burst %2d B: SRAM->PSRAM %6.0f | PSRAM->SRAM %6.0f | PSRAM->PSRAM %6.0f | SRAM->SRAM %6.0f MB/s\n",
			engine_name, burst_sz, res[0], res[1], res[2], res[3]);
	esp_async_memcpy_uninstall(h);
}

// Direct pre-mounted AXI-GDMA channel (zero per-transfer driver allocation, like bandcopy.c)
static volatile int raw_axi_done;
static bool IRAM_ATTR raw_axi_eof_cb(gdma_channel_handle_t chan, gdma_event_data_t *event_data, void *user_data) {
	raw_axi_done = 1;
	return false;
}

static void gdma_axi_direct_bench(uint8_t *ps, size_t psn, uint8_t *s, size_t n) {
	gdma_channel_alloc_config_t tx_cfg = { .flags.reserve_sibling = 1, .direction = GDMA_CHANNEL_DIRECTION_TX };
	gdma_channel_handle_t tx_ch = NULL, rx_ch = NULL;
	if (gdma_new_axi_channel(&tx_cfg, &tx_ch) != ESP_OK) return;
	gdma_channel_alloc_config_t rx_cfg = { .direction = GDMA_CHANNEL_DIRECTION_RX, .sibling_chan = tx_ch };
	if (gdma_new_axi_channel(&rx_cfg, &rx_ch) != ESP_OK) { gdma_del_channel(tx_ch); return; }

	gdma_trigger_t m2m = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_M2M, 0);
	uint32_t free_ids = 0;
	gdma_get_free_m2m_trig_id_mask(tx_ch, &free_ids);
	m2m.instance_id = __builtin_ctz(free_ids);
	gdma_connect(rx_ch, m2m);
	gdma_connect(tx_ch, m2m);

	gdma_strategy_config_t strat = { .owner_check = true, .auto_update_desc = true, .eof_till_data_popped = true };
	gdma_apply_strategy(tx_ch, &strat);
	gdma_apply_strategy(rx_ch, &strat);

	gdma_transfer_config_t xfer_cfg = { .max_data_burst_size = 64, .access_ext_mem = true };
	gdma_config_transfer(tx_ch, &xfer_cfg);
	gdma_config_transfer(rx_ch, &xfer_cfg);

	gdma_rx_event_callbacks_t cbs = { .on_recv_eof = raw_axi_eof_cb };
	gdma_register_rx_event_callbacks(rx_ch, &cbs, NULL);

	const int num_nodes = 16; // enough for 32 KB (9 nodes of 3968 B)
	gdma_link_list_config_t link_cfg = {
		.num_items = num_nodes,
		.item_alignment = 64,
		.flags = { .check_owner = true }
	};
	gdma_link_list_handle_t tx_link = NULL, rx_link = NULL;
	gdma_new_link_list(&link_cfg, &tx_link);
	gdma_new_link_list(&link_cfg, &rx_link);
	uintptr_t tx_head = gdma_link_get_head_addr(tx_link);
	uintptr_t rx_head = gdma_link_get_head_addr(rx_link);

	// Test 10 KB (16-row Quake band) and 32 KB direct transfers SRAM -> PSRAM
	const size_t sizes[2] = { 10240, 32768 };
	for (int si = 0; si < 2; si++) {
		size_t sz = sizes[si];
		int reps = 64;
		uint32_t c0 = cycles();
		for (int r = 0; r < reps; r++) {
			gdma_buffer_mount_config_t tx_m = { .buffer = s, .buffer_alignment = 64, .length = sz, .flags = { .mark_eof = true, .mark_final = true } };
			gdma_buffer_mount_config_t rx_m = { .buffer = ps + (r * 32768), .buffer_alignment = 64, .length = sz, .flags = { .mark_eof = true, .mark_final = true } };
			gdma_link_mount_buffers(tx_link, 0, &tx_m, 1, NULL);
			gdma_link_mount_buffers(rx_link, 0, &rx_m, 1, NULL);
			raw_axi_done = 0;
			gdma_start(rx_ch, rx_head);
			gdma_start(tx_ch, tx_head);
			while (!raw_axi_done) ;
		}
		uint32_t c1 = cycles();
		printf("MEMBENCH dma AXI-GDMA direct pre-allocated link (%5u B x %d, SRAM->PSRAM): %6.0f MB/s\n",
				(unsigned)sz, reps, mbs((double)sz * reps, c1 - c0));
	}

	// Test CPU + AXI-GDMA concurrency: CPU reads 1 MB cold PSRAM while AXI-GDMA streams 1 MB SRAM -> PSRAM
	const int dma_reps = 32; // 32 * 32 KB = 1 MB
	const size_t cpu_n = 1024 * 1024;
	evict(ps + 2 * 1024 * 1024, cpu_n);
	uint32_t c_conc0 = cycles();
	// Start first DMA transfer
	int d_idx = 0;
	gdma_buffer_mount_config_t tx_m = { .buffer = s, .buffer_alignment = 64, .length = 32768, .flags = { .mark_eof = true, .mark_final = true } };
	gdma_buffer_mount_config_t rx_m = { .buffer = ps, .buffer_alignment = 64, .length = 32768, .flags = { .mark_eof = true, .mark_final = true } };
	gdma_link_mount_buffers(tx_link, 0, &tx_m, 1, NULL);
	gdma_link_mount_buffers(rx_link, 0, &rx_m, 1, NULL);
	raw_axi_done = 0;
	gdma_start(rx_ch, rx_head);
	gdma_start(tx_ch, tx_head);

	// CPU streams 1 MB in 32 KB chunks while re-arming DMA whenever done
	for (int i = 0; i < dma_reps; i++) {
		rd128(ps + 2 * 1024 * 1024 + i * 32768, 32768);
		while (!raw_axi_done) ;
		d_idx++;
		if (d_idx < dma_reps) {
			rx_m.buffer = ps + d_idx * 32768;
			gdma_link_mount_buffers(tx_link, 0, &tx_m, 1, NULL);
			gdma_link_mount_buffers(rx_link, 0, &rx_m, 1, NULL);
			raw_axi_done = 0;
			gdma_start(rx_ch, rx_head);
			gdma_start(tx_ch, tx_head);
		}
	}
	uint32_t c_conc = cycles() - c_conc0;
	double cpu_mb = mbs(cpu_n, c_conc);
	double dma_mb = mbs((double)dma_reps * 32768, c_conc);
	printf("MEMBENCH CPU + AXI-GDMA concurrent PSRAM: CPU rd128 %6.0f + DMA SRAM->PSRAM %6.0f = %6.0f MB/s combined\n",
			cpu_mb, dma_mb, cpu_mb + dma_mb);

	gdma_del_link_list(tx_link);
	gdma_del_link_list(rx_link);
	gdma_disconnect(tx_ch);
	gdma_disconnect(rx_ch);
	gdma_del_channel(tx_ch);
	gdma_del_channel(rx_ch);
}

//---- DMA Engine 3: DW-GDMA (Synopsys DesignWare AXI DMAC, contiguous block mode, no descriptor lists!)

static volatile int dw_done;
static bool IRAM_ATTR dw_done_cb(dw_gdma_channel_handle_t chan, const dw_gdma_trans_done_event_data_t *edata, void *user_data) {
	dw_done = 1;
	return false;
}

static void dw_gdma_bench(uint8_t *ps, size_t psn, uint8_t *s, size_t sram_n) {
	static const int out_reqs[] = { 1, 4, 8, 16 };
	static const struct {
		dw_gdma_transfer_width_t w;
		int w_bits;
		dw_gdma_burst_items_t items;
		int items_n;
	} cfgs[] = {
		{ DW_GDMA_TRANS_WIDTH_32,  32,  DW_GDMA_BURST_ITEMS_64,  64 },
		{ DW_GDMA_TRANS_WIDTH_64,  64,  DW_GDMA_BURST_ITEMS_16,  16 },
		{ DW_GDMA_TRANS_WIDTH_64,  64,  DW_GDMA_BURST_ITEMS_64,  64 },
		{ DW_GDMA_TRANS_WIDTH_64,  64,  DW_GDMA_BURST_ITEMS_256, 256 },
	};

	// Sweep outstanding requests (1, 4, 8, 16) with 64-bit width, 64 burst items, burst_len=16
	for (int oi = 0; oi < 4; oi++) {
		int oreq = out_reqs[oi];
		dw_gdma_channel_alloc_config_t alloc_cfg = {
			.src = {
				.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS,
				.role = DW_GDMA_ROLE_MEM,
				.handshake_type = DW_GDMA_HANDSHAKE_HW,
				.num_outstanding_requests = oreq,
			},
			.dst = {
				.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS,
				.role = DW_GDMA_ROLE_MEM,
				.handshake_type = DW_GDMA_HANDSHAKE_HW,
				.num_outstanding_requests = oreq,
			},
			.flow_controller = DW_GDMA_FLOW_CTRL_SELF,
			.chan_priority = 1,
		};
		dw_gdma_channel_handle_t ch = NULL;
		if (dw_gdma_new_channel(&alloc_cfg, &ch) != ESP_OK) {
			printf("MEMBENCH dma DW-GDMA out_reqs=%d: channel alloc failed\n", oreq);
			continue;
		}
		dw_gdma_event_callbacks_t cbs = { .on_full_trans_done = dw_done_cb };
		dw_gdma_channel_register_event_callbacks(ch, &cbs, NULL);
		dw_gdma_block_markers_t markers = { .is_valid = true, .is_last = true, .en_trans_done_intr = true };

		double res[4] = {0};
		for (int m = 0; m < 4; m++) {
			// For PSRAM->PSRAM we can transfer 1 MB in a single contiguous block!
			// For SRAM transfers we transfer 32 KB x 16 = 512 KB (or 16 KB x 16 for SRAM->SRAM).
			size_t xfer = (m == 2) ? (1024 * 1024) : (m == 3) ? (sram_n / 2) : sram_n;
			int reps = (m == 2) ? 1 : 16;
			uint32_t c0 = cycles();
			for (int r = 0; r < reps; r++) {
				uint32_t src_addr = (uint32_t)((m == 0 || m == 3) ? s : (ps + r * xfer));
				uint32_t dst_addr = (uint32_t)((m == 0) ? (ps + r * xfer) : (m == 2) ? (ps + psn / 2) : (m == 3) ? (s + xfer) : s);
				dw_gdma_block_transfer_config_t tcfg = {
					.src = { .addr = src_addr, .burst_mode = DW_GDMA_BURST_MODE_INCREMENT, .burst_items = DW_GDMA_BURST_ITEMS_64, .burst_len = 16, .width = DW_GDMA_TRANS_WIDTH_64 },
					.dst = { .addr = dst_addr, .burst_mode = DW_GDMA_BURST_MODE_INCREMENT, .burst_items = DW_GDMA_BURST_ITEMS_64, .burst_len = 16, .width = DW_GDMA_TRANS_WIDTH_64 },
					.size = xfer / 8,
				};
				dw_done = 0;
				dw_gdma_channel_config_transfer(ch, &tcfg);
				dw_gdma_channel_set_block_markers(ch, markers);
				dw_gdma_channel_enable_ctrl(ch, true);
				while (!dw_done) ;
			}
			uint32_t c1 = cycles();
			res[m] = mbs((double)xfer * reps, c1 - c0);
		}
		printf("MEMBENCH dma DW-GDMA out_req %2d (64b, item 64): SRAM->PSRAM %6.0f | PSRAM->SRAM %6.0f | PSRAM->PSRAM %6.0f | SRAM->SRAM %6.0f MB/s\n",
				oreq, res[0], res[1], res[2], res[3]);

		// On out_req == 16, also test burst_items / width variations and hardware fixed-source memset!
		if (oreq == 16) {
			for (int ci = 0; ci < 4; ci++) {
				size_t xfer = 512 * 1024;
				size_t item_bytes = cfgs[ci].w_bits / 8;
				ps[psn / 2 + xfer - 1] = 0;
				writeback(ps + psn / 2 + xfer - 128, 128);
				dw_gdma_block_transfer_config_t tcfg = {
					.src = { .addr = (uint32_t)ps, .burst_mode = DW_GDMA_BURST_MODE_INCREMENT, .burst_items = cfgs[ci].items, .burst_len = 16, .width = cfgs[ci].w },
					.dst = { .addr = (uint32_t)(ps + psn / 2), .burst_mode = DW_GDMA_BURST_MODE_INCREMENT, .burst_items = cfgs[ci].items, .burst_len = 16, .width = cfgs[ci].w },
					.size = xfer / item_bytes,
				};
				dw_done = 0;
				uint32_t c0 = cycles();
				dw_gdma_channel_config_transfer(ch, &tcfg);
				dw_gdma_channel_set_block_markers(ch, markers);
				dw_gdma_channel_enable_ctrl(ch, true);
				while (!dw_done) ;
				uint32_t c1 = cycles();
				evict(ps + psn / 2 + xfer - 128, 128);
				int full_ok = (ps[psn / 2 + xfer - 1] == ps[xfer - 1]);
				printf("MEMBENCH dma DW-GDMA width %3d-bit, burst_items %3d (PSRAM->PSRAM 512 KB): %6.0f MB/s (%s)\n",
						cfgs[ci].w_bits, cfgs[ci].items_n, mbs(xfer, c1 - c0), full_ok ? "verified" : "clamped to 64b");
			}

			// Hardware fixed-source fill (memset) -> PSRAM (1 MB) and -> SRAM (32 KB x 16)
			dw_gdma_block_transfer_config_t fill_ps = {
				.src = { .addr = (uint32_t)s, .burst_mode = DW_GDMA_BURST_MODE_FIXED, .burst_items = DW_GDMA_BURST_ITEMS_64, .burst_len = 16, .width = DW_GDMA_TRANS_WIDTH_64 },
				.dst = { .addr = (uint32_t)ps, .burst_mode = DW_GDMA_BURST_MODE_INCREMENT, .burst_items = DW_GDMA_BURST_ITEMS_64, .burst_len = 16, .width = DW_GDMA_TRANS_WIDTH_64 },
				.size = (1024 * 1024) / 8,
			};
			dw_done = 0;
			uint32_t cf0 = cycles();
			dw_gdma_channel_config_transfer(ch, &fill_ps);
			dw_gdma_channel_set_block_markers(ch, markers);
			dw_gdma_channel_enable_ctrl(ch, true);
			while (!dw_done) ;
			uint32_t cf1 = cycles();

			uint32_t cfs0 = cycles();
			for (int r = 0; r < 16; r++) {
				dw_gdma_block_transfer_config_t fill_sr = fill_ps;
				fill_sr.dst.addr = (uint32_t)s;
				fill_sr.size = sram_n / 8;
				dw_done = 0;
				dw_gdma_channel_config_transfer(ch, &fill_sr);
				dw_gdma_channel_set_block_markers(ch, markers);
				dw_gdma_channel_enable_ctrl(ch, true);
				while (!dw_done) ;
			}
			uint32_t cfs1 = cycles();
			printf("MEMBENCH dma DW-GDMA fixed-src fill (memset): -> PSRAM (1 MB) %6.0f MB/s | -> SRAM (32 KB) %6.0f MB/s\n",
					mbs(1024 * 1024, cf1 - cf0), mbs((double)sram_n * 16, cfs1 - cfs0));
		}

		dw_gdma_del_channel(ch);
	}
}

//---- DMA Engine 4: 2D-DMA (DMA2D) + PPA (Fill, SRM copy/convert, and CLUT8 Blend)

static void ppa_dma2d_bench(uint8_t *ps, size_t psn, uint8_t *s, size_t sram_n) {
	// 1. PPA Fill across burst lengths (16, 64, 128 B) and pixel formats (RGB565 2B, RGB888 3B, ARGB8888 4B)
	static const ppa_data_burst_length_t bursts[] = {
		PPA_DATA_BURST_LENGTH_16, PPA_DATA_BURST_LENGTH_64, PPA_DATA_BURST_LENGTH_128
	};
	ppa_client_handle_t fill_client = NULL;
	ppa_client_config_t fill_cli_cfg = { .oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1, .data_burst_length = PPA_DATA_BURST_LENGTH_128 };
	if (ppa_register_client(&fill_cli_cfg, &fill_client) == ESP_OK) {
		static const struct { ppa_fill_color_mode_t fmt; const char *name; int bpp; } fmts[] = {
			{ PPA_FILL_COLOR_MODE_RGB565,   "RGB565 (2B/px)",   2 },
			{ PPA_FILL_COLOR_MODE_RGB888,   "RGB888 (3B/px)",   3 },
			{ PPA_FILL_COLOR_MODE_ARGB8888, "ARGB8888 (4B/px)", 4 },
		};
		for (int bi = 0; bi < 3; bi++) {
			ppa_unregister_client(fill_client);
			fill_cli_cfg.data_burst_length = bursts[bi];
			ppa_register_client(&fill_cli_cfg, &fill_client);
			for (int fi = 0; fi < 3; fi++) {
				if (bi < 2 && fi != 2) continue; // full format sweep at burst 128 B, ARGB8888 across all burst sizes
				const int w = 640, h = 480;
				size_t bytes = (size_t)w * h * fmts[fi].bpp;
				ppa_fill_oper_config_t fcfg = {
					.out = { .buffer = ps, .buffer_size = (bytes + 127) & ~127u, .pic_w = w, .pic_h = h, .fill_cm = fmts[fi].fmt },
					.fill_block_w = w, .fill_block_h = h,
					.fill_argb_color = { .val = 0xFF112233 },
					.mode = PPA_TRANS_MODE_BLOCKING,
				};
				uint32_t c0 = cycles();
				esp_err_t err = ppa_do_fill(fill_client, &fcfg);
				uint32_t c1 = cycles();
				if (err == ESP_OK) {
					printf("MEMBENCH dma PPA-Fill (2D-DMA burst %3d B) %-17s -> PSRAM (%4u KB): %6.0f MB/s\n",
							bursts[bi], fmts[fi].name, (unsigned)(bytes/1024), mbs(bytes, c1 - c0));
				}
			}
		}
		// PPA Fill into internal SRAM (128x128 RGB565 = 32 KB)
		const int sw = 128, sh = 128;
		size_t sbytes = sw * sh * 2;
		ppa_fill_oper_config_t sfcfg = {
			.out = { .buffer = s, .buffer_size = sram_n, .pic_w = sw, .pic_h = sh, .fill_cm = PPA_FILL_COLOR_MODE_RGB565 },
			.fill_block_w = sw, .fill_block_h = sh,
			.fill_argb_color = { .val = 0xFF112233 },
			.mode = PPA_TRANS_MODE_BLOCKING,
		};
		uint32_t cs0 = cycles();
		for (int r = 0; r < 16; r++) ppa_do_fill(fill_client, &sfcfg);
		uint32_t cs1 = cycles();
		printf("MEMBENCH dma PPA-Fill (2D-DMA burst 128 B) RGB565 (2B/px)    -> SRAM  (%4u KB): %6.0f MB/s\n",
				(unsigned)(sbytes/1024), mbs((double)sbytes * 16, cs1 - cs0));
		ppa_unregister_client(fill_client);
	}

	// 2. PPA SRM (Scale-Rotate-Mirror 1:1 copy & format conversion)
	ppa_client_handle_t srm_client = NULL;
	ppa_client_config_t srm_cli_cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1, .data_burst_length = PPA_DATA_BURST_LENGTH_128 };
	if (ppa_register_client(&srm_cli_cfg, &srm_client) == ESP_OK) {
		const int w = 640, h = 480;
		static const struct {
			ppa_srm_color_mode_t in_cm, out_cm;
			int in_bpp, out_bpp;
			const char *name;
		} srm_modes[] = {
			{ PPA_SRM_COLOR_MODE_RGB565,   PPA_SRM_COLOR_MODE_RGB565,   2, 2, "RGB565 -> RGB565" },
			{ PPA_SRM_COLOR_MODE_RGB565,   PPA_SRM_COLOR_MODE_RGB888,   2, 3, "RGB565 -> RGB888" },
			{ PPA_SRM_COLOR_MODE_RGB888,   PPA_SRM_COLOR_MODE_RGB888,   3, 3, "RGB888 -> RGB888" },
			{ PPA_SRM_COLOR_MODE_ARGB8888, PPA_SRM_COLOR_MODE_ARGB8888, 4, 4, "ARGB8888 -> ARGB8888" },
		};
		for (int mi = 0; mi < 4; mi++) {
			size_t in_bytes = (size_t)w * h * srm_modes[mi].in_bpp;
			size_t out_bytes = (size_t)w * h * srm_modes[mi].out_bpp;
			ppa_srm_oper_config_t scfg = {
				.in = { .buffer = ps, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h, .srm_cm = srm_modes[mi].in_cm },
				.out = { .buffer = ps + psn / 2, .buffer_size = (out_bytes + 127) & ~127u, .pic_w = w, .pic_h = h, .srm_cm = srm_modes[mi].out_cm },
				.rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
				.scale_x = 1.0f, .scale_y = 1.0f,
				.mode = PPA_TRANS_MODE_BLOCKING,
			};
			uint32_t c0 = cycles();
			esp_err_t err = ppa_do_scale_rotate_mirror(srm_client, &scfg);
			uint32_t c1 = cycles();
			if (err == ESP_OK) {
				printf("MEMBENCH dma PPA-SRM  (2D-DMA burst 128 B) %-20s (PSRAM->PSRAM): out %6.0f MB/s (rd+wr %6.0f MB/s)\n",
						srm_modes[mi].name, mbs(out_bytes, c1 - c0), mbs(in_bytes + out_bytes, c1 - c0));
			}
		}
		ppa_unregister_client(srm_client);
	}

	// 3. PPA Blend: L8 (CLUT8, 1B/px) + A4 (0.5B/px) -> RGB888 (3B/px) and -> RGB565 (2B/px)
	ppa_client_handle_t blend_client = NULL;
	ppa_client_config_t blend_cli_cfg = { .oper_type = PPA_OPERATION_BLEND, .max_pending_trans_num = 1, .data_burst_length = PPA_DATA_BURST_LENGTH_128 };
	if (ppa_register_client(&blend_cli_cfg, &blend_client) == ESP_OK) {
		static uint32_t clut[256];
		for (int i = 0; i < 256; i++) clut[i] = 0xFF000000u | (i * 0x010101u);
		ppa_blend_set_bg_clut(clut);
		const int w = 640, h = 480; // Quake / HDMI resolution (307,200 px)
		size_t bg_bytes = (size_t)w * h;         // 300 KB L8
		size_t fg_bytes = (size_t)w * h / 2;     // 150 KB A4

		static const struct { ppa_blend_color_mode_t out_cm; int out_bpp; const char *name; } bmodes[] = {
			{ PPA_BLEND_COLOR_MODE_RGB888, 3, "L8+A4 -> RGB888 (640x480)" },
			{ PPA_BLEND_COLOR_MODE_RGB565, 2, "L8+A4 -> RGB565 (640x480)" },
		};
		for (int bi = 0; bi < 2; bi++) {
			size_t out_bytes = (size_t)w * h * bmodes[bi].out_bpp;
			ppa_blend_oper_config_t bcfg = {
				.in_bg = { .buffer = ps, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h, .blend_cm = PPA_BLEND_COLOR_MODE_BG_L8 },
				.in_fg = { .buffer = ps, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h, .blend_cm = PPA_BLEND_COLOR_MODE_A4 },
				.out = { .buffer = ps + psn / 2, .buffer_size = (out_bytes + 127) & ~127u, .pic_w = w, .pic_h = h, .blend_cm = bmodes[bi].out_cm },
				.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE, .bg_alpha_fix_val = 255,
				.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE, .fg_alpha_fix_val = 0,
				.mode = PPA_TRANS_MODE_BLOCKING,
			};
			uint32_t c0 = cycles();
			esp_err_t err = ppa_do_blend(blend_client, &bcfg);
			uint32_t c1 = cycles();
			if (err == ESP_OK) {
				double us = (c1 - c0) / mhz;
				printf("MEMBENCH dma PPA-Blend(2D-DMA burst 128 B) %-28s: %5.0f us | out %6.0f MB/s | total rd+wr %6.0f MB/s\n",
						bmodes[bi].name, us, mbs(out_bytes, c1 - c0), mbs(bg_bytes + fg_bytes + out_bytes, c1 - c0));
			}
		}
		ppa_unregister_client(blend_client);
	}
}

static void dma_bench(uint8_t *ps, size_t psn) {
	const size_t n = 32 * 1024;
	uint8_t *s = heap_caps_aligned_alloc(128, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
	if (!s) { printf("MEMBENCH dma: no SRAM DMA buffer\n"); return; }
	memset(s, 0xa5, n);
	writeback(s, n);

	// 1. AXI-GDMA via esp_async_memcpy across burst sizes 16, 32, 64 B
	static const int axi_bursts[] = { 16, 32, 64 };
	for (int i = 0; i < 3; i++)
		gdma_async_memcpy_bench("AXI-GDMA", 1, axi_bursts[i], ps, psn, s, n);

	// 2. Direct pre-mounted AXI-GDMA descriptor links + CPU/DMA concurrency
	gdma_axi_direct_bench(ps, psn, s, n);

	// 3. AHB-GDMA via esp_async_memcpy across burst sizes 4, 8, 16, 32 B
	static const int ahb_bursts[] = { 4, 8, 16, 32 };
	for (int i = 0; i < 4; i++)
		gdma_async_memcpy_bench("AHB-GDMA", 0, ahb_bursts[i], ps, psn, s, n);

	// 4. DW-GDMA (Synopsys AXI DMAC) contiguous block transfers + fixed-src hardware fill
	dw_gdma_bench(ps, psn, s, n);

	// 5. 2D-DMA (DMA2D) + PPA (Fill, SRM, Blend)
	ppa_dma2d_bench(ps, psn, s, n);

	free(s);
}

static SPM_DRAM_ATTR uint8_t tcm_buf[4096] __attribute__((aligned(128)));
static RTC_FAST_ATTR uint8_t lpram_buf[4096] __attribute__((aligned(128)));

void membench_run(const char *when) {
	//check the clock the cycle counter runs at
	int64_t t0=esp_timer_get_time();
	uint32_t c0=cycles();
	while (esp_timer_get_time()-t0<20000) ;
	mhz=(cycles()-c0)/(double)(esp_timer_get_time()-t0);
	printf("MEMBENCH ==== %s (core %d, mcycle runs at %.1f MHz) ====\n", when, xPortGetCoreID(), mhz);
	printf("MEMBENCH config: L1D autoload 0x%08lx, L2 autoload 0x%08lx, L1_WRAP 0x%02lx, L2_WRAP 0x%02lx, PSRAM clk 0x%08lx, L2 %d KB/%d B lines\n",
			(unsigned long)REG_READ(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG),
			(unsigned long)REG_READ(CACHE_L2_CACHE_AUTOLOAD_CTRL_REG),
			(unsigned long)REG_READ(CACHE_L1_CACHE_WRAP_AROUND_CTRL_REG),
			(unsigned long)REG_READ(CACHE_L2_CACHE_WRAP_AROUND_CTRL_REG),
			(unsigned long)REG_READ(SPI_MEM_S_SRAM_CLK_REG),
			CONFIG_CACHE_L2_CACHE_SIZE/1024, CONFIG_CACHE_L2_CACHE_LINE_SIZE);

	// 1. Zero-wait-state TCM (0x3010_0000) and LP SRAM (0x5010_8000)
	region("TCM (0x3010_0000)", tcm_buf, sizeof(tcm_buf), 0);
	region("LP SRAM (0x5010_8000)", lpram_buf, sizeof(lpram_buf), 0);

	// 2. Internal L2MEM SRAM: cached (0x4FF0_0000 in L1, at 32 KB L1 edge, and 96 KB beyond L1) vs non-cached alias (0x8FF0_0000)
	uint8_t *b=heap_caps_aligned_alloc(128, 96*1024, MALLOC_CAP_INTERNAL);
	if (b) {
		memset(b, 0x3c, 96*1024);
		writeback(b, 96*1024);
		region("SRAM in L1 (16 KB)", b, 16*1024, 0);
		region("SRAM L1 edge (32 KB)", b, 32*1024, 0);
		region("SRAM beyond L1 (96 KB)", b, 96*1024, 0);
		uint8_t *b_nc = (uint8_t *)CACHE_LL_L2MEM_NON_CACHE_ADDR(b);
		region("SRAM uncached (0x8FFx)", b_nc, 16*1024, 0);
		free(b);
	}

	// 3. External PSRAM: in L1 (16 KB), at L1 edge (32 KB), in L2 (192 KB), and cold misses (4 MB)
	const size_t big=4*1024*1024;
	uint8_t *p=heap_caps_aligned_alloc(128, 2*big, MALLOC_CAP_SPIRAM);
	if (!p) { printf("MEMBENCH no PSRAM buffer\n"); return; }
	memset(p, 0x5a, 2*big);
	writeback(p, 2*big);
	region("PSRAM in L1 (16 KB)", p, 16*1024, 0);
	region("PSRAM L1 edge (32 KB)", p, 32*1024, 0);
	region("PSRAM in L2 (192 KB)", p, 192*1024, 0);
	region("PSRAM cold (4 MB)", p, big, 1);

	// 4. Cold write-allocate vs L2-warm writeback with hardware L1/L2 cache counters
	write_allocate_bench(p);

	// 5. Hardware autoload, software prefetch, cache wrap-around, and manual L1/L2 preload
	prefetch_bench(p, big);

	// 6. Dual-core PSRAM streaming
	dual_bench(p, p+big, big);

	// 7. All 4 DMA engines (AXI-GDMA, AHB-GDMA, DW-GDMA, 2D-DMA + PPA)
	dma_bench(p, big);

	free(p);
	printf("MEMBENCH ==== %s DONE ====\n", when);
}

#else
void membench_run(const char *when) {}
#endif
