/*
d_zjob.c -- world z-buffer fill on the second core (ESP32-P4 port)

D_DrawSurfaces draws each surface's spans twice: once into the framebuffer and once into
the z-buffer. The z pass only needs the span list and the surface's 1/z gradients, so at
the start of each flush D_DrawSurfaces queues one job per surface here and a worker task
on core 1 fills the z-buffer while core 0 draws the pixels. D_ZJobsWait must be called
before the span lists are reused.
*/

#include "quakedef.h"
#include "d_local.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_ZJOBS		1024
#define ZJOB_CORE		1
#define ZJOB_PRIORITY	6	// above the display conversion task, below audio

typedef struct
{
	espan_t	*spans;
	float	ziorigin, zistepu, zistepv;
} zjob_t;

static zjob_t			zjobs[MAX_ZJOBS];
static volatile int		numzjobs;
static TaskHandle_t		zjob_task;
static TaskHandle_t		zjob_waiter;
static qboolean			zjobs_started;	// only touched by the queueing task

/*
D_DrawZSpansJob

Same as D_DrawZSpans, but with the gradients passed in rather than in globals, so it can
run on the other core.
*/
static void D_DrawZSpansJob (const zjob_t *job)
{
	espan_t			*pspan = job->spans;
	int				count, doublecount, izistep;
	int				izi;
	short			*pdest;
	unsigned		ltemp;
	float			zi;
	float			du, dv;

	izistep = (int)(job->zistepu * 0x8000 * 0x10000);

	do
	{
		pdest = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;

		count = pspan->count;

		du = (float)pspan->u;
		dv = (float)pspan->v;

		zi = job->ziorigin + dv*job->zistepv + du*job->zistepu;
		izi = (int)(zi * 0x8000 * 0x10000);

		if ((intptr_t)pdest & 0x02)
		{
			*pdest++ = (short)(izi >> 16);
			izi += izistep;
			count--;
		}

		if ((doublecount = count >> 1) > 0)
		{
			do
			{
				ltemp = izi >> 16;
				izi += izistep;
				ltemp |= izi & 0xFFFF0000;
				izi += izistep;
				*(int *)pdest = ltemp;
				pdest += 2;
			} while (--doublecount > 0);
		}

		if (count & 1)
			*pdest = (short)(izi >> 16);

	} while ((pspan = pspan->pnext) != NULL);
}

static void D_ZJobTask (void *arg)
{
	int		i;

	for (;;)
	{
		ulTaskNotifyTake (pdTRUE, portMAX_DELAY);
		for (i = 0 ; i < numzjobs ; i++)
			D_DrawZSpansJob (&zjobs[i]);
		xTaskNotifyGive (zjob_waiter);	// exactly once per D_ZJobsStart
	}
}

static void D_ZJobsInit (void)
{
	xTaskCreatePinnedToCore (D_ZJobTask, "zjobs", 4096, NULL, ZJOB_PRIORITY, &zjob_task, ZJOB_CORE);
}

/*
D_ZJobAdd

Queue the z fill of one surface's spans. Returns false if the queue is full, in which case
the caller draws the z spans itself.
*/
qboolean D_ZJobAdd (espan_t *spans, float ziorigin, float zistepu, float zistepv)
{
	zjob_t	*job;

	if (numzjobs >= MAX_ZJOBS)
		return false;
	job = &zjobs[numzjobs++];
	job->spans = spans;
	job->ziorigin = ziorigin;
	job->zistepu = zistepu;
	job->zistepv = zistepv;
	return true;
}

void D_ZJobsStart (void)
{
	if (!zjob_task)
		D_ZJobsInit ();
	if (!numzjobs)
		return;
	zjob_waiter = xTaskGetCurrentTaskHandle ();
	zjobs_started = true;
	xTaskNotifyGive (zjob_task);
}

void D_ZJobsWait (void)
{
	if (zjobs_started)
	{
		ulTaskNotifyTake (pdTRUE, portMAX_DELAY);
		zjobs_started = false;
	}
	numzjobs = 0;
}
