// Low-overhead benchmarking and profiling.
//
// A measurement window is either the first CONFIG_QUAKE_BENCHMARK_SECONDS of the
// attract-mode demo after boot, or a timedemo. During a window we count frames,
// accumulate the display pipeline timings reported by display.c and, with
// CONFIG_QUAKE_PROFILE, sample the game core's program counter. The PC is read
// from the ASSIST_DEBUG PC-recording register by a timer interrupt on core 1, so
// nothing extra runs on core 0, where the game runs.

#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gptimer.h"
#include "soc/assist_debug_reg.h"
#include "sdkconfig.h"
#include "profile.h"

#include "quakedef.h"

static portMUX_TYPE stats_mux=portMUX_INITIALIZER_UNLOCKED;
static volatile int active=0;
static int64_t win_start_us;
static int win_frames;
static int64_t disp_convert_us, disp_scale_us, drawframe_wait_us;
static int disp_frames, wait_frames;

#if CONFIG_QUAKE_PROFILE
static uint32_t *samples;
static volatile uint32_t nsamples;
static gptimer_handle_t sample_timer;
#if CONFIG_QUAKE_PROFILE_CORE1
static uint32_t *samples1;
static volatile uint32_t nsamples1;
static gptimer_handle_t sample_timer1;
#endif

static bool IRAM_ATTR sample_isr(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg) {
	if (active && nsamples<CONFIG_QUAKE_PROFILE_MAX_SAMPLES) {
		samples[nsamples++]=REG_READ(ASSIST_DEBUG_CORE_0_RCD_PDEBUGPC_REG);
	}
	return false;
}

#if CONFIG_QUAKE_PROFILE_CORE1
static bool IRAM_ATTR sample1_isr(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg) {
	if (active && nsamples1<CONFIG_QUAKE_PROFILE_MAX_SAMPLES) {
		samples1[nsamples1++]=REG_READ(ASSIST_DEBUG_CORE_1_RCD_PDEBUGPC_REG);
	}
	return false;
}
#endif

static gptimer_handle_t new_sample_timer(gptimer_alarm_cb_t cb, int extra_us) {
	gptimer_handle_t t;
	const gptimer_config_t cfg={
		.clk_src=GPTIMER_CLK_SRC_DEFAULT,
		.direction=GPTIMER_COUNT_UP,
		.resolution_hz=1000000,
	};
	ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &t));
	const gptimer_alarm_config_t alarm={
		.alarm_count=1000000/CONFIG_QUAKE_PROFILE_SAMPLE_HZ+extra_us,
		.reload_count=0,
		.flags.auto_reload_on_alarm=true,
	};
	ESP_ERROR_CHECK(gptimer_set_alarm_action(t, &alarm));
	const gptimer_event_callbacks_t cbs={ .on_alarm=cb };
	ESP_ERROR_CHECK(gptimer_register_event_callbacks(t, &cbs, NULL));
	ESP_ERROR_CHECK(gptimer_enable(t));
	return t;
}

//the timer interrupt is allocated on the core that registers the callback
static void sample_timer_setup_task(void *arg) {
	sample_timer=new_sample_timer(sample_isr, 0);
	xTaskNotifyGive((TaskHandle_t)arg);
	vTaskDelete(NULL);
}

static int cmp_u32(const void *a, const void *b) {
	uint32_t x=*(const uint32_t*)a, y=*(const uint32_t*)b;
	return (x>y)-(x<y);
}

static void dump_samples_core(uint32_t *buf, uint32_t n, const char *tag) {
	qsort(buf, n, sizeof(uint32_t), cmp_u32);
	printf("%s_BEGIN %lu %d\n", tag, (unsigned long)n, CONFIG_QUAKE_PROFILE_SAMPLE_HZ);
	for (uint32_t i=0; i<n; ) {
		uint32_t j=i;
		while (j<n && buf[j]==buf[i]) j++;
		printf("%s %08lx %lu\n", tag, (unsigned long)buf[i], (unsigned long)(j-i));
		i=j;
	}
	printf("%s_END\n", tag);
	if (n>=CONFIG_QUAKE_PROFILE_MAX_SAMPLES) {
		printf("%s: sample buffer full, later samples were dropped\n", tag);
	}
}

static void dump_samples(void) {
	dump_samples_core(samples, nsamples, "PROF");
#if CONFIG_QUAKE_PROFILE_CORE1
	dump_samples_core(samples1, nsamples1, "PROF1");
#endif
}
#endif

void profile_init() {
#if CONFIG_QUAKE_PROFILE
	samples=heap_caps_malloc(CONFIG_QUAKE_PROFILE_MAX_SAMPLES*sizeof(uint32_t), MALLOC_CAP_SPIRAM);
	assert(samples);
	//make sure core 0's PC is being recorded (ESP-IDF normally enables this at startup)
	REG_WRITE(ASSIST_DEBUG_CORE_0_RCD_EN_REG, ASSIST_DEBUG_CORE_0_RCD_PDEBUGEN|ASSIST_DEBUG_CORE_0_RCD_RECORDEN);
	xTaskCreatePinnedToCore(sample_timer_setup_task, "proftimer", 4096, xTaskGetCurrentTaskHandle(), 5, NULL, 1);
	ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if CONFIG_QUAKE_PROFILE_CORE1
	samples1=heap_caps_malloc(CONFIG_QUAKE_PROFILE_MAX_SAMPLES*sizeof(uint32_t), MALLOC_CAP_SPIRAM);
	assert(samples1);
	REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_EN_REG, ASSIST_DEBUG_CORE_1_RCD_PDEBUGEN|ASSIST_DEBUG_CORE_1_RCD_RECORDEN);
	//app_main runs on core 0. A slightly different period keeps the two sampling interrupts
	//from always coinciding (each would then only ever see the other core in its ISR).
	sample_timer1=new_sample_timer(sample1_isr, 37);
