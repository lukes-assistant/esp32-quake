/*
r_pipe.c -- frame pipelining between the cores (ESP32-P4 port)

Core 0 runs the game and renders the world of a frame (with core 1 helping through the job
queue, d_jobs.c). The rest of the frame -- alias models, particles, the 2D overlay and handing
the frame to the display -- doesn't feed back into the game, so instead of drawing it, core 0
records it and goes on with the next frame while core 1 draws it:

	core 0:  game N | world N        | record N | game N+1 | world N+1      | record N+1
	core 1:  ...    | world jobs N   | finish N-1 ...      | world jobs N+1 | finish N

Frame buffers: one is being finished by core 1, one is being rendered by core 0 and one
is being shown, so there are three (vid_null.c). The z-buffer alternates between two, since
core 1's models of frame N read it while core 0 writes the world's of frame N+1.

What core 1 reads is either copied into the frame record (entities, alias lighting,
particles, translation tables, palette, view state) or doesn't change while a frame is
pending: model and pic data live in the cache/hunk, and every Cache/Hunk allocation or
release first waits for the pending frame (R_PipeSync, zone.c).

Frames that can't be deferred (underwater warp, sprites) draw their models and particles
on core 0 as before (after waiting for the pending frame); their 2D is still recorded.
*/

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"
#include "r_pipe.h"
#include "quakegeneric.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_DRAWCMDS		8192
#define MAX_TRANSLATIONS	16
#define MAX_PIPE_PARTICLES	2048	// MAX_PARTICLES in r_part.c

typedef struct
{
	vec3_t	org;
	int		color;
} pipeparticle_t;

// what the core-1 drawing code reads (view_c1_names.h)
typedef struct
{
	vec3_t		r_origin, vpn, vright, vup, r_pright, r_pup, r_ppn;
	float		xcenter, ycenter, xscale, yscale;
	float		aliasxcenter, aliasycenter, aliasxscale, aliasyscale;
	float		r_aliastransition, r_resfudge;
	qboolean	r_recursiveaffinetriangles;
	unsigned	d_zwidth;
	int			screenwidth;
	int			d_vrectx, d_vrecty, d_vrectright_particle, d_vrectbottom_particle;
	int			d_pix_min, d_pix_max, d_pix_shift, d_y_aspect_shift;
	short		*d_pzbuffer;
	pixel_t		*d_viewbuffer;
	float		time;
	refdef_t	refdef;
} pipeview_t;

typedef struct
{
	qboolean		deferred3d;
	pipeview_t		view;
	int				numaliases;
	aliasitem_t		aliases[MAX_VISEDICTS + 1];
	entity_t		entities[MAX_VISEDICTS + 1];	// snapshots the items point to
	int				numparticles;
	pipeparticle_t	particles[MAX_PIPE_PARTICLES];
	int				numcmds;
	drawcmd_t		cmds[MAX_DRAWCMDS];
	int				numtranslations;
	byte			translations[MAX_TRANSLATIONS][256];
	qboolean		setpalette;
	unsigned char	palette[768];
	byte			*buffer;
} pipeframe_t;

static EXT_RAM_BSS_ATTR pipeframe_t	frames[2];
static int				recframe;		// the record core 0 is filling
static pipeframe_t		*runframe;		// the record handed to core 1
static volatile int		pending;		// a record has been handed to core 1
static TaskHandle_t		pipe_waiter;

qboolean	r_pipe_recording;
qboolean	r_pipe_deferred3d;

cvar_t		r_pipeline = {"r_pipeline", "1"};

