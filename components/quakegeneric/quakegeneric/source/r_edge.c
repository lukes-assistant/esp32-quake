/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_edge.c

#include "quakedef.h"
#include "r_local.h"
#include "quakegeneric.h"
#include "d_local.h"	// D_Jobs*

#if 0
// FIXME
the complex cases add new polys on most lines, so dont optimize for keeping them the same
have multiple free span lists to try to get better coherence?
low depth complexity -- 1 to 3 or so

this breaks spans at every edge, even hidden ones (bad)

have a sentinal at both ends?
#endif


edge_t	*auxedges;
edge_t	*r_edges, *edge_p, *edge_max;

surf_t	*surfaces, *surface_p, *surf_max;

// surfaces are generated in back to front order by the bsp, so if a surf
// pointer is greater than another one, it should be drawn in front
// surfaces[1] is the background, and is used as the active surface stack

edge_t	*newedges[MAXHEIGHT];
edge_t	*removeedges[MAXHEIGHT];

espan_t	*span_p, *max_span_p;

int		r_currentkey;

extern	int	screenwidth;

int	current_iv;

int	edge_head_u_shift20, edge_tail_u_shift20;

static void (*pdrawfunc)(void);

edge_t	edge_head;
edge_t	edge_tail;
edge_t	edge_aftertail;
edge_t	edge_sentinel;

float	fv;

void R_GenerateSpans (void);
void R_GenerateSpansBackward (void);

void R_LeadingEdge (edge_t *edge);
void R_LeadingEdgeBackwards (edge_t *edge);
void R_TrailingEdge (surf_t *surf, edge_t *edge);


//=============================================================================


/*
==============
R_DrawCulledPolys
==============
*/
void R_DrawCulledPolys (void)
{
	surf_t			*s;
	msurface_t		*pface;

	currententity = &cl_entities[0];

	if (r_worldpolysbacktofront)
	{
		for (s=surface_p-1 ; s>&surfaces[1] ; s--)
		{
			if (!s->spans)
				continue;

			if (!(s->flags & SURF_DRAWBACKGROUND))
			{
				pface = (msurface_t *)s->data;
				R_RenderPoly (pface, 15);
			}
		}
	}
	else
	{
		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			if (!(s->flags & SURF_DRAWBACKGROUND))
			{
				pface = (msurface_t *)s->data;
				R_RenderPoly (pface, 15);
			}
		}
	}
}


/*
==============
R_BeginEdgeFrame
==============
*/
void R_BeginEdgeFrame (void)
{
	int		v;

	edge_p = r_edges;
	edge_max = &r_edges[r_numallocatededges];

	surface_p = &surfaces[2];	// background is surface 1,
								//  surface 0 is a dummy
	surfaces[1].spans = NULL;	// no background spans yet
	surfaces[1].flags = SURF_DRAWBACKGROUND;

// put the background behind everything in the world
	if (r_draworder.value)
	{
		pdrawfunc = R_GenerateSpansBackward;
		surfaces[1].key = 0;
		r_currentkey = 1;
	}
	else
	{
		pdrawfunc = R_GenerateSpans;
		surfaces[1].key = 0x7FFFFFFF;
		r_currentkey = 0;
	}

// FIXME: set with memset
	for (v=r_refdef.vrect.y ; v<r_refdef.vrectbottom ; v++)
	{
		newedges[v] = removeedges[v] = NULL;
	}
}

/*
==============
R_InsertNewEdges

Adds the edges in the linked list edgestoadd, adding them to the edges in the
linked list edgelist.  edgestoadd is assumed to be sorted on u, and non-empty (this is actually newedges[v]).  edgelist is assumed to be sorted on u, with a
sentinel at the end (actually, this is the active edge table starting at
edge_head.next).
==============
*/
void R_InsertNewEdges (edge_t *edgestoadd, edge_t *edgelist)
{
	edge_t	*next_edge;

	do
	{
		next_edge = edgestoadd->next;
edgesearch:
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		goto edgesearch;

	// insert edgestoadd before edgelist
addedge:
		edgestoadd->next = edgelist;
		edgestoadd->prev = edgelist->prev;
		edgelist->prev->next = edgestoadd;
		edgelist->prev = edgestoadd;
	} while ((edgestoadd = next_edge) != NULL);
}

