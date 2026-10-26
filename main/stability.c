// Stability diagnostics (CONFIG_QUAKE_STABILITY_DEBUG).
//
// - At boot: the reset reason, and a record of what the previous run was doing (kept in
//   .noinit SRAM, which survives panics, watchdog and software resets but not power-on or
//   brownout). Together with the ROM's "Core0/1 Saved PC" line (the PC each core had when
//   the reset hit) and a core dump (panics), this places a hang or reboot.
// - A watchdog task on core 1 notices when no frame has been presented for a while and
//   prints both cores' program counters (sampled from the ASSIST_DEBUG PC-recording
//   registers, without stopping the cores) and the pipeline state, until frames resume.
// - The draw task checks each converted frame: runs of (nearly) all-black frames are
//   logged with their length and the game state, so a black flash on the monitor can be
//   told apart from the game drawing black, or the display losing sync (a DSI underrun is
//   logged by the esp_lcd driver: "underrun happens").
//
// Lines start with "STAB" so they're easy to grep in a serial log.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "soc/assist_debug_reg.h"
#include "soc/pmu_reg.h"
#include "soc/pvt_reg.h"
#include "sdkconfig.h"
#include "stability.h"

#include "quakedef.h"

#if CONFIG_QUAKE_STABILITY_DEBUG

#define REC_MAGIC		0x51554b45
#define PC_HISTORY		16
#define STALL_MS		250		// no frame presented for this long: report

typedef struct {
	uint32_t magic;
	uint32_t boots;
	uint32_t game_ticks;		// core 0 game loop iterations
	uint32_t presents;			// frames handed to the display
	uint32_t uptime_ms;			// refreshed by the watchdog task
	uint32_t stalls;			// stall reports this run
	uint32_t black_runs;		// runs of black frames this run
	uint32_t pc_pos;
	uint32_t pc0[PC_HISTORY];	// last PCs sampled by the watchdog task (every 20 ms)
	uint32_t pc1[PC_HISTORY];
	char state[96];				// game state at the last sample
} crashrec_t;

static __NOINIT_ATTR crashrec_t rec;

void D_JobsDebugState(char *buf, int size);
void QG_CopyDebugState(char *buf, int size);
qboolean R_PipePending(void);

static const char *reset_reason_str(esp_reset_reason_t r) {
	switch (r) {
	case ESP_RST_POWERON: return "power-on";
	case ESP_RST_EXT: return "external pin";
	case ESP_RST_SW: return "software (esp_restart)";
	case ESP_RST_PANIC: return "PANIC (exception/abort)";
	case ESP_RST_INT_WDT: return "INTERRUPT WATCHDOG";
	case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
	case ESP_RST_WDT: return "OTHER WATCHDOG (RWDT/MWDT: lockup?)";
	case ESP_RST_DEEPSLEEP: return "deep sleep";
	case ESP_RST_BROWNOUT: return "BROWNOUT";
	case ESP_RST_SDIO: return "sdio";
	case ESP_RST_USB: return "usb (host/esptool)";
	case ESP_RST_JTAG: return "jtag";
	case ESP_RST_EFUSE: return "efuse";
	case ESP_RST_PWR_GLITCH: return "POWER GLITCH";
	case ESP_RST_CPU_LOCKUP: return "CPU LOCKUP";
	default: return "unknown";
	}
}

static void game_state(char *buf, int size) {
	snprintf(buf, size, "cls.state %d demo %d signon %d loading %d key_dest %d intermission %d",
			cls.state, cls.demoplayback, cls.signon, scr_disabled_for_loading, key_dest, cl.intermission);
}