// core-1 view state (declared by the headers under the names of view_c1_names.h)
vec3_t		r_origin_c1, vpn_c1, vright_c1, vup_c1, r_pright_c1, r_pup_c1, r_ppn_c1;
float		xcenter_c1, ycenter_c1, xscale_c1, yscale_c1;
float		aliasxcenter_c1, aliasycenter_c1, aliasxscale_c1, aliasyscale_c1;
float		r_aliastransition_c1, r_resfudge_c1;
qboolean	r_recursiveaffinetriangles_c1;
qboolean	r_alias_split_c1;
unsigned int d_zwidth_c1;
int			screenwidth_c1;
int			d_vrectx_c1, d_vrecty_c1, d_vrectright_particle_c1, d_vrectbottom_particle_c1;
int			d_pix_min_c1, d_pix_max_c1, d_pix_shift_c1, d_y_aspect_shift_c1;
short		*d_pzbuffer_c1;
EXT_RAM_BSS_ATTR int	d_scantable_c1[MAXHEIGHT];
pixel_t		*d_viewbuffer_c1;
EXT_RAM_BSS_ATTR short	*zspantable_c1[MAXHEIGHT];
float		r_time_c1;
extern refdef_t	r_refdef_c1;

void D_DrawParticle_c1 (particle_t *pparticle);

static void R_PipeCaptureView (pipeview_t *v)
{
	VectorCopy (r_origin, v->r_origin);
	VectorCopy (vpn, v->vpn);
	VectorCopy (vright, v->vright);
	VectorCopy (vup, v->vup);
	VectorScale (vright, xscaleshrink, v->r_pright);	// as R_DrawParticles
	VectorScale (vup, yscaleshrink, v->r_pup);
	VectorCopy (vpn, v->r_ppn);
	v->xcenter = xcenter;
	v->ycenter = ycenter;
	v->xscale = xscale;
	v->yscale = yscale;
	v->aliasxcenter = aliasxcenter;
	v->aliasycenter = aliasycenter;
	v->aliasxscale = aliasxscale;
	v->aliasyscale = aliasyscale;
	v->r_aliastransition = r_aliastransition;
	v->r_resfudge = r_resfudge;
	v->r_recursiveaffinetriangles = r_recursiveaffinetriangles;
	v->d_zwidth = d_zwidth;
	v->screenwidth = screenwidth;
	v->d_vrectx = d_vrectx;
	v->d_vrecty = d_vrecty;
	v->d_vrectright_particle = d_vrectright_particle;
	v->d_vrectbottom_particle = d_vrectbottom_particle;
	v->d_pix_min = d_pix_min;
	v->d_pix_max = d_pix_max;
	v->d_pix_shift = d_pix_shift;
	v->d_y_aspect_shift = d_y_aspect_shift;
	v->d_pzbuffer = d_pzbuffer;
	v->d_viewbuffer = d_viewbuffer;
	v->time = cl.time;
	v->refdef = r_refdef;
}

static void R_PipeApplyView (const pipeview_t *v)
{
	int		i;

	VectorCopy (v->r_origin, r_origin_c1);
	VectorCopy (v->vpn, vpn_c1);
	VectorCopy (v->vright, vright_c1);
	VectorCopy (v->vup, vup_c1);
	VectorCopy (v->r_pright, r_pright_c1);
	VectorCopy (v->r_pup, r_pup_c1);
	VectorCopy (v->r_ppn, r_ppn_c1);
	xcenter_c1 = v->xcenter;
	ycenter_c1 = v->ycenter;
	xscale_c1 = v->xscale;
	yscale_c1 = v->yscale;
	aliasxcenter_c1 = v->aliasxcenter;
	aliasycenter_c1 = v->aliasycenter;
	aliasxscale_c1 = v->aliasxscale;
	aliasyscale_c1 = v->aliasyscale;
	r_aliastransition_c1 = v->r_aliastransition;
	r_resfudge_c1 = v->r_resfudge;
	r_recursiveaffinetriangles_c1 = v->r_recursiveaffinetriangles;
	d_zwidth_c1 = v->d_zwidth;
	screenwidth_c1 = v->screenwidth;
	d_vrectx_c1 = v->d_vrectx;
	d_vrecty_c1 = v->d_vrecty;
	d_vrectright_particle_c1 = v->d_vrectright_particle;
	d_vrectbottom_particle_c1 = v->d_vrectbottom_particle;
	d_pix_min_c1 = v->d_pix_min;
	d_pix_max_c1 = v->d_pix_max;
	d_pix_shift_c1 = v->d_pix_shift;
	d_y_aspect_shift_c1 = v->d_y_aspect_shift;
	d_pzbuffer_c1 = v->d_pzbuffer;
	d_viewbuffer_c1 = v->d_viewbuffer;
	r_time_c1 = v->time;
	r_refdef_c1 = v->refdef;
	r_alias_split_c1 = false;
	// the scan table doesn't depend on the buffer; the z span table does
	for (i = 0 ; i < vid.height ; i++)
	{
		d_scantable_c1[i] = d_scantable[i];
		zspantable_c1[i] = v->d_pzbuffer + i * v->d_zwidth;
	}
}

