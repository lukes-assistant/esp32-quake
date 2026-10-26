#pragma once
#include <stdint.h>

void profile_init();
//called by the display code once per frame handed over by the game
void profile_frame();
//true if the frame being presented should be dumped as a screenshot (CONFIG_QUAKE_SCREENSHOTS)
int profile_screenshot_due();
//display pipeline timings (draw task on core 1), in microseconds
void profile_display_times(int64_t convert_us, int64_t scale_us);
//time the game task spent waiting for the draw task to release the frame
void profile_drawframe_wait(int64_t wait_us);