//a failed allocation is often the first sign of trouble (USB host and drivers abort on it)
static void alloc_failed(size_t size, uint32_t caps, const char *function_name) {
	esp_rom_printf("STAB ALLOC FAILED %u bytes caps 0x%x in %s: internal free %u largest %u, dma free %u largest %u, min-ever %u\n",
			size, caps, function_name?function_name:"?",
			heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
			heap_caps_get_free_size(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL),
			heap_caps_get_largest_free_block(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL),
			heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

void stability_boot_report(void) {
	heap_caps_register_failed_alloc_callback(alloc_failed);
	esp_reset_reason_t r=esp_reset_reason();
	printf("STAB reset reason: %s (%d)\n", reset_reason_str(r), r);
	if (rec.magic==REC_MAGIC) {
		printf("STAB previous run (boot #%lu): up %lu ms, %lu game ticks, %lu frames presented, %lu stalls, %lu black runs\n",
				(unsigned long)rec.boots, (unsigned long)rec.uptime_ms, (unsigned long)rec.game_ticks,
				(unsigned long)rec.presents, (unsigned long)rec.stalls, (unsigned long)rec.black_runs);
		rec.state[sizeof(rec.state)-1]=0;
		printf("STAB previous run last state: %s\n", rec.state);
		printf("STAB previous run last PCs (oldest first, 20 ms apart):\n");
		for (int i=0; i<PC_HISTORY; i++) {
			int k=(rec.pc_pos+i)%PC_HISTORY;
			printf("STAB   core0 0x%08lx core1 0x%08lx\n", (unsigned long)rec.pc0[k], (unsigned long)rec.pc1[k]);
		}
		rec.boots++;
	} else {
		printf("STAB no record of a previous run (power-on)\n");
		rec.boots=1;
	}
	uint32_t boots=rec.boots;
	memset(&rec, 0, sizeof(rec));
	rec.boots=boots;
	rec.magic=REC_MAGIC;
}

void stability_game_tick(void) {
	rec.game_ticks++;
}

void stability_present(void) {
	rec.presents++;
}

#define NPCS 64

static int cmp_u32(const void *a, const void *b) {
	uint32_t x=*(const uint32_t*)a, y=*(const uint32_t*)b;
	return x<y?-1:x>y;
}

static uint32_t pcs0[NPCS], pcs1[NPCS];
static TaskHandle_t sampler1_task, watchdog_handle;

static void sample_pcs(uint32_t *pcs, uint32_t reg) {
	for (int i=0; i<NPCS; i++) {
		pcs[i]=REG_READ(reg);
		esp_rom_delay_us(200);
	}
}

//runs on core 0 (above the game task) so that core 1 is sampled while doing its own work
static void sampler1(void *arg) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		sample_pcs(pcs1, ASSIST_DEBUG_CORE_1_RCD_PDEBUGPC_REG);
		xTaskNotifyGive(watchdog_handle);
	}
}

//print the distinct sampled PCs with counts
static void print_pc_histogram(int core, uint32_t *pcs) {
	qsort(pcs, NPCS, sizeof(uint32_t), cmp_u32);
	char line[256];
	int n=snprintf(line, sizeof(line), "STAB   core%d PCs:", core);
	for (int i=0; i<NPCS;) {
		int j=i;
		while (j<NPCS && pcs[j]==pcs[i]) j++;
		if (n<(int)sizeof(line)-24) n+=snprintf(line+n, sizeof(line)-n, " %08lx*%d", (unsigned long)pcs[i], j-i);
		i=j;
	}
	printf("%s\n", line);
}

//LT8912B HDMI bridge: its measurement of the incoming DSI sync (as read by the BSP at init);
//a change means the bridge saw the DSI video stream break up
static i2c_master_dev_handle_t lt_dev;
static uint8_t lt_last[4];
static int lt_ok;

static void lt8912b_probe_init(void) {
	i2c_master_bus_handle_t bus=bsp_i2c_get_handle();
	i2c_device_config_t cfg={ .dev_addr_length=I2C_ADDR_BIT_LEN_7, .device_address=0x48, .scl_speed_hz=100000 };
	if (!bus || i2c_master_bus_add_device(bus, &cfg, &lt_dev)!=ESP_OK) {
		printf("STAB LT8912B probe unavailable\n");
		lt_dev=NULL;
	}
}