/*
R_PipeSnapshotView

Give core 1's drawing code the current view right away (the split alias pass of a frame that
isn't deferred; core 1 is idle then).
*/
void R_PipeSnapshotView (void)
{
	static pipeview_t	v;

	R_PipeCaptureView (&v);
	R_PipeApplyView (&v);
	r_alias_split_c1 = r_alias_split;
}

qboolean R_PipePending (void)
{
	return __atomic_load_n (&pending, __ATOMIC_ACQUIRE);
}

/*
R_PipeSync

Wait until core 1 has finished the pending frame, if any. Safe to call from core 1 (no-op).
*/
void R_PipeSync (void)
{
	if (xPortGetCoreID () != 0)
		return;
	while (R_PipePending ())
	{
		pipe_waiter = xTaskGetCurrentTaskHandle ();
		if (R_PipePending ())
			ulTaskNotifyTake (pdTRUE, 1);
	}
}

/*
R_PipeStartRecording

Called by R_RenderView when the world is done: from here to VID_Update, 2D drawing and the
palette are recorded.
*/
void R_PipeStartRecording (void)
{
	pipeframe_t	*f = &frames[recframe];

	if (!r_pipeline.value)
		return;
	// (this record was last used two frames ago; core 1 finished it before the last present)
	f->numcmds = 0;
	f->numtranslations = 0;
	f->setpalette = false;
	r_pipe_recording = true;
}

/*
R_PipeDefer3D

Called by R_DrawEntitiesAndViewModel (before R_PipeStartRecording): returns true if this
frame's models and particles go to core 1. Otherwise waits for the pending frame, so core 0
can draw them itself, using core 1 for half of the models.
*/
qboolean R_PipeDefer3D (void)
{
	pipeframe_t	*f = &frames[recframe];
	int			i;

	r_pipe_deferred3d = false;
	f->deferred3d = false;
	f->numaliases = 0;
	f->numparticles = 0;
	if (r_pipeline.value && !r_dowarp)
	{
		r_pipe_deferred3d = true;
		if (r_drawentities.value)
		{
			for (i = 0 ; i < cl_numvisedicts ; i++)
			{
				if (cl_visedicts[i] == &cl_entities[cl.viewentity])
					continue;
				if (cl_visedicts[i]->model->type == mod_sprite)
				{
					r_pipe_deferred3d = false;	// sprites are only drawn by core 0
					break;
				}
			}
		}
	}
	if (r_pipe_deferred3d)
	{
		f->deferred3d = true;
		R_PipeCaptureView (&f->view);
	}
	else
	{
		R_PipeSync ();
	}
	return r_pipe_deferred3d;
}

void R_PipeAddAlias (aliasitem_t *items, int count)
{
	pipeframe_t	*f = &frames[recframe];
	int			i;

	for (i = 0 ; i < count && f->numaliases < MAX_VISEDICTS + 1 ; i++)
	{
		f->entities[f->numaliases] = *items[i].entity;
		f->aliases[f->numaliases] = items[i];
		f->aliases[f->numaliases].entity = &f->entities[f->numaliases];
		f->numaliases++;
	}
}

