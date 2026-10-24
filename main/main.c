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
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "sd_protocol_defs.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "quakegeneric.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "quakekeys.h"
#include "eth_connect.h"
#include "font_8x16.h"
#include "input.h"
#include "display.h"
#include "profile.h"

#include "quakedef.h"

void QG_Init(void) {
}

void QG_Quit(void) {
	display_quit();
	while(1) vTaskDelay(100);
}


static void quake_task(void *param) {
	char *argv[]={
		"quake", 
		"-basedir", "/sdcard/",
		NULL
	};
	//initialize Quake
	QG_Create(3, argv);

	int64_t oldtime_us = esp_timer_get_time();
	while (1) {
		// Run the frame at the correct duration.
		int64_t newtime_us = esp_timer_get_time();
		QG_Tick((double)(newtime_us - oldtime_us)/1000000.0);
		oldtime_us = newtime_us;
	}
}

//Olimex ESP32-P4-PC board specifics (pins from the Rev.C schematic)
#define SD_PWR_EN_GPIO	45	//SD1_PWR_EN, P-MOS high-side switch, active low
#define HUB_RST_GPIO	21	//HUB_RST# of the FE1.1s USB hub, active low

static void board_init(void) {
	gpio_config_t io={
		.pin_bit_mask=(1ULL<<SD_PWR_EN_GPIO)|(1ULL<<HUB_RST_GPIO),
		.mode=GPIO_MODE_OUTPUT,
	};
	gpio_set_level(SD_PWR_EN_GPIO, 0);	//SD card power on
	gpio_set_level(HUB_RST_GPIO, 0);	//hold USB hub in reset...
	ESP_ERROR_CHECK(gpio_config(&io));
	vTaskDelay(pdMS_TO_TICKS(20));
	gpio_set_level(HUB_RST_GPIO, 1);	//...then release it
	vTaskDelay(pdMS_TO_TICKS(100));		//let SD power and the hub settle
}

static void sdcard_mount(void) {
	//The BSP default is 40MHz; Olimex validated the SD slot on this board at 20MHz
	sdmmc_host_t host;
	bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);
	host.max_freq_khz=SDMMC_FREQ_DEFAULT;
	bsp_sdcard_cfg_t cfg={
		.host=&host,
	};
	esp_err_t err=bsp_sdcard_sdmmc_mount(&cfg);
	if (err!=ESP_OK) printf("SD card mount failed: %s\n", esp_err_to_name(err));
}

void app_main() {
	board_init();
	profile_init();
	sdcard_mount();
	display_init();

	ethernet_connect();

	input_init();

	//The renderer's big edge/surface/span arrays are static (internal SRAM), so the
	//stack is small and can live in internal SRAM too.
	int stack_depth=32*1024;

	StaticTask_t *taskbuf=calloc(1, sizeof(StaticTask_t));
	uint8_t *stackbuf=heap_caps_calloc(stack_depth, 1, MALLOC_CAP_INTERNAL);
	xTaskCreateStaticPinnedToCore(quake_task, "quake", stack_depth, NULL, 2, (StackType_t*)stackbuf, taskbuf, 0);
}


