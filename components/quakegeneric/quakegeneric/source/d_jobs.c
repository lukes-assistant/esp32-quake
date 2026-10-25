/*
d_jobs.c -- share world surface drawing with the second core (ESP32-P4 port)

While R_ScanEdges scans a frame, each span flush (D_DrawSurfaces) turns every surface with
spans into a job instead of drawing it:
- normal textured surfaces: build the surface-cache block if needed (r_drawsurf setup
  captured by D_CacheSurfaceDeferred), draw the textured spans, fill the z-buffer;
- sky, water and background surfaces are drawn by core 0 during the walk; their job only
  fills the z-buffer.
The gradients each job needs are captured when it is queued, so jobs are independent and
can run on either core in any order. Each core builds surface-cache blocks with its own copy
of the surface code (r_surf_c1.c is a renamed copy of r_surf.c for core 1).

Jobs are published to a queue that lasts the whole frame. A worker task on core 1 runs them
as they are published and sleeps when there is nothing to do. R_ScanEdges alternates between
two span buffers, so core 0 keeps scanning while the jobs of the previous flush run; before
reusing a span buffer it waits (running jobs itself meanwhile) for the jobs that use it.
D_JobsEndFrame runs/waits for everything, since the entity pass needs the complete z-buffer.

The job versions of D_DrawZSpans and D_DrawSpans8 do the same arithmetic as the originals,
with the gradients passed in instead of read from globals.
*/

#include "quakedef.h"
#include "d_local.h"
#include "r_local.h"
#include "r_pipe.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern drawsurf_t	r_drawsurf_c1;
void R_DrawSurface_c1 (void);

#define MAX_JOBS		1600	// peak demand 1493 per frame (timedemo demo1 at 800x600)
#define MAX_RECS		448		// one per surface drawn (NUMSTACKSURFACES)
#define MAX_BUILDS		160		// peak 107 per frame (timedemo demo1 at 800x600)
#define JOBS_CORE		1
#define JOBS_PRIORITY	6	// above the display conversion task, below audio

// What a surface's jobs share within a frame: its gradients and surface-cache block. A surface
// gets a job per span flush (every band), so this is stored once per frame and the jobs point
// to it.
typedef struct
{
	float		ziorigin, zistepu, zistepv;
	pixel_t		*cacheblock;	// NULL: z only
	float		sdivzorigin, sdivzstepu, sdivzstepv;
	float		tdivzorigin, tdivzstepu, tdivzstepv;
	fixed16_t	sadjust, tadjust, bbextents, bbextentt;
	int16_t		cachewidth;
	int16_t		surf;			// index in surfaces[] (the record's key this frame)
} jobrec_t;

typedef struct
{
	espan_t		*spans;
	pixel_t		*viewbuffer;	// d_viewbuffer when queued (a band buffer, r_edge.c)
	int16_t		rec;			// index into recs[]
	int16_t		waitjob;		// >= 0: the job building the surface-cache block, wait for it
	int16_t		build_idx;		// >= 0: index into build_ds[] (build surface-cache block first)
	uint8_t		color;			// draw textured spans (else z only)
	uint8_t		pad;
} job_t;

static job_t					jobs[MAX_JOBS];
static jobrec_t					recs[MAX_RECS];
static int16_t					recof[NUMSTACKSURFACES];	// surface -> its record (valid if it points back)
static int						numrecs;		// written by core 0 only
static drawsurf_t				build_ds[MAX_BUILDS];
static uint32_t					jobdone[MAX_JOBS];	// = frame generation when finished

static int		numjobs;		// published; written by core 0 only
static int		numbuilds;		// written by core 0 only
static int		nextjob;		// claimed by either core
static int		jobs_closed;	// core 0 has published the frame's last job
static uint32_t	frame_gen;		// incremented per frame by core 0
static uint32_t	done_gen;		// frame generation the worker has finished
static int		worker_waiting;
static qboolean	jobs_active;	// a frame is in progress (core 0 only)

int		d_peak_jobs, d_peak_builds;	// per-frame peaks, for sizing (BENCH report)
int		d_peak_demand;				// jobs + refused (queue full) in one frame
int		d_peak_recs;
int		d_frames_full, d_refused_total, d_stat_frames;
static int	refused;					// this frame

