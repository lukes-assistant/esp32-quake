/*
r_alias_c1.c -- core-1 copy of r_alias.c, plus the core-1 side of the split alias pass
(ESP32-P4 port, see alias_c1_names.h and R_DrawEntitiesAndViewModel in r_main.c)
*/
#include "alias_c1_names.h"
#include "r_alias.c"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// core 1's instances of the shared state the alias code writes
entity_t	*currententity;
vec3_t		modelorg;
refdef_t	r_refdef;
int			ubasestep, errorterm, erroradjustup, erroradjustdown;

// Core 0 has already loaded every model this frame (Mod_Extradata in the prepass), so core 1
// only reads the cached pointer and leaves the cache's LRU list alone.
void *Mod_Extradata (model_t *mod)
{
	return mod->cache.data;
}

#define ALIAS_C1_CORE		1
#define ALIAS_C1_PRIORITY	6

static TaskHandle_t		alias_task;
static TaskHandle_t		alias_waiter;
static aliasitem_t		*alias_items;
static int				alias_count;
static int				alias_done;	// set by core 1 when the list is drawn

static void R_AliasC1Task (void *arg)
{
	for (;;)
	{
		ulTaskNotifyTake (pdTRUE, portMAX_DELAY);
		R_DrawAliasItems (alias_items, alias_count);	// = R_DrawAliasItems_c1
		__atomic_store_n (&alias_done, 1, __ATOMIC_RELEASE);
		xTaskNotifyGive (alias_waiter);
	}
}

#undef r_refdef
void R_AliasC1Start (aliasitem_t *items, int count, const refdef_t *refdef)
{
	if (!alias_task)
		xTaskCreatePinnedToCore (R_AliasC1Task, "alias_c1", 4096, NULL, ALIAS_C1_PRIORITY,
				&alias_task, ALIAS_C1_CORE);
	r_refdef_c1 = *refdef;
	alias_items = items;
	alias_count = count;
	alias_waiter = xTaskGetCurrentTaskHandle ();
	__atomic_store_n (&alias_done, 0, __ATOMIC_RELEASE);
	xTaskNotifyGive (alias_task);
}

// The waiting task also gets notifications from the world job worker (d_jobs.c), so a
// notification alone doesn't mean core 1 is done: check the flag.
void R_AliasC1Wait (void)
{
	while (!__atomic_load_n (&alias_done, __ATOMIC_ACQUIRE))
		ulTaskNotifyTake (pdTRUE, 1);
}