#endif
#endif
}

extern int r_peak_edges, r_peak_surfs;
extern int d_stat_builds, d_stat_buildtexels, d_stat_dlightbuilds;
static int stat_builds0, stat_texels0, stat_dlight0;

static void window_start(void) {
	taskENTER_CRITICAL(&stats_mux);
	disp_convert_us=0;
	disp_scale_us=0;
	drawframe_wait_us=0;
	disp_frames=0;
	wait_frames=0;
	taskEXIT_CRITICAL(&stats_mux);
	win_frames=0;
	stat_builds0=d_stat_builds;
	stat_texels0=d_stat_buildtexels;
	stat_dlight0=d_stat_dlightbuilds;
	win_start_us=esp_timer_get_time();
#if CONFIG_QUAKE_PROFILE
	nsamples=0;
	ESP_ERROR_CHECK(gptimer_start(sample_timer));
#if CONFIG_QUAKE_PROFILE_CORE1
	nsamples1=0;
	ESP_ERROR_CHECK(gptimer_start(sample_timer1));
#endif
#endif
	active=1;
}

static void window_end(const char *what, int frames, float seconds) {
	active=0;
#if CONFIG_QUAKE_PROFILE
	ESP_ERROR_CHECK(gptimer_stop(sample_timer));
#if CONFIG_QUAKE_PROFILE_CORE1
	ESP_ERROR_CHECK(gptimer_stop(sample_timer1));
#endif
#endif
	printf("BENCH %s: %d frames %.2f seconds %.2f fps\n", what, frames, seconds, frames/seconds);
	if (disp_frames) {
		printf("BENCH display (core 1) per frame: palette convert %.2f ms, PPA scale %.2f ms; %d frames converted\n",
				disp_convert_us/1000.0/disp_frames, disp_scale_us/1000.0/disp_frames, disp_frames);
	}
	if (wait_frames) {
		printf("BENCH game (core 0) waited for display: %.2f ms per frame\n",
				drawframe_wait_us/1000.0/wait_frames);
	}
	TaskHandle_t quake_task=xTaskGetHandle("quake");
	printf("BENCH peak edges %d, peak surfaces %d, quake stack unused %lu bytes\n",
			r_peak_edges, r_peak_surfs,
			quake_task?(unsigned long)uxTaskGetStackHighWaterMark(quake_task):0);
	if (frames>0) {
		printf("BENCH surface builds per frame %.1f (%.0f texels, %.1f due to dynamic lights)\n",
				(float)(d_stat_builds-stat_builds0)/frames,
				(float)(d_stat_buildtexels-stat_texels0)/frames,
				(float)(d_stat_dlightbuilds-stat_dlight0)/frames);
	}
	{
		extern int r_pipe_stat_frames, r_pipe_stat_deferred;
		printf("BENCH pipeline: models/particles of %d of %d frames drawn by core 1 (r_pipe.c)\n",
				r_pipe_stat_deferred, r_pipe_stat_frames);
	}
	printf("BENCH internal RAM free %u bytes, largest block %u bytes\n",
			heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
			heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
	printf("BENCH PSRAM heap free %u bytes of %u, largest block %u bytes\n",
			heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
			heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#if CONFIG_QUAKE_PROFILE
	dump_samples();
#endif
}

//automatic benchmark of the attract-mode demo; armed once at boot
static int bench_state=(CONFIG_QUAKE_BENCHMARK_SECONDS>0)?1:0;

void profile_frame() {
	if (bench_state==0 || cls.timedemo) {
		if (active) win_frames++;
		return;
	}
	//only count frames while a demo is actually playing
	if (!cls.demoplayback || cls.signon!=SIGNONS) return;
	if (bench_state==1) {
		//skip the first demo frame, which includes level loading
		bench_state=2;
		return;
	}
	if (bench_state==2) {
		window_start();
		bench_state=3;
		return;
	}
	win_frames++;
	float seconds=(float)(esp_timer_get_time()-win_start_us)*1e-6f;
	if (seconds>=CONFIG_QUAKE_BENCHMARK_SECONDS) {
		window_end("attract demo", win_frames, seconds);
		bench_state=0;
	}
}

static int shot_countdown=-1, shots_left=CONFIG_QUAKE_SCREENSHOTS;

int profile_screenshot_due() {
	if (shots_left<=0) return 0;
	if (shot_countdown<0) {
		if (bench_state!=0 || CONFIG_QUAKE_BENCHMARK_SECONDS==0) return 0;
		shot_countdown=1;	//benchmark finished: first shot on the next frame
	}
	if (--shot_countdown>0) return 0;
	shots_left--;
	shot_countdown=100;
	return 1;
}

void QG_TimeDemoStart(void) {
	if (active) return; //attract-demo window still running
	window_start();
}

void QG_TimeDemoEnd(int frames, float seconds) {
	if (!active) return;
	window_end("timedemo", frames, seconds);
}

void profile_display_times(int64_t convert_us, int64_t scale_us) {
	if (!active) return;
	taskENTER_CRITICAL(&stats_mux);
	disp_convert_us+=convert_us;
	disp_scale_us+=scale_us;
	disp_frames++;
	taskEXIT_CRITICAL(&stats_mux);
}

void profile_drawframe_wait(int64_t wait_us) {
	if (!active) return;
	taskENTER_CRITICAL(&stats_mux);
	drawframe_wait_us+=wait_us;
	wait_frames++;
	taskEXIT_CRITICAL(&stats_mux);
}
