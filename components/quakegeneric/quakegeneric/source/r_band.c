/*
r_band.c -- banded rendering (ESP32-P4 port)

The view is rendered in horizontal bands of R_BAND_ROWS rows. Each band's pixels and
z-values go to small buffers in internal SRAM instead of the full-size framebuffer and
z-buffer in PSRAM. R_ScanEdges already walks the scanlines top to bottom; at the last
row of each band it flushes the spans, then R_BandFinish draws the entities, view model
and particles clipped to the band, and copies the band's pixels to the real framebuffer.
The z-buffer never leaves SRAM.

Clipping to the band: world spans only ever cover the rows being scanned; alias models
are clipped through r_refdef.aliasvrect; particles and sprites check r_band_top/bottom.
*/

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

#define R_BAND_ROWS		32

cvar_t		r_bands = {"r_bands", "1"};

qboolean	r_band_active;			// a banded frame is in progress
int			r_band_top, r_band_bottom;	// rows of the current band, inclusive
qboolean	r_band_last;			// the current band is the last one
qboolean	r_bands_drew;			// this frame's entities/particles were drawn per band

static byte		band_fb[R_BAND_ROWS * QUAKEGENERIC_RES_X] __attribute__((aligned(64)));
static short	band_z[R_BAND_ROWS * QUAKEGENERIC_RES_X] __attribute__((aligned(64)));

static pixel_t	*full_viewbuffer;
static short	*full_zbuffer;
static int		saved_aliasvrect_y, saved_aliasvrectbottom;

// world state that the entity pass changes and D_DrawSurfaces depends on
static vec3_t	world_modelorg, world_entorigin;
static entity_t	*world_entity;

static void R_BandSetup (int top)
{
	int		v;

	r_band_top = top;
	r_band_bottom = top + R_BAND_ROWS - 1;
	if (r_band_bottom > r_refdef.vrectbottom - 1)
		r_band_bottom = r_refdef.vrectbottom - 1;
	r_band_last = (r_band_bottom == r_refdef.vrectbottom - 1);

// bias the buffer pointers so that row v of the view lands in the band buffers
	d_viewbuffer = band_fb - top * screenwidth;
	d_pzbuffer = band_z - top * d_zwidth;
	for (v = r_band_top ; v <= r_band_bottom ; v++)
		zspantable[v] = d_pzbuffer + v * d_zwidth;

// alias models clip against aliasvrect (r_aliasuvscale is 1, checked in R_BandsBegin)
	r_refdef.aliasvrect.y = r_band_top;
	r_refdef.aliasvrectbottom = r_band_bottom + 1;
}

/*
R_BandsBegin

Called by R_ScanEdges before scanning. Returns false if this frame can't be banded.
*/
qboolean R_BandsBegin (void)
{
	if (!r_bands.value || r_aliasuvscale != 1.0 ||
		screenwidth > QUAKEGENERIC_RES_X || d_zwidth > QUAKEGENERIC_RES_X)
		return false;

	full_viewbuffer = d_viewbuffer;
	full_zbuffer = d_pzbuffer;
	saved_aliasvrect_y = r_refdef.aliasvrect.y;
	saved_aliasvrectbottom = r_refdef.aliasvrectbottom;

	VectorCopy (modelorg, world_modelorg);
	VectorCopy (r_entorigin, world_entorigin);
	world_entity = currententity;

	r_band_active = true;
	R_BandSetup (r_refdef.vrect.y);
	return true;
}

/*
R_BandFinish

Called by R_ScanEdges once the spans of all rows up to r_band_bottom have been drawn.
*/
void R_BandFinish (void)
{
	int		v;

	R_DrawEntitiesOnList ();
	R_DrawViewModel ();
	R_DrawParticles ();

	VectorCopy (world_modelorg, modelorg);
	VectorCopy (world_entorigin, r_entorigin);
	currententity = world_entity;

	for (v = r_band_top ; v <= r_band_bottom ; v++)
		memcpy (full_viewbuffer + v * screenwidth + r_refdef.vrect.x,
				band_fb + (v - r_band_top) * screenwidth + r_refdef.vrect.x,
				r_refdef.vrect.width);

	if (!r_band_last)
		R_BandSetup (r_band_bottom + 1);
}

void R_BandsEnd (void)
{
	int		v;

	d_viewbuffer = full_viewbuffer;
	d_pzbuffer = full_zbuffer;
	for (v = r_refdef.vrect.y ; v < r_refdef.vrectbottom ; v++)
		zspantable[v] = d_pzbuffer + v * d_zwidth;
	r_refdef.aliasvrect.y = saved_aliasvrect_y;
	r_refdef.aliasvrectbottom = saved_aliasvrectbottom;

	r_band_active = false;
	r_bands_drew = true;
}