static void lt8912b_probe(void) {
	uint8_t reg=0x9c, v[4];
	if (!lt_dev) return;
	if (i2c_master_transmit_receive(lt_dev, &reg, 1, v, 4, 50)!=ESP_OK) {
		printf("STAB LT8912B read failed at %lld ms\n", esp_timer_get_time()/1000);
		return;
	}
	//the vertical count jitters by +-1 line; report a change of the horizontal measurement or a
	//vertical jump of more than 2 (input lost or re-locked)
	int dv=(int)((v[3]<<8)|v[2])-(int)((lt_last[3]<<8)|lt_last[2]);
	if (!lt_ok || v[0]!=lt_last[0] || v[1]!=lt_last[1] || dv>2 || dv<-2) {
		printf("STAB LT8912B dsi sync h %02x%02x v %02x%02x at %lld ms%s\n", v[1], v[0], v[3], v[2],
				esp_timer_get_time()/1000, lt_ok?" (CHANGED)":"");
		memcpy(lt_last, v, 4);
		lt_ok=1;
	}
}

//Core supply: PVT auto-dbias (CONFIG_ESP_ENABLE_PVT) watches on-chip delay cells and raises
//the HP voltage request when the logic gets slow. A request at its ceiling (31) means the chip
//wants more voltage than it is getting (the external DC-DC must follow FB_DCDC, see the
//AR2025-005 advisory).
static uint32_t pvt_min=99, pvt_max;

static void pvt_sample(void) {
	uint32_t v=GET_PERI_REG_BITS2(PMU_HP_ACTIVE_HP_REGULATOR0_REG, PMU_HP_DBIAS_VOL_V, PMU_HP_DBIAS_VOL_S);
	if (v<pvt_min) pvt_min=v;
	if (v>pvt_max) pvt_max=v;
}

static void pvt_report(void) {
	uint32_t c0=REG_READ(PVT_COMB_PD_SITE3_UNIT0_VT1_CONF2_REG), c1=REG_READ(PVT_COMB_PD_SITE3_UNIT1_VT1_CONF2_REG);
	printf("STAB pvt hp_dbias %lu (min %lu max %lu) dcm_vset %lu, site3 timing_err u0 %lu%s u1 %lu%s\n",
			(unsigned long)GET_PERI_REG_BITS2(PMU_HP_ACTIVE_HP_REGULATOR0_REG, PMU_HP_DBIAS_VOL_V, PMU_HP_DBIAS_VOL_S),
			(unsigned long)pvt_min, (unsigned long)pvt_max,
			(unsigned long)GET_PERI_REG_BITS2(PMU_HP_ACTIVE_BIAS_REG, PMU_HP_ACTIVE_DCM_VSET_V, PMU_HP_ACTIVE_DCM_VSET_S),
			(unsigned long)(c0>>16), (c0&BIT(15))?" ovf":"", (unsigned long)(c1>>16), (c1&BIT(15))?" ovf":"");
	pvt_min=99;
	pvt_max=0;
}

