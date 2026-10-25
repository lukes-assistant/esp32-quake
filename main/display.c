// Copyright 2024 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_ops.h"
#include "quakegeneric.h"
#include "soc/mipi_dsi_bridge_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "font_8x16.h"
#include "driver/ppa.h"
#include "profile.h"

#include "quakedef.h"

//ESP32-P4-PC: HDMI output via the LT8912B bridge. The BSP's LCD resolution macros
//describe its MIPI panels, not the HDMI mode, so the output size is defined here.
//The LT8912B path also requires RGB888 framebuffers.
#if CONFIG_BSP_LCD_HDMI_640x480_60HZ
#define LCD_H_RES 640
#define LCD_V_RES 480
#define LCD_HDMI_RES BSP_HDMI_RES_640x480
#else
#define LCD_H_RES 1280
#define LCD_V_RES 720
#define LCD_HDMI_RES BSP_HDMI_RES_1280x720
#endif
#define LCD_BPP 3

//When Quake renders at the display resolution, the palette conversion writes RGB888 straight
//into the frame buffer; otherwise it writes RGB565 that the PPA scales and converts.
#define DIRECT_CONVERT (QUAKEGENERIC_RES_X==LCD_H_RES && QUAKEGENERIC_RES_Y==LCD_V_RES)

esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_panel_io_handle_t io_handle = NULL;


static uint16_t pal[256];
static uint32_t pal888[256];	//0x00RRGGBB: little-endian B,G,R bytes as in the frame buffer
static volatile int pal_changed=1;	//pal888 changed since the PPA CLUT was last loaded
static uint8_t *cur_pixels;
static uint8_t *lcdbuf[2]={};
static int cur_buf=1;
static int draw_task_quit=0;

static TaskHandle_t draw_task_handle;
static SemaphoreHandle_t drawing_mux;

static int fps_ticks=0;
static int64_t start_time_fps_meas;

void QG_DrawFrame(void *pixels) {
	int64_t wait_start_us=esp_timer_get_time();
	xSemaphoreTake(drawing_mux, portMAX_DELAY);
	profile_drawframe_wait(esp_timer_get_time()-wait_start_us);
	profile_frame();
	cur_pixels=pixels;
	xSemaphoreGive(drawing_mux);
	xTaskNotifyGive(draw_task_handle);
	fps_ticks++;
	if (fps_ticks>100) {
		int64_t newtime_us=esp_timer_get_time();
		int64_t fpstime=(newtime_us-start_time_fps_meas)/fps_ticks;
		fps_ticks=0;
		start_time_fps_meas = newtime_us;
		printf("Fps: %02f\n", 1000000.0/fpstime);
	}
}

#if DIRECT_CONVERT
//CPU conversion: 4 pixels at a time into 3 aligned 32-bit words of RGB888
static void convert_cpu(const uint8_t *p, uint8_t *dst, const uint32_t *palette) {
	uint32_t *out=(uint32_t*)dst;
	for (int i=0; i<LCD_H_RES*LCD_V_RES; i+=4) {
		uint32_t p0=palette[p[0]], p1=palette[p[1]], p2=palette[p[2]], p3=palette[p[3]];
		out[0]=p0|(p1<<24);
		out[1]=(p1>>8)|(p2<<16);
		out[2]=(p2>>16)|(p3<<8);
		p+=4;
		out+=3;
	}
}

//Hardware conversion: the PPA blending engine reads the 8-bit frame as L8 through its CLUT
//(local addition to the PPA driver, see components/esp_driver_ppa) and writes RGB888. The
//foreground is the same buffer made fully transparent.
static ppa_client_handle_t ppa_blend;

static esp_err_t convert_ppa(const uint8_t *src, uint8_t *dst) {
	ppa_blend_oper_config_t op={
		.in_bg={
			.buffer=src,
			.pic_w=LCD_H_RES, .pic_h=LCD_V_RES,
			.block_w=LCD_H_RES, .block_h=LCD_V_RES,
			.blend_cm=PPA_BLEND_COLOR_MODE_BG_L8,
		},
		.in_fg={
			.buffer=src,
			.pic_w=LCD_H_RES, .pic_h=LCD_V_RES,
			.block_w=LCD_H_RES, .block_h=LCD_V_RES,
			.blend_cm=PPA_BLEND_COLOR_MODE_A8,
		},
		.out={
			.buffer=dst,
			.buffer_size=LCD_H_RES*LCD_V_RES*LCD_BPP,
			.pic_w=LCD_H_RES, .pic_h=LCD_V_RES,
			.blend_cm=PPA_BLEND_COLOR_MODE_RGB888,
		},
		.bg_alpha_update_mode=PPA_ALPHA_FIX_VALUE,
		.bg_alpha_fix_val=255,
		.fg_alpha_update_mode=PPA_ALPHA_FIX_VALUE,
		.fg_alpha_fix_val=0,
		.mode=PPA_TRANS_MODE_BLOCKING,
	};
	return ppa_do_blend(ppa_blend, &op);
}