/*
==============
R_RemoveEdges
==============
*/
void R_RemoveEdges (edge_t *pedge)
{

	do
	{
		pedge->next->prev = pedge->prev;
		pedge->prev->next = pedge->next;
	} while ((pedge = pedge->nextremove) != NULL);
}

/*
==============
R_StepActiveU
==============
*/
void R_StepActiveU (edge_t *pedge)
{
	edge_t		*pnext_edge, *pwedge;

	while (1)
	{
nextedge:
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		goto nextedge;		
		
pushback:
		if (pedge == &edge_aftertail)
			return;
			
	// push it back to keep it sorted		
		pnext_edge = pedge->next;

	// pull the edge out of the edge list
		pedge->next->prev = pedge->prev;
		pedge->prev->next = pedge->next;

	// find out where the edge goes in the edge list
		pwedge = pedge->prev->prev;

		while (pwedge->u > pedge->u)
		{
			pwedge = pwedge->prev;
		}

	// put the edge back into the edge list
		pedge->next = pwedge->next;
		pedge->prev = pwedge;
		pedge->next->prev = pedge;
		pwedge->next = pedge;

		pedge = pnext_edge;
		if (pedge == &edge_tail)
			return;
	}
}

/*
==============
R_CleanupSpan
==============
*/
void R_CleanupSpan ()
{
	surf_t	*surf;
	int		iu;
	espan_t	*span;

// now that we've reached the right edge of the screen, we're done with any
// unfinished surfaces, so emit a span for whatever's on top
	surf = surfaces[1].next;
	iu = edge_tail_u_shift20;
	if (iu > surf->last_u)
	{
		span = span_p++;
		span->u = surf->last_u;
		span->count = iu - span->u;
		span->v = current_iv;
		span->pnext = surf->spans;
		surf->spans = span;
	}

// reset spanstate for all surfaces in the surface stack
	do
	{
		surf->spanstate = 0;
		surf = surf->next;
	} while (surf != &surfaces[1]);
}


/*
==============
R_LeadingEdgeBackwards
==============
*/
void R_LeadingEdgeBackwards (edge_t *edge)
{
	espan_t			*span;
	surf_t			*surf, *surf2;
	int				iu;

// it's adding a new surface in, so find the correct place
	surf = &surfaces[edge->surfs[1]];

// don't start a span if this is an inverted span, with the end
// edge preceding the start edge (that is, we've already seen the
// end edge)
	if (++surf->spanstate == 1)
	{
		surf2 = surfaces[1].next;

		if (surf->key > surf2->key)
			goto newtop;

	// if it's two surfaces on the same plane, the one that's already
	// active is in front, so keep going unless it's a bmodel
		if (surf->insubmodel && (surf->key == surf2->key))
		{
		// must be two bmodels in the same leaf; don't care, because they'll
		// never be farthest anyway
			goto newtop;
		}

continue_search:

		do
		{
			surf2 = surf2->next;
		} while (surf->key < surf2->key);

		if (surf->key == surf2->key)
		{
		// if it's two surfaces on the same plane, the one that's already
		// active is in front, so keep going unless it's a bmodel
			if (!surf->insubmodel)
				goto continue_search;

		// must be two bmodels in the same leaf; don't care which is really
		// in front, because they'll never be farthest anyway
		}

		goto gotposition;

newtop:
	// emit a span (obscures current top)
		iu = edge->u >> 20;

		if (iu > surf2->last_u)
		{
			span = span_p++;
			span->u = surf2->last_u;
			span->count = iu - span->u;
			span->v = current_iv;
			span->pnext = surf2->spans;
			surf2->spans = span;
		}

		// set last_u on the new span
		surf->last_u = iu;
				
gotposition:
	// insert before surf2
		surf->next = surf2;
		surf->prev = surf2->prev;
		surf2->prev->next = surf;
		surf2->prev = surf;
	}
}


