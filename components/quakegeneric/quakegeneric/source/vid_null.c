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
// vid_null.c -- null video driver to aid porting efforts

#include "quakedef.h"
#include "d_local.h"
#include "r_local.h"
#include "r_pipe.h"

#include "quakegeneric.h"
#include "esp_heap_caps.h"
#include "esp32p4/rom/cache.h"
#include "sdkconfig.h"

viddef_t	vid;				// global video state

#define	BASEWIDTH	QUAKEGENERIC_RES_X
#define	BASEHEIGHT	QUAKEGENERIC_RES_Y

// three frame buffers and two z-buffers: core 1 finishes one frame while core 0 renders the
// next and the display shows the one before (r_pipe.c)
static byte	*vid_buffer[3];
static short	*zbuffers[2];
static int	curbuf, curz;
static byte	*surfcache;
static size_t	surfcache_size;

void	VID_SetPalette (unsigned char *palette)
{
	if (r_pipe_recording)
	{
		R_PipeSetPalette (palette);	// applied with this frame (r_pipe.c)
		return;
	}
	R_PipeSync ();
	// quake generic
	QG_SetPalette(palette);
}

void	VID_ShiftPalette (unsigned char *palette)
{
	VID_SetPalette (palette);
}

void	VID_Init (unsigned char *palette)
{
	zbuffers[0] = calloc(BASEWIDTH*BASEHEIGHT, sizeof(short));
	zbuffers[1] = calloc(BASEWIDTH*BASEHEIGHT, sizeof(short));
	// cache-line (128 B) aligned: the world's bands are copied in by DMA (r_edge.c)
	for (int i = 0 ; i < 3 ; i++)
		vid_buffer[i] = heap_caps_aligned_calloc(128, BASEWIDTH*BASEHEIGHT, 1, MALLOC_CAP_SPIRAM);
	d_gunzbuffer = calloc(BASEWIDTH*BASEHEIGHT, sizeof(unsigned));	// view model (d_polyse.c)
	vid.maxwarpwidth = vid.width = vid.conwidth = BASEWIDTH;
	vid.maxwarpheight = vid.height = vid.conheight = BASEHEIGHT;
	// pixel aspect for the shape the image is displayed at (1.0 = square pixels)
	vid.aspect = ((float)BASEHEIGHT / (float)BASEWIDTH) * QUAKEGENERIC_DISPLAY_ASPECT;
	vid.numpages = 3;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
	vid.buffer = vid.conbuffer = vid_buffer[0];
	vid.rowbytes = vid.conrowbytes = BASEWIDTH;
	
	d_pzbuffer = zbuffers[0];

	// 3x the usual size: span drawing is deferred to after all surfaces of a flush are
	// cached (d_jobs.c), so a block must survive until the end of the flush
	surfcache_size = D_SurfaceCacheForRes(BASEWIDTH, BASEHEIGHT) * 3;
	surfcache = malloc(surfcache_size);
	D_InitCaches (surfcache, surfcache_size);

	{
		extern byte *hunk_base;
		extern int hunk_size;
		uint32_t sc_a0 = ((uint32_t)surfcache + 127u) & ~127u;
		uint32_t sc_a1 = ((uint32_t)surfcache + (uint32_t)surfcache_size) & ~127u;
		uint32_t hk_a0 = ((uint32_t)hunk_base + 127u) & ~127u;
		uint32_t hk_a1 = ((uint32_t)hunk_base + (uint32_t)hunk_size) & ~127u;
		struct l1_dcache_l2_autoload_config al_cfg = {
			.order = CACHE_AUTOLOAD_POSITIVE,
			.trigger = CACHE_AUTOLOAD_BOTH_TRIGGER,
			.ena0 = 1,
			.addr0 = sc_a0,
			.size0 = sc_a1 - sc_a0,
			.ena1 = 1,
			.addr1 = hk_a0,
			.size1 = hk_a1 - hk_a0,
		};
#if CONFIG_QUAKE_L2_AUTOLOAD
		Cache_Disable_L2_Cache_Autoload ();
		Cache_Config_L2_Cache_Autoload (&al_cfg);
		Cache_Enable_L2_Cache_Autoload ();
#else
		(void)al_cfg;
		printf ("L2 autoload DISABLED (CONFIG_QUAKE_L2_AUTOLOAD)\n");
		if (0)
#endif
		printf ("L2 autoload enabled on surfcache (0x%08lx..0x%08lx, %lu KB) + hunk (0x%08lx..0x%08lx, %lu KB)\n",
				(unsigned long)sc_a0, (unsigned long)sc_a1, (unsigned long)((sc_a1 - sc_a0) / 1024),
				(unsigned long)hk_a0, (unsigned long)hk_a1, (unsigned long)((hk_a1 - hk_a0) / 1024));
	}

	{
		extern qboolean	r_bandcopy;

		r_bandcopy = QG_CopyInit ();
	}

	// quake generic
	QG_Init();
}

void	VID_Shutdown (void)
{
	R_PipeSync ();
	free(zbuffers[0]);
	free(zbuffers[1]);
	free(vid_buffer[0]);
	free(vid_buffer[1]);
	free(vid_buffer[2]);
	free(surfcache);
}

void	VID_Update (vrect_t *rects)
{
	// recorded frames are finished and presented by core 1
	if (!R_PipePresent (vid.buffer))
		QG_DrawFrame(vid.buffer);	// quake generic
	curbuf = (curbuf + 1) % 3;
	vid.buffer = vid.conbuffer = vid_buffer[curbuf];
	curz ^= 1;
	D_SetZBuffer (zbuffers[curz]);
}

/*
================
D_BeginDirectRect
================
*/
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
}


/*
================
D_EndDirectRect
================
*/
void D_EndDirectRect (int x, int y, int width, int height)
{
}