static void load_clut(const uint32_t *palette) {
	uint32_t clut[256];
	for (int i=0; i<256; i++) clut[i]=0xff000000|palette[i];
	ppa_blend_set_bg_clut(clut);
}

//Convert a test pattern with the PPA and check it against the CPU conversion, so a
//misconfigured PPA can never reach the screen. Allows +-1 per channel for blend rounding.
static bool ppa_convert_selftest(uint8_t *scratch_out) {
	ppa_client_config_t cfg={ .oper_type=PPA_OPERATION_BLEND };
	if (ppa_register_client(&cfg, &ppa_blend)!=ESP_OK) return false;
	uint8_t *src=heap_caps_malloc(LCD_H_RES*LCD_V_RES, MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA);
	uint8_t *ref=heap_caps_malloc(LCD_H_RES*LCD_V_RES*LCD_BPP, MALLOC_CAP_SPIRAM);
	uint32_t testpal[256];
	bool ok=false;
	if (src && ref) {
		for (int i=0; i<256; i++) testpal[i]=(i<<16)|((255-i)<<8)|((i*37)&0xff);
		for (int i=0; i<LCD_H_RES*LCD_V_RES; i++) src[i]=(uint8_t)(i*7+(i/LCD_H_RES));
		convert_cpu(src, ref, testpal);
		load_clut(testpal);
		if (convert_ppa(src, scratch_out)==ESP_OK) {
			int bad=0;
			for (int i=0; i<LCD_H_RES*LCD_V_RES*LCD_BPP; i++) {
				int d=(int)scratch_out[i]-(int)ref[i];
				if (d<-1 || d>1) bad++;
			}
			printf("PPA CLUT conversion self-test: %d bad bytes\n", bad);
			ok=(bad==0);
		} else {
			printf("PPA CLUT conversion self-test: ppa_do_blend failed\n");
		}
	}
	free(src);
	free(ref);
	if (!ok) {
		ppa_unregister_client(ppa_blend);
		ppa_blend=NULL;
	}
	pal_changed=1;
	return ok;
}
#endif

static void draw_task(void *param) {
	ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 2, (void**)&lcdbuf[0], (void**)&lcdbuf[1]));
#if DIRECT_CONVERT
	bool use_ppa=ppa_convert_selftest(lcdbuf[1]);
	printf("Palette conversion: %s\n", use_ppa?"PPA (hardware CLUT)":"CPU");
	uint32_t clut_pal[256];
	while(!draw_task_quit) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		xSemaphoreTake(drawing_mux, portMAX_DELAY);
		int64_t start_us = esp_timer_get_time();
		if (use_ppa) {
			//the game core may change the palette at any time; snapshot it under the mutex
			if (pal_changed) {
				pal_changed=0;
				memcpy(clut_pal, pal888, sizeof(clut_pal));
				load_clut(clut_pal);
			}
			ESP_ERROR_CHECK(convert_ppa(cur_pixels, lcdbuf[cur_buf]));
		} else {
			convert_cpu(cur_pixels, lcdbuf[cur_buf], pal888);
		}
		xSemaphoreGive(drawing_mux);
		profile_display_times(esp_timer_get_time()-start_us, 0);
		//do a draw to trigger cache write-back and the fb flip
		esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, lcdbuf[cur_buf]);
		cur_buf=cur_buf?0:1;
	}
#else
	ppa_client_config_t ppa_cfg={
		.oper_type=PPA_OPERATION_SRM,
	};
	ppa_client_handle_t ppa;
	ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &ppa));

	uint16_t *rgbfb=heap_caps_calloc(QUAKEGENERIC_RES_X*QUAKEGENERIC_RES_Y, sizeof(uint16_t), MALLOC_CAP_DMA|MALLOC_CAP_SPIRAM);
	assert(rgbfb);

	while(!draw_task_quit) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		
		xSemaphoreTake(drawing_mux, portMAX_DELAY);
		int64_t start_us = esp_timer_get_time();
		// convert pixels
		uint8_t *p=(uint8_t*)cur_pixels;
		uint16_t *lcdp=rgbfb;
		for (int y=0; y<QUAKEGENERIC_RES_Y; y++) {
			for (int x=0; x<QUAKEGENERIC_RES_X; x++) {
				*lcdp++=pal[*p++];
			}
		}

		int64_t convert_end_us = esp_timer_get_time();

		//use ppa to scale image and convert to rgb888
		ppa_srm_oper_config_t op={
			.in={
				.buffer=rgbfb,
				.pic_w=QUAKEGENERIC_RES_X,
				.pic_h=QUAKEGENERIC_RES_Y,
				.block_w=QUAKEGENERIC_RES_X,
				.block_h=QUAKEGENERIC_RES_Y,
				.srm_cm=PPA_SRM_COLOR_MODE_RGB565,
			},
			.out={
				.buffer=lcdbuf[cur_buf],
				.buffer_size=LCD_V_RES*LCD_H_RES*LCD_BPP,
				.pic_w=LCD_H_RES,
				.pic_h=LCD_V_RES,
				.srm_cm=PPA_SRM_COLOR_MODE_RGB888, //PPA converts from RGB565 while scaling
			},
			.scale_x=(float)LCD_H_RES/(float)QUAKEGENERIC_RES_X,
			.scale_y=(float)LCD_V_RES/(float)QUAKEGENERIC_RES_Y,
			.mode=PPA_TRANS_MODE_BLOCKING,
		};
		ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(ppa, &op));
		profile_display_times(convert_end_us-start_us, esp_timer_get_time()-convert_end_us);

		xSemaphoreGive(drawing_mux);
		//do a draw to trigger fb flip
		esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, lcdbuf[cur_buf]);
		cur_buf=cur_buf?0:1;
		int64_t end_us = esp_timer_get_time();