/*
==============
R_TrailingEdge
==============
*/
void R_TrailingEdge (surf_t *surf, edge_t *edge)
{
	espan_t			*span;
	int				iu;

// don't generate a span if this is an inverted span, with the end
// edge preceding the start edge (that is, we haven't seen the
// start edge yet)
	if (--surf->spanstate == 0)
	{
		if (surf->insubmodel)
			r_bmodelactive--;

		if (surf == surfaces[1].next)
		{
		// emit a span (current top going away)
			iu = edge->u >> 20;
			if (iu > surf->last_u)
			{
				span = span_p++;
				span->u = surf->last_u;
				span->count = iu - span->u;
				span->v = current_iv;
				span->pnext = surf->spans;
				surf->spans = span;
			}

		// set last_u on the surface below
			surf->next->last_u = iu;
		}

		surf->prev->next = surf->next;
		surf->next->prev = surf->prev;
	}
}

/*
==============
R_LeadingEdge
==============
*/
void R_LeadingEdge (edge_t *edge)
{
	espan_t			*span;
	surf_t			*surf, *surf2;
	int				iu;
	float			fu, newzi, testzi, newzitop, newzibottom;

	if (edge->surfs[1])
	{
	// it's adding a new surface in, so find the correct place
		surf = &surfaces[edge->surfs[1]];

	// don't start a span if this is an inverted span, with the end
	// edge preceding the start edge (that is, we've already seen the
	// end edge)
		if (++surf->spanstate == 1)
		{
			surf2 = surfaces[1].next;

			if (!surf->insubmodel)
			{
				if (surf->key < surf2->key)
					goto newtop;

				do
				{
					surf2 = surf2->next;
				} while (surf->key >= surf2->key);

				goto gotposition;
			}

			r_bmodelactive++;

			if (surf->key < surf2->key)
				goto newtop;

		// if it's two surfaces on the same plane, the one that's already
		// active is in front, so keep going unless it's a bmodel
			if (surf->key == surf2->key)
			{
			// must be two bmodels in the same leaf; sort on 1/z
				fu = (float)(edge->u - 0xFFFFF) * (1.0 / 0x100000);
				newzi = surf->d_ziorigin + fv*surf->d_zistepv +
						fu*surf->d_zistepu;
				newzibottom = newzi * 0.99;

				testzi = surf2->d_ziorigin + fv*surf2->d_zistepv +
						fu*surf2->d_zistepu;

				if (newzibottom >= testzi)
				{
					goto newtop;
				}

				newzitop = newzi * 1.01;
				if (newzitop >= testzi)
				{
					if (surf->d_zistepu >= surf2->d_zistepu)
					{
						goto newtop;
					}
				}
			}

continue_search:

			do
			{
				surf2 = surf2->next;
			} while (surf->key > surf2->key);

			if (surf->key == surf2->key)
			{
			// if it's two surfaces on the same plane, the one that's already
			// active is in front, so keep going unless it's a bmodel
				if (!surf->insubmodel)
					goto continue_search;

			// must be two bmodels in the same leaf; sort on 1/z
				fu = (float)(edge->u - 0xFFFFF) * (1.0 / 0x100000);
				newzi = surf->d_ziorigin + fv*surf->d_zistepv +
						fu*surf->d_zistepu;
				newzibottom = newzi * 0.99;

				testzi = surf2->d_ziorigin + fv*surf2->d_zistepv +
						fu*surf2->d_zistepu;

				if (newzibottom >= testzi)
				{
					goto gotposition;
				}

				newzitop = newzi * 1.01;
				if (newzitop >= testzi)
				{
					if (surf->d_zistepu >= surf2->d_zistepu)
					{
						goto gotposition;
					}
				}

				goto continue_search;
			}

			goto gotposition;

newtop:
		// emit a span (obscures current top)
			iu = edge->u >> 20;

			if (iu > surf2->last_u)
			{
				span = span_p++;
				span->u = surf2->last_u;
				span->count = iu - span->u;
				span->v = current_iv;
				span->pnext = surf2->spans;
				surf2->spans = span;
			}

			// set last_u on the new span
			surf->last_u = iu;
				
gotposition:
		// insert before surf2
			surf->next = surf2;
			surf->prev = surf2->prev;
			surf2->prev->next = surf;
			surf2->prev = surf;
		}
	}
}


