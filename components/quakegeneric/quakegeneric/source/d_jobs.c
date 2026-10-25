/*
d_jobs.c -- share world span drawing with the second core (ESP32-P4 port)

Each D_DrawSurfaces flush works in two phases:

1. Core 0 walks the surfaces as before: it builds the surface caches and texture gradients,
   but instead of drawing the spans of normal textured surfaces it snapshots the gradients
   into a "colour job". Meanwhile a worker task on core 1 fills the z-buffer for every
   surface ("z jobs", queued before the walk starts).
2. Both cores draw the colour jobs, taking them from a shared queue.

D_JobsFinish waits for core 1 before the span lists are reused. The job versions of
D_DrawZSpans and D_DrawSpans8 do the same arithmetic as the originals, with the gradients
passed in instead of read from globals.
*/

#include "quakedef.h"
#include "d_local.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_ZJOBS		1024
#define MAX_COLORJOBS	1024
#define JOBS_CORE		1
#define JOBS_PRIORITY	6	// above the display conversion task, below audio

typedef struct
{
	espan_t	*spans;
	float	ziorigin, zistepu, zistepv;
} zjob_t;

typedef struct
{
	espan_t		*spans;
	pixel_t		*cacheblock;
	int			cachewidth;
	float		sdivzorigin, sdivzstepu, sdivzstepv;
	float		tdivzorigin, tdivzstepu, tdivzstepv;
	float		ziorigin, zistepu, zistepv;
	fixed16_t	sadjust, tadjust, bbextents, bbextentt;
} colorjob_t;

static zjob_t							zjobs[MAX_ZJOBS];
static int								numzjobs;
static EXT_RAM_BSS_ATTR colorjob_t		colorjobs[MAX_COLORJOBS];
static int								numcolorjobs;
static int								nextcolorjob;	// claimed atomically by both cores

static TaskHandle_t		jobs_task;
static TaskHandle_t		jobs_waiter;
static qboolean			jobs_started;	// only touched by the queueing task

/*
D_DrawZSpansJob

D_DrawZSpans with the gradients passed in.
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

/*
D_DrawSpans8Job

D_DrawSpans8 with the gradients passed in.
*/
static void D_DrawSpans8Job (const colorjob_t *job)
{
	espan_t			*pspan = job->spans;
	int				count, spancount;
	unsigned char	*pbase, *pdest;
	fixed16_t		s, t, snext, tnext, sstep, tstep;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz8stepu, tdivz8stepu, zi8stepu;
	const int		cachewidth = job->cachewidth;
	const fixed16_t	sadjust = job->sadjust, tadjust = job->tadjust;
	const fixed16_t	bbextents = job->bbextents, bbextentt = job->bbextentt;

	sstep = 0;	// keep compiler happy
	tstep = 0;	// ditto

	pbase = (unsigned char *)job->cacheblock;

	sdivz8stepu = job->sdivzstepu * 8;
	tdivz8stepu = job->tdivzstepu * 8;
	zi8stepu = job->zistepu * 8;

	do
	{
		pdest = (unsigned char *)((byte *)d_viewbuffer +
				(screenwidth * pspan->v) + pspan->u);

		count = pspan->count;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = job->sdivzorigin + dv*job->sdivzstepv + du*job->sdivzstepu;
		tdivz = job->tdivzorigin + dv*job->tdivzstepv + du*job->tdivzstepu;
		zi = job->ziorigin + dv*job->zistepv + du*job->zistepu;
		z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
		// calculate s and t at the far end of the span
			if (count >= 8)
				spancount = 8;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
			// calculate s/z, t/z, zi->fixed s and t at far end of span,
			// calculate s and t steps across span by shifting
				sdivz += sdivz8stepu;
				tdivz += tdivz8stepu;
				zi += zi8stepu;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;	// guard against round-off error on <0 steps

				sstep = (snext - s) >> 3;
				tstep = (tnext - t) >> 3;
			}
			else
			{
			// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
			// can't step off polygon), clamp, calculate s and t steps across
			// span by division, biasing steps low so we don't run off the
			// texture
				spancountminus1 = (float)(spancount - 1);
				sdivz += job->sdivzstepu * spancountminus1;
				tdivz += job->tdivzstepu * spancountminus1;
				zi += job->zistepu * spancountminus1;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;	// guard against round-off error on <0 steps

				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			do
			{
				*pdest++ = *(pbase + (s >> 16) + (t >> 16) * cachewidth);
				s += sstep;
				t += tstep;
			} while (--spancount > 0);

			s = snext;
			t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

static void D_DrawColorJobs (void)
{
	int		i;

	while ((i = __atomic_fetch_add (&nextcolorjob, 1, __ATOMIC_ACQ_REL)) < numcolorjobs)
		D_DrawSpans8Job (&colorjobs[i]);
}

static void D_JobsTask (void *arg)
{
	int		i;

	for (;;)
	{
	// phase 1: z-buffer fill
		ulTaskNotifyTake (pdFALSE, portMAX_DELAY);	// one notification at a time: two can be pending
		for (i = 0 ; i < numzjobs ; i++)
			D_DrawZSpansJob (&zjobs[i]);

	// phase 2: share the colour jobs with core 0
		ulTaskNotifyTake (pdFALSE, portMAX_DELAY);	// one notification at a time: two can be pending
		D_DrawColorJobs ();

		xTaskNotifyGive (jobs_waiter);	// exactly once per D_JobsStart
	}
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

/*
D_ColorJobAdd

Queue the textured spans of the current surface, using the gradients that D_CalcGradients
and D_DrawSurfaces have just set up. Returns false if the queue is full, in which case the
caller draws the spans itself.
*/
qboolean D_ColorJobAdd (espan_t *spans)
{
	colorjob_t	*job;

	if (!jobs_started || numcolorjobs >= MAX_COLORJOBS)
		return false;
	job = &colorjobs[numcolorjobs++];
	job->spans = spans;
	job->cacheblock = cacheblock;
	job->cachewidth = cachewidth;
	job->sdivzorigin = d_sdivzorigin;
	job->sdivzstepu = d_sdivzstepu;
	job->sdivzstepv = d_sdivzstepv;
	job->tdivzorigin = d_tdivzorigin;
	job->tdivzstepu = d_tdivzstepu;
	job->tdivzstepv = d_tdivzstepv;
	job->ziorigin = d_ziorigin;
	job->zistepu = d_zistepu;
	job->zistepv = d_zistepv;
	job->sadjust = sadjust;
	job->tadjust = tadjust;
	job->bbextents = bbextents;
	job->bbextentt = bbextentt;
	return true;
}

/*
D_JobsStart

Start phase 1: core 1 fills the z-buffer for the queued z jobs.
*/
void D_JobsStart (void)
{
	if (!jobs_task)
		xTaskCreatePinnedToCore (D_JobsTask, "d_jobs", 4096, NULL, JOBS_PRIORITY, &jobs_task, JOBS_CORE);
	numcolorjobs = 0;
	nextcolorjob = 0;
	jobs_waiter = xTaskGetCurrentTaskHandle ();
	jobs_started = true;
	xTaskNotifyGive (jobs_task);
}

/*
D_JobsFinish

Phase 2: draw the colour jobs on both cores, then wait for core 1.
*/
void D_JobsFinish (void)
{
	if (!jobs_started)
		return;
	__atomic_thread_fence (__ATOMIC_RELEASE);
	xTaskNotifyGive (jobs_task);
	D_DrawColorJobs ();
	ulTaskNotifyTake (pdFALSE, portMAX_DELAY);	// one notification at a time: two can be pending
	jobs_started = false;
	numzjobs = 0;
}
