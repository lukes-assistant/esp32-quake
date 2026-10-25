// 8-bit palettised frame -> YUV422 display frame, without the CPU touching pixels.
//
// The HDMI path's DSI bridge can't convert RGB565 on this chip revision, but it converts
// YUV422 to RGB888 (verified on screen). A YUV422 display buffer is 2 bytes per pixel instead
// of 3: a third less scan-out and conversion traffic in PSRAM. The PPA can't write YUV on this
// revision, so each 16-row chunk is converted in two hardware steps:
//   PPA blend:        L8 (PSRAM, through the palette CLUT) -> RGB888 chunk in internal SRAM
//   2D-DMA M2M + CSC: RGB888 chunk -> YUV422 rows of the display buffer (PSRAM)
// The two chunk buffers let the PPA fill one while the 2D-DMA empties the other.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_private/dma2d.h"
#include "hal/dma2d_types.h"
#include "hal/color_types.h"
#include "soc/dma2d_channel.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "yuvconv.h"

#define W			YUVCONV_W
#define H			YUVCONV_H
#define CHUNK_ROWS	32
#define NCHUNKS		(H/CHUNK_ROWS)

static uint8_t chunkbuf[1][W*CHUNK_ROWS*3] __attribute__((aligned(128)));	//the PPA wants 128-byte (PSRAM line) alignment
static dma2d_descriptor_t tx_desc[2] __attribute__((aligned(64)));
static dma2d_descriptor_t rx_desc[2] __attribute__((aligned(64)));
static dma2d_pool_handle_t pool;
static SemaphoreHandle_t done_sem[2];

typedef struct {
	dma2d_trans_config_t chan_desc;
	int slot;
	uint8_t placeholder[0];	//followed by SIZEOF_DMA2D_TRANS_T bytes
} m2m_trans_t;
static m2m_trans_t *trans[2];

static dma2d_transfer_ability_t ability={
	.access_ext_mem=true,
	.data_burst_length=128,
	.desc_burst_en=true,
	.mb_size=DMA2D_MACRO_BLOCK_SIZE_NONE,
};
static dma2d_csc_config_t csc={
	.tx_csc_option=DMA2D_CSC_TX_RGB888_TO_YUV422_601,
	.pre_scramble=DMA2D_SCRAMBLE_ORDER_BYTE2_1_0,	//the PPA writes B,G,R
};

static IRAM_ATTR bool m2m_done(dma2d_channel_handle_t chan, dma2d_event_data_t *e, void *arg) {
	BaseType_t woken=pdFALSE;
	xSemaphoreGiveFromISR(done_sem[(int)arg], &woken);
	return woken==pdTRUE;
}

static IRAM_ATTR bool m2m_picked(uint32_t n, const dma2d_trans_channel_info_t *chans, void *user) {
	m2m_trans_t *t=(m2m_trans_t *)user;
	int txi=chans[0].dir==DMA2D_CHANNEL_DIRECTION_RX ? 1 : 0;
	dma2d_channel_handle_t tx=chans[txi].chan, rx=chans[1-txi].chan;
	dma2d_trigger_t trig={ .periph=DMA2D_TRIG_PERIPH_M2M, .periph_sel_id=SOC_DMA2D_TRIG_PERIPH_M2M_TX };
	dma2d_connect(tx, &trig);
	trig.periph_sel_id=SOC_DMA2D_TRIG_PERIPH_M2M_RX;
	dma2d_connect(rx, &trig);
	dma2d_set_transfer_ability(tx, &ability);
	dma2d_set_transfer_ability(rx, &ability);
	dma2d_configure_color_space_conversion(tx, &csc);
	dma2d_rx_event_callbacks_t cbs={ .on_recv_eof=m2m_done };
	dma2d_register_rx_event_callbacks(rx, &cbs, (void *)t->slot);
	dma2d_set_desc_addr(tx, (intptr_t)&tx_desc[t->slot]);
	dma2d_set_desc_addr(rx, (intptr_t)&rx_desc[t->slot]);
	dma2d_start(tx);
	dma2d_start(rx);
	return false;
}

static void desc_2d(dma2d_descriptor_t *d, void *buf, int rows, int pbyte, int eof) {
	memset(d, 0, sizeof(*d));
	d->owner=DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
	d->suc_eof=eof;
	d->dma2d_en=1;
	d->ha_length=W; d->hb_length=W;
	d->va_size=rows; d->vb_size=rows;
	d->pbyte=pbyte;
	d->mode=DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
	d->buffer=buf;
	d->next=NULL;
}

