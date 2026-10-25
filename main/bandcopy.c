// Asynchronous SRAM -> PSRAM copies for the renderer's frame-buffer bands (r_edge.c).
//
// The world is drawn into small internal-RAM band buffers; each finished band is copied to
// the PSRAM frame buffer by an AXI GDMA channel pair, so the cores never take write-allocate
// cache misses on the frame buffer. One copy at a time (the renderer waits for the previous
// one before starting the next), with link lists allocated once: esp_async_memcpy allocates
// per copy, which cost more than the copy saved. The destination rows are invalidated in the
// cache first: they hold no data the CPU still needs (the frame buffer was written back
// before the display read it).

#include <stdio.h>
#include <string.h>
#include "esp_private/gdma.h"
#include "esp_private/gdma_link.h"
#include "esp_cache.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "quakegeneric.h"
#include "esp_timer.h"

#define MAX_COPY	(16*640)	// a band (r_edge.c)
#define DESC_BYTES	4032		// < 4095, a multiple of 64

static gdma_channel_handle_t tx_chan, rx_chan;
static gdma_link_list_handle_t tx_list, rx_list;
static size_t rx_ext_align, rx_int_align, tx_ext_align, tx_int_align;
static volatile uint32_t copies_done;
static uint32_t copies_started;

static IRAM_ATTR bool copy_done_isr(gdma_channel_handle_t chan, gdma_event_data_t *e, void *arg) {
	copies_done++;
	return false;
}

int QG_CopyInit(void) {
	gdma_channel_alloc_config_t tx_cfg={
		.flags.reserve_sibling=1,
		.direction=GDMA_CHANNEL_DIRECTION_TX,
	};
	if (gdma_new_axi_channel(&tx_cfg, &tx_chan)!=ESP_OK) goto fail;
	gdma_channel_alloc_config_t rx_cfg={
		.direction=GDMA_CHANNEL_DIRECTION_RX,
		.sibling_chan=tx_chan,
	};
	if (gdma_new_axi_channel(&rx_cfg, &rx_chan)!=ESP_OK) goto fail;
	gdma_trigger_t m2m=GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_M2M, 0);
	uint32_t free_ids=0;
	gdma_get_free_m2m_trig_id_mask(tx_chan, &free_ids);
	m2m.instance_id=__builtin_ctz(free_ids);
	if (gdma_connect(rx_chan, m2m)!=ESP_OK || gdma_connect(tx_chan, m2m)!=ESP_OK) goto fail;
	gdma_strategy_config_t strategy={
		.owner_check=true,
		.auto_update_desc=true,
		.eof_till_data_popped=true,
	};
	gdma_apply_strategy(tx_chan, &strategy);
	gdma_apply_strategy(rx_chan, &strategy);
	gdma_transfer_config_t transfer={
		.max_data_burst_size=64,
		.access_ext_mem=true,
	};
	if (gdma_config_transfer(tx_chan, &transfer)!=ESP_OK || gdma_config_transfer(rx_chan, &transfer)!=ESP_OK) goto fail;
	gdma_get_alignment_constraints(rx_chan, &rx_int_align, &rx_ext_align);
	gdma_get_alignment_constraints(tx_chan, &tx_int_align, &tx_ext_align);
	gdma_link_list_config_t list_cfg={
		.item_alignment=64,	//AXI descriptors
		.num_items=(MAX_COPY+DESC_BYTES-1)/DESC_BYTES+1,
		.flags.check_owner=true,
	};
	if (gdma_new_link_list(&list_cfg, &tx_list)!=ESP_OK || gdma_new_link_list(&list_cfg, &rx_list)!=ESP_OK) goto fail;
	gdma_rx_event_callbacks_t cbs={ .on_recv_eof=copy_done_isr };
	if (gdma_register_rx_event_callbacks(rx_chan, &cbs, NULL)!=ESP_OK) goto fail;
	printf("band copy: AXI GDMA, alignment rx %u/%u tx %u/%u\n", (unsigned)rx_int_align, (unsigned)rx_ext_align, (unsigned)tx_int_align, (unsigned)tx_ext_align);
	return 1;
fail:
	printf("band copy: GDMA setup failed, drawing straight into the frame buffer\n");
	return 0;
}

//returns a ticket for QG_CopyWait, or 0 if the copy was done synchronously
uint32_t QG_CopyStart(void *dst, void *src, size_t n) {
	if (!rx_list || n>MAX_COPY || ((uint32_t)dst & 127) || (n & 127)) {
		memcpy(dst, src, n);
		return 0;
	}
	esp_cache_msync(src, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_UNALIGNED);
	esp_cache_msync(dst, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
	gdma_buffer_mount_config_t tx_buf={
		.buffer=src,
		.buffer_alignment=tx_int_align,
		.length=n,
		.flags={ .mark_eof=true, .mark_final=GDMA_FINAL_LINK_TO_NULL },
	};
	gdma_buffer_mount_config_t rx_buf={
		.buffer=dst,
		.buffer_alignment=rx_ext_align,
		.length=n,
		.flags={ .mark_final=GDMA_FINAL_LINK_TO_NULL },
	};
	gdma_link_mount_buffers(tx_list, 0, &tx_buf, 1, NULL);
	gdma_link_mount_buffers(rx_list, 0, &rx_buf, 1, NULL);
	copies_started++;
	gdma_start(rx_chan, gdma_link_get_head_addr(rx_list));
	gdma_start(tx_chan, gdma_link_get_head_addr(tx_list));
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