/*
==============
R_GenerateSpans
==============
*/
void R_GenerateSpans (void)
{
	edge_t			*edge;
	surf_t			*surf;

	r_bmodelactive = 0;

// clear active surfaces to just the background surface
	surfaces[1].next = surfaces[1].prev = &surfaces[1];
	surfaces[1].last_u = edge_head_u_shift20;

// generate spans
	for (edge=edge_head.next ; edge != &edge_tail; edge=edge->next)
	{
		__builtin_prefetch (edge->next, 0, 1);
		if (edge->surfs[0])
		{
		// it has a left surface, so a surface is going away for this span
			surf = &surfaces[edge->surfs[0]];

			R_TrailingEdge (surf, edge);

			if (!edge->surfs[1])
				continue;
		}

		R_LeadingEdge (edge);
	}

	R_CleanupSpan ();
}

/*
==============
R_GenerateSpansBackward
==============
*/
void R_GenerateSpansBackward (void)
{
	edge_t			*edge;

	r_bmodelactive = 0;

// clear active surfaces to just the background surface
	surfaces[1].next = surfaces[1].prev = &surfaces[1];
	surfaces[1].last_u = edge_head_u_shift20;

// generate spans
	for (edge=edge_head.next ; edge != &edge_tail; edge=edge->next)
	{
		__builtin_prefetch (edge->next, 0, 1);
		if (edge->surfs[0])
			R_TrailingEdge (&surfaces[edge->surfs[0]], edge);

		if (edge->surfs[1])
			R_LeadingEdgeBackwards (edge);
	}

	R_CleanupSpan ();
}


/*
==============
R_ScanEdges

Input: 
newedges[] array
	this has links to edges, which have links to surfaces

Output:
Each surface has a linked list of its visible spans
==============
*/
/*
==============
Banded frame buffer (ESP32-P4 port)

The world is drawn into one of two small internal-RAM band buffers instead of the PSRAM
frame buffer (d_viewbuffer points into the band, biased by its top row, so every span
drawer works unchanged; queued jobs remember it). The span list is flushed at the end of
every band; once a band's jobs are done, the band is copied to the frame buffer by DMA
(main/bandcopy.c) while the next ones are drawn. PSRAM write-allocate misses on the frame
buffer were ~8% of the frame.
==============
*/
#define BAND_ROWS	16
#define BAND_WIDTH	640		// wider views draw straight into the frame buffer

qboolean	r_bandcopy;		// the DMA copy is available (vid_null.c)
static byte	bandbuf[2][BAND_ROWS * BAND_WIDTH] __attribute__((aligned(128)));

typedef struct
{
	int			top, rows;
	int			firstjob, lastjob;
	uint32_t	ticket;			// its copy (QG_CopyWait)
	qboolean	copying;
} band_t;

static band_t	bands[2];		// the current band and the previous one, by index & 1
static int		curband;
static pixel_t	*band_framebuffer;	// the real d_viewbuffer
static qboolean	banding;
static qboolean	band_needwait;	// the current band's buffer may still be being copied out

static void R_BandReady (void);

static void R_BandSetBuffer (int b)
{
	band_t	*band = &bands[b & 1];

	band->top = r_refdef.vrect.y + b * BAND_ROWS;
	band->rows = r_refdef.vrectbottom - band->top;
	if (band->rows > BAND_ROWS)
		band->rows = BAND_ROWS;
	band->firstjob = D_JobsCount ();
	d_viewbuffer = (pixel_t *)(bandbuf[b & 1] - band->top * screenwidth);
}