//start converting chunk buffer `slot` (rows of RGB888) into the YUV422 rows at dst
static esp_err_t m2m_start(int slot, uint8_t *dst, int rows, int cpu_touched) {
	const color_space_pixel_format_t rgb={ .color_space=COLOR_SPACE_RGB, .pixel_format=COLOR_PIXEL_RGB888 };
	const color_space_pixel_format_t yuv={ .color_space=COLOR_SPACE_YUV, .pixel_format=COLOR_PIXEL_YUV422 };
	// in normal frames only the PPA and the 2D-DMA touch the chunk and the display rows: no
	// cache maintenance (the start-up test writes the chunk with the CPU)
	if (cpu_touched) {
		esp_cache_msync(chunkbuf[slot], W*rows*3, ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_UNALIGNED);
		esp_cache_msync(dst, W*rows*2, ESP_CACHE_MSYNC_FLAG_DIR_M2C);	//(rows are line-aligned)
	}
	desc_2d(&tx_desc[slot], chunkbuf[slot], rows, dma2d_desc_pixel_format_to_pbyte_value(rgb), 1);
	desc_2d(&rx_desc[slot], dst, rows, dma2d_desc_pixel_format_to_pbyte_value(yuv), 0);
	esp_cache_msync(&tx_desc[slot], sizeof(dma2d_descriptor_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_UNALIGNED);
	esp_cache_msync(&rx_desc[slot], sizeof(dma2d_descriptor_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_UNALIGNED);
	m2m_trans_t *t=trans[slot];
	memset(&t->chan_desc, 0, sizeof(t->chan_desc));
	t->slot=slot;
	t->chan_desc.tx_channel_num=1;
	t->chan_desc.rx_channel_num=1;
	t->chan_desc.channel_flags=DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING|DMA2D_CHANNEL_FUNCTION_FLAG_TX_CSC;
	t->chan_desc.user_config=t;
	t->chan_desc.on_job_picked=m2m_picked;
	return dma2d_enqueue(pool, &t->chan_desc, (dma2d_trans_t *)t->placeholder);
}

int yuvconv_init(void) {
	dma2d_pool_config_t pc={ .pool_id=0 };
	if (dma2d_acquire_pool(&pc, &pool)!=ESP_OK) return 0;
	for (int i=0; i<2; i++) {
		done_sem[i]=xSemaphoreCreateBinary();
		trans[i]=heap_caps_calloc(1, sizeof(m2m_trans_t)+SIZEOF_DMA2D_TRANS_T, MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
		if (!done_sem[i] || !trans[i]) return 0;
	}
	return 1;
}

//convert one RGB888 chunk already in chunkbuf[0] (test helper)
int yuvconv_test_rgb(const uint8_t rgb[][3], int n, uint8_t *out_yuv, uint8_t *dst_psram) {
	for (int i=0; i<W*CHUNK_ROWS; i++) {	//(the test fills the whole chunk)
		const uint8_t *c=rgb[(i%W)*n/W];
		chunkbuf[0][i*3+0]=c[2]; chunkbuf[0][i*3+1]=c[1]; chunkbuf[0][i*3+2]=c[0];	//B,G,R as the PPA writes
	}
	if (m2m_start(0, dst_psram, CHUNK_ROWS, 1)!=ESP_OK) return 0;
	if (xSemaphoreTake(done_sem[0], pdMS_TO_TICKS(100))!=pdTRUE) return 0;
	memcpy(out_yuv, dst_psram, W*2);
	return 1;
}

int yuvconv_frame(const uint8_t *l8, uint8_t *yuv, yuvconv_chunk_fn ppa_chunk) {
	//one chunk buffer, steps in sequence: the PPA and the 2D-DMA copy compete for the same
	//2D-DMA channels anyway, and each operation has a fixed CPU cost, so fewer, larger chunks
	for (int c=0; c<NCHUNKS; c++) {
		if (!ppa_chunk(l8, c*CHUNK_ROWS, CHUNK_ROWS, chunkbuf[0])) { printf("YUVDBG chunk %d: ppa failed\n", c); return 0; }
		esp_err_t e=m2m_start(0, yuv+c*CHUNK_ROWS*W*2, CHUNK_ROWS, 0);
		if (e!=ESP_OK) { printf("YUVDBG chunk %d: m2m enqueue %s\n", c, esp_err_to_name(e)); return 0; }
		if (xSemaphoreTake(done_sem[0], pdMS_TO_TICKS(100))!=pdTRUE) { printf("YUVDBG chunk %d: m2m timeout\n", c); return 0; }
	}
	return 1;
}
