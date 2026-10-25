// Memory hierarchy micro-benchmark (CONFIG_QUAKE_MEMBENCH): latency and bandwidth of the
// TCM, internal SRAM and PSRAM, as seen from one HP core at the configured clock, plus GDMA
// copy throughput. Results are printed as "MEMBENCH" lines; see MEMORY_AND_COMPUTE_BUDGET.md.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "sdkconfig.h"
#include "quakegeneric.h"

#if CONFIG_QUAKE_MEMBENCH

#define LINE 128	//L2 line; also >= the 64-byte L1 line

static inline uint32_t cycles(void) { uint32_t v; __asm__ volatile ("csrr %0, mcycle" : "=r"(v)); return v; }

static uint32_t rng=12345;
static uint32_t rnd(void) { rng=rng*1103515245u+12345u; return rng>>8; }

//build a random cyclic chain through `n` lines spread over the given blocks, then chase it
static void latency(const char *what, uint8_t **blk, int nblk, size_t blksize) {
	int per=blksize/LINE, n=per*nblk;
	int *order=malloc(n*sizeof(int));
	if (!order) return;
	for (int i=0; i<n; i++) order[i]=i;
	for (int i=n-1; i>0; i--) { int j=rnd()%(i+1); int t=order[i]; order[i]=order[j]; order[j]=t; }
	#define NODE(k) ((void **)(blk[(k)/per]+((k)%per)*LINE))
	for (int i=0; i<n; i++) *NODE(order[i])=NODE(order[(i+1)%n]);
	for (int b=0; b<nblk; b++) esp_cache_msync(blk[b], blksize, ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_UNALIGNED);
	void **p=NODE(order[0]);
	for (int i=0; i<n; i++) p=(void **)*p;	//warm up
	int steps=n<4096 ? 4096*8 : n*2;
	uint32_t c0=cycles();
	for (int i=0; i<steps; i++) p=(void **)*p;
	uint32_t c1=cycles();
	printf("MEMBENCH latency %-34s %8u bytes: %6.1f cycles = %6.1f ns per dependent load (%p)\n",
			what, (unsigned)(blksize*nblk), (double)(c1-c0)/steps, (double)(c1-c0)/steps*1000.0/CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, p);
	free(order);
	#undef NODE
}

static void bandwidth(const char *what, uint8_t **blk, int nblk, size_t blksize) {
	const int reps=blksize*nblk>=1<<20 ? 2 : 16;
	uint32_t sum=0;
	//read: 8 independent 32-bit loads per iteration
	uint32_t c0=cycles();
	for (int r=0; r<reps; r++)
		for (int b=0; b<nblk; b++) {
			const uint32_t *q=(const uint32_t *)blk[b], *e=q+blksize/4;
			for (; q<e; q+=8) sum+=q[0]+q[1]+q[2]+q[3]+q[4]+q[5]+q[6]+q[7];
		}
	uint32_t c1=cycles();
	double bytes=(double)blksize*nblk*reps;
	double rd=bytes/((c1-c0)/(double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);	//bytes per us = MB/s
	//write: 8 32-bit stores per iteration
	c0=cycles();
	for (int r=0; r<reps; r++)
		for (int b=0; b<nblk; b++) {
			uint32_t *q=(uint32_t *)blk[b], *e=q+blksize/4;
			for (; q<e; q+=8) { q[0]=r; q[1]=r; q[2]=r; q[3]=r; q[4]=r; q[5]=r; q[6]=r; q[7]=r; }
		}
	for (int b=0; b<nblk; b++) esp_cache_msync(blk[b], blksize, ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_UNALIGNED);
	c1=cycles();
	double wr=bytes/((c1-c0)/(double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
	//memcpy within the region (first half -> second half of each block)
	c0=cycles();
	for (int r=0; r<reps; r++)
		for (int b=0; b<nblk; b++) memcpy(blk[b]+blksize/2, blk[b], blksize/2);
	c1=cycles();
	double cp=bytes/2/((c1-c0)/(double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
	printf("MEMBENCH bandwidth %-32s %8u bytes: read %7.1f MB/s, write %7.1f MB/s, memcpy %7.1f MB/s (%u)\n",
			what, (unsigned)(blksize*nblk), rd, wr, cp, (unsigned)sum);
}

static void region(const char *what, uint8_t **blk, int nblk, size_t blksize) {
	latency(what, blk, nblk, blksize);
	bandwidth(what, blk, nblk, blksize);
}

static void dma_bench(const char *what) {
	//GDMA (AXI) SRAM -> PSRAM, as the band copy-out (main/bandcopy.c): 10 KB transfers
	const size_t n=16*640, count=200;
	uint8_t *src=heap_caps_aligned_alloc(128, n, MALLOC_CAP_INTERNAL);
	uint8_t *dst=heap_caps_aligned_alloc(128, n*count, MALLOC_CAP_SPIRAM);
	if (!src || !dst) { free(src); free(dst); return; }
	memset(src, 0x55, n);
	uint32_t c0=cycles();
	for (size_t i=0; i<count; i++) QG_CopyWait(QG_CopyStart(dst+i*n, src, n));
	uint32_t c1=cycles();
	printf("MEMBENCH dma %-38s GDMA SRAM->PSRAM 10 KB transfers: %7.1f MB/s\n", what,
			(double)n*count/((c1-c0)/(double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ));
	free(src); free(dst);
}

static SPM_DRAM_ATTR uint8_t tcm_buf[4096] __attribute__((aligned(128)));

void membench_run(const char *when) {
	printf("MEMBENCH ---- %s (core %d, %d MHz)\n", when, xPortGetCoreID(), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
	uint8_t *b[8]={0};
	//TCM
	b[0]=tcm_buf;
	region("TCM (8 KB SPM)", b, 1, sizeof(tcm_buf));
	//internal SRAM, fits in the 64 KB L1 D-cache
	b[0]=heap_caps_aligned_alloc(128, 16384, MALLOC_CAP_INTERNAL);
	if (b[0]) { region("internal SRAM, 16 KB (L1 hits)", b, 1, 16384); free(b[0]); }
	//internal SRAM, more than L1 (several blocks)
	int nb=0;
	for (; nb<6; nb++) if (!(b[nb]=heap_caps_aligned_alloc(128, 16384, MALLOC_CAP_INTERNAL))) break;
	if (nb>=5) region("internal SRAM, 80-96 KB (> L1)", b, nb, 16384);
	for (int i=0; i<nb; i++) free(b[i]);
	//PSRAM within the 256 KB L2
	b[0]=heap_caps_aligned_alloc(128, 32768, MALLOC_CAP_SPIRAM);
	if (b[0]) { region("PSRAM, 32 KB (L1 hits)", b, 1, 32768); free(b[0]); }
	b[0]=heap_caps_aligned_alloc(128, 192*1024, MALLOC_CAP_SPIRAM);
	if (b[0]) { region("PSRAM, 192 KB (L2 hits)", b, 1, 192*1024); free(b[0]); }
	//PSRAM far beyond the caches
	b[0]=heap_caps_aligned_alloc(128, 4*1024*1024, MALLOC_CAP_SPIRAM);
	if (b[0]) { region("PSRAM, 4 MB (misses)", b, 1, 4*1024*1024); free(b[0]); }
	dma_bench(when);
}

#else
void membench_run(const char *when) {}
#endif