// all of the band's jobs are done: copy it out
static void R_BandCopy (int b)
{
	band_t	*band = &bands[b & 1];

	band->ticket = QG_CopyStart (band_framebuffer + band->top * screenwidth, bandbuf[b & 1],
			band->rows * screenwidth);
	band->copying = true;
}

static void R_BandWaitCopy (int b)
{
	band_t	*band = &bands[b & 1];

	if (band->copying)
	{
		QG_CopyWait (band->ticket);
		band->copying = false;
	}
}

static void R_BandBegin (void)
{
	banding = r_bandcopy && !r_dowarp && screenwidth <= BAND_WIDTH &&
			r_refdef.vrect.x == 0 && r_refdef.vrect.width == screenwidth;
	if (!banding)
		return;
	band_framebuffer = d_viewbuffer;
	bands[0].copying = bands[1].copying = false;
	band_needwait = false;
	curband = 0;
	R_BandSetBuffer (0);
}

// called after the flush at the end of a band's last row
static void R_BandNext (void)
{
	bands[curband & 1].lastjob = D_JobsCount ();
	R_BandReady ();	// (normally done at the band's flush already) one copy at a time
	if (curband >= 1)
	{	// the previous band: its jobs have had a band's time to finish
		D_JobsWaitRange (bands[(curband - 1) & 1].firstjob, bands[(curband - 1) & 1].lastjob);
		R_BandCopy (curband - 1);
	}
	curband++;
	// its buffer was band curband-2's, whose copy was only just started: it is waited for
	// at this band's first flush (R_BandReady), giving the DMA a band's worth of scanning
	band_needwait = true;
	R_BandSetBuffer (curband);
}

// before anything is drawn into the current band's buffer
static void R_BandReady (void)
{
	if (banding && band_needwait)
	{
		R_BandWaitCopy (curband - 2);
		band_needwait = false;
	}
}

// after the last flush and D_JobsEndFrame (all jobs done)
static void R_BandEnd (void)
{
	if (!banding)
		return;
	R_BandReady ();
	bands[curband & 1].lastjob = D_JobsCount ();
	if (curband >= 1 && !bands[(curband - 1) & 1].copying)
		R_BandCopy (curband - 1);
	R_BandWaitCopy (curband - 1);	// (one copy at a time)
	R_BandCopy (curband);
	R_BandWaitCopy (curband);
	d_viewbuffer = band_framebuffer;
	banding = false;
}