static TaskHandle_t		worker_task;
static TaskHandle_t		frame_task;	// core 0's task, notified when the worker is done

/*
D_DrawZSpansJob

D_DrawZSpans with the gradients passed in.
*/
static void D_DrawZSpansJob (const job_t *job, const jobrec_t *rec)
{
	espan_t			*pspan = job->spans;
	int				count, doublecount, izistep;
	int				izi, u, v, u0, u1;
	short			*pdest;
	unsigned		ltemp;
	float			zi;
	float			du, dv;

	izistep = (int)(rec->zistepu * 0x8000 * 0x10000);

	do
	{
		v = pspan->v;
		u0 = d_zrowmin[v];
		u1 = d_zrowmax[v];
		if (u0 > u1)
			continue;

		u = pspan->u;
		count = pspan->count;
		if (u + count - 1 > u1)
			count = u1 - u + 1;
		if (u + count <= u0 || count <= 0)
			continue;

		du = (float)u;
		dv = (float)v;

		zi = rec->ziorigin + dv*rec->zistepv + du*rec->zistepu;
		izi = (int)(zi * 0x8000 * 0x10000);

		if (u < u0)
		{
			int skip = u0 - u;
			u = u0;
			count -= skip;
			izi += skip * izistep;
		}

		pdest = d_pzbuffer + (d_zwidth * v) + u;

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
// pixels between perspective-correct divisions (the original Quake used 16 with d_subdiv16)
#define SPAN_SUBDIV			16
#define SPAN_SUBDIV_SHIFT	4

static void D_DrawSpans8Job (const job_t *job, const jobrec_t *rec)
{
	espan_t			*pspan = job->spans;
	int				count, spancount;
	const unsigned char	* restrict pbase;
	unsigned char	* restrict pdest;
	fixed16_t		s, t, snext, tnext, sstep, tstep;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz8stepu, tdivz8stepu, zi8stepu;
	const float		k65536 = (float)0x10000;
	const int		cachewidth = rec->cachewidth;
	const fixed16_t	sadjust = rec->sadjust, tadjust = rec->tadjust;
	const fixed16_t	bbextents = rec->bbextents, bbextentt = rec->bbextentt;

	pbase = (const unsigned char *)rec->cacheblock;

	sdivz8stepu = rec->sdivzstepu * (float)SPAN_SUBDIV;
	tdivz8stepu = rec->tdivzstepu * (float)SPAN_SUBDIV;
	zi8stepu = rec->zistepu * (float)SPAN_SUBDIV;

	do
	{
		pdest = (unsigned char *)((byte *)job->viewbuffer +
				(screenwidth * pspan->v) + pspan->u);

		count = pspan->count;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = rec->sdivzorigin + dv*rec->sdivzstepv + du*rec->sdivzstepu;
		tdivz = rec->tdivzorigin + dv*rec->tdivzstepv + du*rec->tdivzstepu;
		zi = rec->ziorigin + dv*rec->zistepv + du*rec->zistepu;
		z = k65536 / zi;	// prescale to 16.16 fixed-point

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

		if (count > SPAN_SUBDIV)
		{
			sdivz += sdivz8stepu;
			tdivz += tdivz8stepu;
			zi += zi8stepu;
			z = k65536 / zi;

			for (;;)
			{
				float z_next;
				int k;

				count -= SPAN_SUBDIV;

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;

				sstep = (snext - s) >> SPAN_SUBDIV_SHIFT;
				tstep = (tnext - t) >> SPAN_SUBDIV_SHIFT;

				if (count > SPAN_SUBDIV)
				{
					sdivz += sdivz8stepu;
					tdivz += tdivz8stepu;
					zi += zi8stepu;
					__asm__ volatile ("fdiv.s %0, %1, %2" : "=f"(z_next) : "f"(k65536), "f"(zi));
				}
				else if (count > 1)
				{
					spancountminus1 = (float)(count - 1);
					sdivz += rec->sdivzstepu * spancountminus1;
					tdivz += rec->tdivzstepu * spancountminus1;
					zi += rec->zistepu * spancountminus1;
					__asm__ volatile ("fdiv.s %0, %1, %2" : "=f"(z_next) : "f"(k65536), "f"(zi));
				}

				#pragma GCC unroll 16
				for (k = 0 ; k < SPAN_SUBDIV ; k += 2)
				{
					unsigned char p0, p1;

					p0 = pbase[(s >> 16) + (t >> 16) * cachewidth];
					s += sstep; t += tstep;
					p1 = pbase[(s >> 16) + (t >> 16) * cachewidth];
					s += sstep; t += tstep;
					pdest[k] = p0;
					pdest[k + 1] = p1;
				}
				pdest += SPAN_SUBDIV;

				s = snext;
				t = tnext;

				z = z_next;
				if (count <= SPAN_SUBDIV)
					break;
			}

			if (count == 1)
			{
				*pdest = pbase[(s >> 16) + (t >> 16) * cachewidth];
				continue;
			}
		}
		else
		{
			if (count == 1)
			{
				*pdest = pbase[(s >> 16) + (t >> 16) * cachewidth];
				continue;
			}
			spancountminus1 = (float)(count - 1);
			sdivz += rec->sdivzstepu * spancountminus1;
			tdivz += rec->tdivzstepu * spancountminus1;
			zi += rec->zistepu * spancountminus1;
			z = k65536 / zi;
		}

		snext = (int)(sdivz * z) + sadjust;
		if (snext > bbextents)
			snext = bbextents;
		else if (snext < 8)
			snext = 8;

		tnext = (int)(tdivz * z) + tadjust;
		if (tnext > bbextentt)
			tnext = bbextentt;
		else if (tnext < 8)
			tnext = 8;

		spancount = count;
		sstep = (snext - s) / (spancount - 1);
		tstep = (tnext - t) / (spancount - 1);

		#pragma GCC unroll 0
		do
		{
			*pdest++ = pbase[(s >> 16) + (t >> 16) * cachewidth];
			s += sstep;
			t += tstep;
		} while (--spancount > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

static void D_RunJob (job_t *job, int core)
{
	const jobrec_t	*rec = &recs[job->rec];

	if (job->build_idx >= 0)
	{
		if (core)
		{
			r_drawsurf_c1 = build_ds[job->build_idx];
			R_DrawSurface_c1 ();
		}
		else
		{
			r_drawsurf = build_ds[job->build_idx];
			R_DrawSurface ();
		}
	}
	if (job->color)
	{
		// the block may still be being built by an earlier job (a surface spans several
		// flushes); that job was claimed before this one, so it is running or done
		if (job->waitjob >= 0)
			while (__atomic_load_n (&jobdone[job->waitjob], __ATOMIC_ACQUIRE) != frame_gen)
				;
		D_DrawSpans8Job (job, rec);
	}
	D_DrawZSpansJob (job, rec);
}

/*
D_WorkerSleepUnless

Core 1 sleeps until core 0 publishes jobs, closes the queue or starts a frame. worker_waiting
is set before the condition is checked one last time, so anything core 0 does after that
check sends a notification (no lost wakeups); stale notifications only cause an extra check
by the caller.
*/
static void D_WorkerSleepUnless (qboolean (*ready) (void *), void *arg)
{
	__atomic_store_n (&worker_waiting, 1, __ATOMIC_SEQ_CST);
	if (!ready (arg))
		ulTaskNotifyTake (pdTRUE, portMAX_DELAY);
	__atomic_store_n (&worker_waiting, 0, __ATOMIC_RELEASE);
}

static void D_WakeWorker (void)
{
	if (__atomic_load_n (&worker_waiting, __ATOMIC_SEQ_CST))
		xTaskNotifyGive (worker_task);
}

static qboolean D_JobPublishedOrClosed (void *arg)
{
	int		i = *(int *)arg;

	return i < __atomic_load_n (&numjobs, __ATOMIC_SEQ_CST) ||
		__atomic_load_n (&jobs_closed, __ATOMIC_SEQ_CST);
}

static qboolean D_NewFrame (void *arg)
{
	return __atomic_load_n (&frame_gen, __ATOMIC_SEQ_CST) != *(uint32_t *)arg;
}

static qboolean D_NewFrameOrPipe (void *arg)
{
	return D_NewFrame (arg) || R_PipePending ();
}

// core 0 has handed over a frame to finish (r_pipe.c)
void D_JobsWakeWorker (void)
{
	D_WakeWorker ();
}

/*
D_RunJobs

Claim and run jobs of the current frame until the queue is closed and every job has been
claimed. Core 1 sleeps while it waits for a claimed job to be published; core 0 spins (it
only waits after closing the queue, i.e. never for long).
*/
static void D_RunJobs (int core, uint32_t gen)
{
	int		i;

	for (;;)
	{
		i = __atomic_fetch_add (&nextjob, 1, __ATOMIC_ACQ_REL);
		while (i >= __atomic_load_n (&numjobs, __ATOMIC_ACQUIRE))
		{
			if (__atomic_load_n (&jobs_closed, __ATOMIC_ACQUIRE) &&
				i >= __atomic_load_n (&numjobs, __ATOMIC_ACQUIRE))
				return;
			if (core)
				D_WorkerSleepUnless (D_JobPublishedOrClosed, &i);
		}
		D_RunJob (&jobs[i], core);
		__atomic_store_n (&jobdone[i], gen, __ATOMIC_RELEASE);
	}
}

static void D_WorkerTask (void *arg)
{
	uint32_t	my_gen = 0;

	for (;;)
	{
		while (!D_NewFrameOrPipe (&my_gen))
			D_WorkerSleepUnless (D_NewFrameOrPipe, &my_gen);
		if (R_PipePending ())
		{
			R_PipeRun ();	// finish the previous frame (r_pipe.c)
			continue;
		}
		my_gen = __atomic_load_n (&frame_gen, __ATOMIC_ACQUIRE);

		D_RunJobs (1, my_gen);

		__atomic_store_n (&done_gen, my_gen, __ATOMIC_RELEASE);
		xTaskNotifyGive (frame_task);
	}
}

void D_JobsBeginFrame (void)
{
	if (!worker_task)
	{
		// a static stack in internal SRAM: the heap could hand out slow memory (with little
		// internal RAM left, a heap-allocated stack once landed in LP RAM: 2.6x slower)
		static StackType_t	worker_stack[4096 / sizeof(StackType_t)];	// peak use ~2.4 KB
		static StaticTask_t	worker_tcb;

		worker_task = xTaskCreateStaticPinnedToCore (D_WorkerTask, "d_jobs", sizeof(worker_stack), NULL,
				JOBS_PRIORITY, worker_stack, &worker_tcb, JOBS_CORE);
	}
	frame_task = xTaskGetCurrentTaskHandle ();
	numjobs = 0;
	numbuilds = 0;
	numrecs = 0;
	nextjob = 0;
	jobs_closed = 0;
	jobs_active = true;
	__atomic_add_fetch (&frame_gen, 1, __ATOMIC_SEQ_CST);
	D_WakeWorker ();
}

int D_JobsCount (void)
{
	return numjobs;
}

/*
D_JobAdd

Queue a surface: z fill always; if color, also the textured spans with the gradients that
D_CalcGradients/D_DrawSurfaces have just set up, building the surface-cache block described
by *ds first if needbuild. Returns false if no frame is active or the queue is full, in
which case the caller draws the surface itself.
*/
qboolean D_JobAdd (espan_t *spans, float ziorigin, float zistepu, float zistepv,
		qboolean color, qboolean needbuild, const drawsurf_t *ds, int waitjob, int surf)
{
	job_t		*job;
	jobrec_t	*rec;
	int			r;

	if (!jobs_active)
		return false;
	if (numjobs >= MAX_JOBS || (needbuild && numbuilds >= MAX_BUILDS))
	{
		refused++;
		return false;
	}

// the surface's record from an earlier flush this frame, if it still matches
	r = -1;
	if (surf >= 0 && surf < NUMSTACKSURFACES)
	{
		r = recof[surf];
		if (r < 0 || r >= numrecs || recs[r].surf != surf || recs[r].ziorigin != ziorigin ||
			(recs[r].cacheblock != NULL) != (color != 0) ||
			(color && (recs[r].cacheblock != cacheblock || recs[r].sdivzorigin != d_sdivzorigin)))
			r = -1;
	}
	if (r < 0)
	{
		if (numrecs >= MAX_RECS)
		{
			refused++;
			return false;
		}
		r = numrecs++;
		rec = &recs[r];
		rec->ziorigin = ziorigin;
		rec->zistepu = zistepu;
		rec->zistepv = zistepv;
		rec->surf = (int16_t)surf;
		rec->cacheblock = NULL;
		if (color)
		{
			rec->cacheblock = cacheblock;
			rec->cachewidth = (int16_t)cachewidth;
			rec->sdivzorigin = d_sdivzorigin;
			rec->sdivzstepu = d_sdivzstepu;
			rec->sdivzstepv = d_sdivzstepv;
			rec->tdivzorigin = d_tdivzorigin;
			rec->tdivzstepu = d_tdivzstepu;
			rec->tdivzstepv = d_tdivzstepv;
			rec->sadjust = sadjust;
			rec->tadjust = tadjust;
			rec->bbextents = bbextents;
			rec->bbextentt = bbextentt;
		}
		if (surf >= 0 && surf < NUMSTACKSURFACES)
			recof[surf] = (int16_t)r;
	}

	job = &jobs[numjobs];
	job->spans = spans;
	job->viewbuffer = d_viewbuffer;
	job->rec = (int16_t)r;
	job->color = color ? 1 : 0;
	job->waitjob = (int16_t)waitjob;
	if (needbuild)
	{
		job->build_idx = (int16_t)numbuilds;
		build_ds[numbuilds++] = *ds;
	}
	else
	{
		job->build_idx = -1;
	}
	__atomic_store_n (&numjobs, numjobs + 1, __ATOMIC_SEQ_CST);	// publish
	return true;
}

/*
D_JobsPublished

Called after a flush has queued its jobs: wake core 1 if it's asleep.
*/
void D_JobsPublished (void)
{
	if (jobs_active)
		D_WakeWorker ();
}

/*
D_JobsWaitRange

Wait until jobs [first, last) of this frame are finished, running jobs meanwhile.
*/
void D_JobsWaitRange (int first, int last)
{
	uint32_t	gen = frame_gen;
	int			j, i;

// core 1 may be asleep holding a claim on a job that was published after it went to sleep
// (it is only woken at the end of a flush); wake it, or we could wait for that job forever
	D_WakeWorker ();

	for (j = first ; j < last ; j++)
	{
		while (__atomic_load_n (&jobdone[j], __ATOMIC_ACQUIRE) != gen)
		{
		// help: run an unclaimed published job, if any
			i = __atomic_load_n (&nextjob, __ATOMIC_ACQUIRE);
			if (i < __atomic_load_n (&numjobs, __ATOMIC_ACQUIRE) &&
				__atomic_compare_exchange_n (&nextjob, &i, i + 1, false,
					__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			{
				D_RunJob (&jobs[i], 0);
				__atomic_store_n (&jobdone[i], gen, __ATOMIC_RELEASE);
			}
		}
	}
}

/*
D_JobsEndFrame

Close the queue, run the remaining jobs and wait for core 1, so the z-buffer and the
framebuffer are complete.
*/
void D_JobsEndFrame (void)
{
	uint32_t	gen = frame_gen;

	if (!jobs_active)
		return;
	__atomic_store_n (&jobs_closed, 1, __ATOMIC_SEQ_CST);
	D_WakeWorker ();
	if (numjobs > d_peak_jobs)
		d_peak_jobs = numjobs;
	if (numjobs + refused > d_peak_demand)
		d_peak_demand = numjobs + refused;
	d_stat_frames++;
	if (refused)
	{
		d_frames_full++;
		d_refused_total += refused;
	}
	refused = 0;
	if (numbuilds > d_peak_builds)
		d_peak_builds = numbuilds;
	if (numrecs > d_peak_recs)
		d_peak_recs = numrecs;
	D_RunJobs (0, gen);
	while (__atomic_load_n (&done_gen, __ATOMIC_ACQUIRE) != gen)
		ulTaskNotifyTake (pdTRUE, pdMS_TO_TICKS (10));
	jobs_active = false;
}

/*
D_JobsWaitAll

Wait (running jobs meanwhile) until every job published so far this frame has finished.
*/
void D_JobsWaitAll (void)
{
	if (jobs_active)
		D_JobsWaitRange (0, numjobs);
}


/*
D_JobsDebugState

One-line snapshot of the queue for the stall watchdog (main/stability.c); racy by design.
*/
void D_JobsDebugState (char *buf, int size)
{
	snprintf (buf, size, "jobs %d/%d next %d closed %d active %d gen %lu done %lu wwait %d",
		numjobs, MAX_JOBS, nextjob, jobs_closed, jobs_active, (unsigned long)frame_gen,
		(unsigned long)done_gen, worker_waiting);
}