#if 0
		//Shows the maximum FPS possible given the lcd frame drawing code
		printf("LCD Fps: %02f\n", 1000000.0/(end_us-start_us));
#endif
	}
#endif
}

void QG_SetPalette(unsigned char palette[768]) {
	unsigned char *p=palette;
	for (int i=0; i<256; i++) {
		//convert to rgb565
		int b=(*p++)>>3;
		int g=(*p++)>>2;
		int r=(*p++)>>3;
		pal[i]=r+(g<<5)+(b<<11);
		pal888[i]=(palette[i*3]<<16)|(palette[i*3+1]<<8)|palette[i*3+2];
		pal_changed=1;
	}
}

//write a palette entry (rgb565) to an rgb888 framebuffer pixel
static void put_pixel(uint8_t *fb, int x, int y, uint16_t c) {
	uint8_t *p=&fb[(LCD_H_RES*y+x)*LCD_BPP];
	p[0]=(c<<3)&0xf8;		//b
	p[1]=(c>>3)&0xfc;		//g
	p[2]=(c>>8)&0xf8;		//r
}

#define CHAR_W 8
#define CHAR_H 16

static void draw_char(int x, int y, int ch, int fore, int back) {
	//make sure not to draw out of bounds
	if (x<0 || y<0) return;
	if (x+CHAR_W >= LCD_H_RES) return;
	if (y+CHAR_H >= LCD_V_RES) return;
	//write character
	uint8_t *fb=lcdbuf[cur_buf];
	for (int py=0; py<CHAR_H; py++) {
		for (int px=0; px<CHAR_W; px++) {
			int pix=fontdata_8x16[ch*CHAR_H+py]&(1<<px);
			int col=pix?fore:back;
			put_pixel(fb, x+(7-px), y+py, pal[col]);
		}
	}
}

static void draw_end_screen(char *vgamem) {
	memset(lcdbuf[cur_buf], 0, LCD_H_RES*LCD_V_RES*LCD_BPP);

	const unsigned char pal[768]={ //EGA pallette; b, g, r
		0x00, 0x00, 0x00,
		0x00, 0x00, 0xaa,
		0x00, 0xaa, 0x00,
		0x00, 0xaa, 0xaa,
		0xaa, 0x00, 0x00,
		0xaa, 0x00, 0xaa,
		0xaa, 0x55, 0x00,
		0xaa, 0xaa, 0xaa,
		0x55, 0x55, 0x55,
		0x55, 0x55, 0xff,
		0x55, 0xff, 0x55,
		0x55, 0xff, 0xff,
		0xff, 0x55, 0x55,
		0xff, 0x55, 0xff,
		0xff, 0xff, 0x55,
		0xff, 0xff, 0xff
	};
	QG_SetPalette(pal);

	int off_x=(LCD_H_RES-(80*8))/2;
	int off_y=(LCD_V_RES-(25*16))/2;

	if (vgamem) {
		for (int y=0; y<25; y++) {
			for (int x=0; x<80; x++) {
				draw_char(x*CHAR_W+off_x, y*CHAR_H+off_y, vgamem[0], vgamem[1]&0xf, (vgamem[1]>>4)&0x7);
				vgamem+=2;
			}
		}
	}
	esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, lcdbuf[cur_buf]);
}


void display_quit() {
	draw_task_quit=1;
	xSemaphoreGive(drawing_mux);
	vTaskDelay(pdMS_TO_TICKS(30)+1);
	//we can now use the framebuffer to draw the end screen
	unsigned char *d;
	if (registered.value)
		d = COM_LoadHunkFile ("end2.bin"); 
	else
		d = COM_LoadHunkFile ("end1.bin"); 
	draw_end_screen(d);
}


void display_init() {
	//initialize LCD
	const bsp_display_config_t config = {
		.hdmi_resolution = LCD_HDMI_RES,
		.dsi_bus = {
			.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
			.lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
		},
	};

	bsp_display_new(&config, &panel_handle, &io_handle);
	esp_lcd_panel_disp_on_off(panel_handle, true);
	//no backlight on HDMI; the BSP's HDMI backlight pin (GPIO23) is UEXT I2C_SCL2 here

	drawing_mux=xSemaphoreCreateMutex();
	xTaskCreatePinnedToCore(draw_task, "draw", 4096, NULL, 3, &draw_task_handle, 1);
}