void R_ScanEdges (void)
{
	int		iv, bottom;
	// internal SRAM, not stack; two buffers so scanning continues while the previous
	// flush's surfaces are drawn by the job queue (d_jobs.c)
	static byte	basespans[2][MAXSPANS*sizeof(espan_t)+CACHE_SIZE];
	static int	bufjobs[2][2];	// range of jobs that use each span buffer
	int		curbuf, firstjob;
	espan_t	*basespan_p;
	surf_t	*s;

	D_JobsBeginFrame ();
	R_BandBegin ();
	curbuf = 0;
	bufjobs[0][0] = bufjobs[0][1] = bufjobs[1][0] = bufjobs[1][1] = 0;

	basespan_p = (espan_t *)
			((intptr_t)(basespans[curbuf] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));
	max_span_p = &basespan_p[MAXSPANS - r_refdef.vrect.width];

	span_p = basespan_p;

// clear active edges to just the background edges around the whole screen
// FIXME: most of this only needs to be set up once
	edge_head.u = r_refdef.vrect.x << 20;
	edge_head_u_shift20 = edge_head.u >> 20;
	edge_head.u_step = 0;
	edge_head.prev = NULL;
	edge_head.next = &edge_tail;
	edge_head.surfs[0] = 0;
	edge_head.surfs[1] = 1;
	
	edge_tail.u = (r_refdef.vrectright << 20) + 0xFFFFF;
	edge_tail_u_shift20 = edge_tail.u >> 20;
	edge_tail.u_step = 0;
	edge_tail.prev = &edge_head;
	edge_tail.next = &edge_aftertail;
	edge_tail.surfs[0] = 1;
	edge_tail.surfs[1] = 0;
	
	edge_aftertail.u = -1;		// force a move
	edge_aftertail.u_step = 0;
	edge_aftertail.next = &edge_sentinel;
	edge_aftertail.prev = &edge_tail;

// FIXME: do we need this now that we clamp x in r_draw.c?
	edge_sentinel.u = 2000 << 24;		// make sure nothing sorts past this
	edge_sentinel.prev = &edge_aftertail;

//	
// process all scan lines
//
	bottom = r_refdef.vrectbottom - 1;

	for (iv=r_refdef.vrect.y ; iv<bottom ; iv++)
	{
		current_iv = iv;
		fv = (float)iv;

	// mark that the head (background start) span is pre-included
		surfaces[1].spanstate = 1;

		if (newedges[iv])
		{
			R_InsertNewEdges (newedges[iv], edge_head.next);
		}

		(*pdrawfunc) ();

	// flush the span list if we can't be sure we have enough spans left for
	// the next scan
		if (span_p >= max_span_p)
		{
			VID_UnlockBuffer ();
			S_ExtraUpdate ();	// don't let sound get messed up if going slow
			VID_LockBuffer ();
		
			firstjob = D_JobsCount ();
			if (r_drawculledpolys)
			{
				R_DrawCulledPolys ();
			}
			else
			{
				R_BandReady ();
				D_DrawSurfaces ();
			}
			bufjobs[curbuf][0] = firstjob;
			bufjobs[curbuf][1] = D_JobsCount ();

		// clear the surface span pointers
			for (s = &surfaces[1] ; s<surface_p ; s++)
				s->spans = NULL;

		// switch span buffers, once the jobs still using the other one are done
			curbuf ^= 1;
			D_JobsWaitRange (bufjobs[curbuf][0], bufjobs[curbuf][1]);
			basespan_p = (espan_t *)
					((intptr_t)(basespans[curbuf] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));
			max_span_p = &basespan_p[MAXSPANS - r_refdef.vrect.width];
			span_p = basespan_p;
		}

	// end of a band: flush, so it can be copied out once drawn
		if (banding && iv == bands[curband & 1].top + BAND_ROWS - 1 && span_p != basespan_p)
		{
			firstjob = D_JobsCount ();
			R_BandReady ();
			D_DrawSurfaces ();
			bufjobs[curbuf][0] = firstjob;
			bufjobs[curbuf][1] = D_JobsCount ();
			for (s = &surfaces[1] ; s<surface_p ; s++)
				s->spans = NULL;
			curbuf ^= 1;
			D_JobsWaitRange (bufjobs[curbuf][0], bufjobs[curbuf][1]);
			basespan_p = (espan_t *)
					((intptr_t)(basespans[curbuf] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));
			max_span_p = &basespan_p[MAXSPANS - r_refdef.vrect.width];
			span_p = basespan_p;
		}
		if (banding && iv == bands[curband & 1].top + BAND_ROWS - 1)
			R_BandNext ();

		if (removeedges[iv])
			R_RemoveEdges (removeedges[iv]);

		if (edge_head.next != &edge_tail)
			R_StepActiveU (edge_head.next);
	}

// do the last scan (no need to step or sort or remove on the last scan)

	current_iv = iv;
	fv = (float)iv;

// mark that the head (background start) span is pre-included
	surfaces[1].spanstate = 1;

	if (newedges[iv])
		R_InsertNewEdges (newedges[iv], edge_head.next);

	(*pdrawfunc) ();

// draw whatever's left in the span list
	R_BandReady ();
	if (r_drawculledpolys)
		R_DrawCulledPolys ();
	else
		D_DrawSurfaces ();

// the entity pass needs the complete z-buffer
	D_JobsEndFrame ();
	R_BandEnd ();
}