void R_PipeAddParticle (const particle_t *p)
{
	pipeframe_t	*f = &frames[recframe];

	if (f->numparticles < MAX_PIPE_PARTICLES)
	{
		VectorCopy (p->org, f->particles[f->numparticles].org);
		f->particles[f->numparticles].color = p->color;
		f->numparticles++;
	}
}

void R_PipeRecord2D (drawop_t op, int x, int y, int w, int h, void *p, const byte *translation)
{
	pipeframe_t	*f = &frames[recframe];
	drawcmd_t	*c;

	if (f->numcmds >= MAX_DRAWCMDS)
		return;
	c = &f->cmds[f->numcmds];
	c->op = op;
	c->x = x;
	c->y = y;
	c->w = w;
	c->h = h;
	c->p = p;
	c->translation = NULL;
	if (translation)
	{
		if (f->numtranslations >= MAX_TRANSLATIONS)
			return;
		memcpy (f->translations[f->numtranslations], translation, 256);
		c->translation = f->translations[f->numtranslations++];
	}
	f->numcmds++;
}

void R_PipeSetPalette (const unsigned char *palette)
{
	pipeframe_t	*f = &frames[recframe];

	memcpy (f->palette, palette, 768);
	f->setpalette = true;
}

/*
R_PipePresent

Called by VID_Update. If this frame was recorded, waits for the previous one and hands it to
core 1, which draws the rest and presents it; returns true. Otherwise returns false (after
waiting for the pending frame) and the caller presents the buffer itself.
*/
qboolean R_PipePresent (byte *buffer)
{
	pipeframe_t	*f = &frames[recframe];

	R_PipeSync ();
	if (!r_pipe_recording)
		return false;
	r_pipe_recording = false;
	r_pipe_deferred3d = false;
	f->buffer = buffer;
	runframe = f;
	pipe_waiter = xTaskGetCurrentTaskHandle ();
	__atomic_store_n (&pending, 1, __ATOMIC_RELEASE);
	recframe ^= 1;
	D_JobsWakeWorker ();
	return true;
}

/*
R_PipeRun

Core 1: draw and present the pending frame.
*/
void R_PipeRun (void)
{
	pipeframe_t	*f = runframe;
	particle_t	p;
	int			i;

	if (f->deferred3d)
	{
		R_PipeApplyView (&f->view);
		{	//DIAG
			static int n;
			if (n++ < 3)
				printf ("PIPEDIAG vb %p zb %p sw %d zw %u ax %f %f %f %f vrect %d %d %d %d aliasvrect %d %d %d %d n %d split %d\n",
					d_viewbuffer_c1, d_pzbuffer_c1, screenwidth_c1, d_zwidth_c1, aliasxscale_c1, aliasyscale_c1,
					aliasxcenter_c1, aliasycenter_c1, r_refdef_c1.vrect.x, r_refdef_c1.vrect.y, r_refdef_c1.vrect.width,
					r_refdef_c1.vrect.height, r_refdef_c1.aliasvrect.x, r_refdef_c1.aliasvrect.y, r_refdef_c1.aliasvrectright,
					r_refdef_c1.aliasvrectbottom, f->numaliases, r_alias_split_c1);
		}
		if (f->numaliases)
			R_DrawAliasItems_c1 (f->aliases, f->numaliases);
		memset (&p, 0, sizeof(p));
		for (i = 0 ; i < f->numparticles ; i++)
		{
			VectorCopy (f->particles[i].org, p.org);
			p.color = f->particles[i].color;
			D_DrawParticle_c1 (&p);
		}
	}
	for (i = 0 ; i < f->numcmds ; i++)
		Draw_Exec (f->buffer, &f->cmds[i]);
	if (f->setpalette)
		QG_SetPalette (f->palette);
	QG_DrawFrame (f->buffer);

	__atomic_store_n (&pending, 0, __ATOMIC_RELEASE);
	if (pipe_waiter)
		xTaskNotifyGive (pipe_waiter);
}