static void watchdog_task(void *arg) {
	uint32_t last_presents=rec.presents, last_ticks=rec.game_ticks;
	int64_t last_change=esp_timer_get_time();
	int64_t next_report=0;
	int stalled=0;
	int64_t next_heap=0;
	char buf[128];
	//enable PC recording on both cores (the profiler does the same)
	REG_WRITE(ASSIST_DEBUG_CORE_0_RCD_EN_REG, ASSIST_DEBUG_CORE_0_RCD_PDEBUGEN|ASSIST_DEBUG_CORE_0_RCD_RECORDEN);
	REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_EN_REG, ASSIST_DEBUG_CORE_1_RCD_PDEBUGEN|ASSIST_DEBUG_CORE_1_RCD_RECORDEN);
	lt8912b_probe_init();
	for (int iter=0;; iter++) {
		vTaskDelay(pdMS_TO_TICKS(20));
		if (iter%5==0) lt8912b_probe();
		pvt_sample();
		int64_t now=esp_timer_get_time();
		rec.uptime_ms=now/1000;
		rec.pc0[rec.pc_pos]=REG_READ(ASSIST_DEBUG_CORE_0_RCD_PDEBUGPC_REG);
		rec.pc1[rec.pc_pos]=REG_READ(ASSIST_DEBUG_CORE_1_RCD_PDEBUGPC_REG);
		rec.pc_pos=(rec.pc_pos+1)%PC_HISTORY;
		game_state(rec.state, sizeof(rec.state));

		if (now>=next_heap) {
			//internal RAM headroom: USB host, lwIP and the drivers allocate from it at run time
			next_heap=now+10000000;
			printf("STAB heap internal free %u min %u largest %u, dma free %u largest %u\n",
					heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
					heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
					heap_caps_get_free_size(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL),
					heap_caps_get_largest_free_block(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL));
			pvt_report();
		}

		uint32_t p=rec.presents;
		if (p!=last_presents) {
			if (stalled) {
				printf("STAB stall end: no frame for %lld ms (game ticks advanced %lu)\n",
						(now-last_change)/1000, (unsigned long)(rec.game_ticks-last_ticks));
				stalled=0;
			}
			last_presents=p;
			last_ticks=rec.game_ticks;
			last_change=now;
			continue;
		}
		if (p==0 || now-last_change<STALL_MS*1000) continue;	//armed by the first frame
		if (!stalled) {
			stalled=1;
			rec.stalls++;
			next_report=now;
		}
		if (now<next_report) continue;
		next_report=now+500000;	//repeat every 500 ms while stalled
		printf("STAB STALL %lld ms: %lu frames presented, game ticks +%lu since\n", (now-last_change)/1000,
				(unsigned long)p, (unsigned long)(rec.game_ticks-last_ticks));
		printf("STAB   %s\n", rec.state);
		D_JobsDebugState(buf, sizeof(buf));
		printf("STAB   %s pipe pending %d\n", buf, R_PipePending());
		QG_CopyDebugState(buf, sizeof(buf));
		printf("STAB   %s\n", buf);
		//core 0 sampled from here (core 1), core 1 from the helper on core 0, at the same time
		xTaskNotifyGive(sampler1_task);
		sample_pcs(pcs0, ASSIST_DEBUG_CORE_0_RCD_PDEBUGPC_REG);
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
		print_pc_histogram(0, pcs0);
		print_pc_histogram(1, pcs1);
	}
}

void stability_init(void) {
	//stack in PSRAM: internal RAM is tight, and taking 4 KB of it made USB HID enumeration fail
	xTaskCreatePinnedToCoreWithCaps(watchdog_task, "stab_wdt", 4096, NULL, configMAX_PRIORITIES-2, &watchdog_handle, 1, MALLOC_CAP_SPIRAM);
	xTaskCreatePinnedToCoreWithCaps(sampler1, "stab_s1", 2048, NULL, configMAX_PRIORITIES-2, &sampler1_task, 0, MALLOC_CAP_SPIRAM);
}

static int black_frames;
static int64_t black_start_us;

void stability_check_frame(const uint8_t *pixels, const uint32_t *pal888) {
	//sample a 32x24 grid; "black" = every channel below 24
	int dark=0, total=0;
	for (int y=5; y<QUAKEGENERIC_RES_Y; y+=QUAKEGENERIC_RES_Y/24) {
		const uint8_t *row=pixels+y*QUAKEGENERIC_RES_X;
		for (int x=7; x<QUAKEGENERIC_RES_X; x+=QUAKEGENERIC_RES_X/32) {
			uint32_t c=pal888[row[x]];
			if (((c>>16)&0xff)<24 && ((c>>8)&0xff)<24 && (c&0xff)<24) dark++;
			total++;
		}
	}
	int black=(dark*100>=total*98);
	if (black && !black_frames) {
		char st[96];
		uint32_t lum=0;
		for (int i=0; i<256; i++) lum+=((pal888[i]>>16)&0xff)+((pal888[i]>>8)&0xff)+(pal888[i]&0xff);
		game_state(st, sizeof(st));
		black_start_us=esp_timer_get_time();
		rec.black_runs++;
		printf("STAB BLACK frame start at %lld ms (frame %lu, %d/%d samples dark, palette mean %lu): %s\n",
				black_start_us/1000, (unsigned long)rec.presents, dark, total, (unsigned long)(lum/768), st);
	}
	if (black) {
		black_frames++;
	} else if (black_frames) {
		printf("STAB BLACK run end: %d frames, %lld ms\n", black_frames, (esp_timer_get_time()-black_start_us)/1000);
		black_frames=0;
	}
}

#else

void stability_boot_report(void) {}
void stability_init(void) {}
void stability_game_tick(void) {}
void stability_present(void) {}
void stability_check_frame(const uint8_t *pixels, const uint32_t *pal888) {}

#endif
