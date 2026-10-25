// Asynchronous SRAM -> PSRAM copies for the renderer's frame-buffer bands (r_edge.c).
//
// The world is drawn into small internal-RAM band buffers; each finished band is copied to
// the PSRAM frame buffer by a DW-GDMA channel in contiguous block mode (557 MB/s vs 154 MB/s
// for AXI-GDMA, and no descriptor link lists to mount), so the cores never take write-allocate
// cache misses on the frame buffer. One copy at a time (the renderer waits for the previous
// one before starting the next). The destination rows are invalidated in the cache first:
// they hold no data the CPU still needs (the frame buffer was written back before the display
// read it).

#include <stdio.h>
#include <string.h>
#include "esp_private/dw_gdma.h"
#include "esp_cache.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "quakegeneric.h"
#include "esp_timer.h"

#define MAX_COPY	(16*640)	// a band (r_edge.c)

static dw_gdma_channel_handle_t dw_chan;
static volatile uint32_t copies_done;
static uint32_t copies_started;

static IRAM_ATTR bool copy_done_isr(dw_gdma_channel_handle_t chan, const dw_gdma_trans_done_event_data_t *e, void *arg) {
	copies_done++;
	return false;
}

int QG_CopyInit(void) {
	if (dw_chan) return 1;	//already done
	dw_gdma_channel_alloc_config_t alloc_cfg={
		.src={
			.block_transfer_type=DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS,
			.role=DW_GDMA_ROLE_MEM,
			.handshake_type=DW_GDMA_HANDSHAKE_HW,
			.num_outstanding_requests=8,
		},
		.dst={
			.block_transfer_type=DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS,
			.role=DW_GDMA_ROLE_MEM,
			.handshake_type=DW_GDMA_HANDSHAKE_HW,
			.num_outstanding_requests=8,
		},
		.flow_controller=DW_GDMA_FLOW_CTRL_SELF,
		.chan_priority=1,
	};
	if (dw_gdma_new_channel(&alloc_cfg, &dw_chan)!=ESP_OK) goto fail;
	dw_gdma_event_callbacks_t cbs={ .on_full_trans_done=copy_done_isr };
	if (dw_gdma_channel_register_event_callbacks(dw_chan, &cbs, NULL)!=ESP_OK) goto fail;
	printf("band copy: DW-GDMA contiguous (64-bit, burst_items=64, out_req=8)\n");
	return 1;
fail:
	printf("band copy: DW-GDMA setup failed, drawing straight into the frame buffer\n");
	return 0;
}

//returns a ticket for QG_CopyWait, or 0 if the copy was done synchronously
uint32_t QG_CopyStart(void *dst, void *src, size_t n) {
	if (!dw_chan || n>MAX_COPY || ((uint32_t)dst & 127) || (n & 127)) {
		memcpy(dst, src, n);
		return 0;
	}
	esp_cache_msync(src, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_UNALIGNED);
	esp_cache_msync(dst, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
	dw_gdma_block_transfer_config_t tcfg={
		.src={
			.addr=(uint32_t)src,
			.burst_mode=DW_GDMA_BURST_MODE_INCREMENT,
			.burst_items=DW_GDMA_BURST_ITEMS_64,
			.burst_len=16,
			.width=DW_GDMA_TRANS_WIDTH_64,
		},
		.dst={
			.addr=(uint32_t)dst,
			.burst_mode=DW_GDMA_BURST_MODE_INCREMENT,
			.burst_items=DW_GDMA_BURST_ITEMS_64,
			.burst_len=16,
			.width=DW_GDMA_TRANS_WIDTH_64,
		},
		.size=n/8,
	};
	dw_gdma_block_markers_t markers={
		.is_valid=true,
		.is_last=true,
		.en_trans_done_intr=true,
	};
	copies_started++;
	dw_gdma_channel_config_transfer(dw_chan, &tcfg);
	dw_gdma_channel_set_block_markers(dw_chan, markers);
	dw_gdma_channel_enable_ctrl(dw_chan, true);
	return copies_started;
}

void QG_CopyWait(uint32_t ticket) {
	int64_t t0=esp_timer_get_time();
	while ((int32_t)(copies_done-ticket)<0) {
		if (esp_timer_get_time()-t0>20000) {	//safety net: a lost interrupt must not hang the game
			static int n;
			if (n++<5) printf("band copy timeout: ticket %lu done %lu\n", (unsigned long)ticket, (unsigned long)copies_done);
			copies_done=ticket;
			return;
		}
	}
}
