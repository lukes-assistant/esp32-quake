#pragma once
#include <stdint.h>

// Stability diagnostics (CONFIG_QUAKE_STABILITY_DEBUG, see stability.c). The calls are
// no-ops when the option is off.

//print the reset reason and what the previous run was doing when it ended
void stability_boot_report(void);
//start the stall watchdog (a task on core 1)
void stability_init(void);
//core 0: once per game tick
void stability_game_tick(void);
//once per frame handed to the display (QG_DrawFrame)
void stability_present(void);
//draw task: check a converted frame for being (nearly) all black
void stability_check_frame(const uint8_t *pixels, const uint32_t *pal888);
