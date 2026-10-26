/*
r_zrows.c -- where the world z-buffer is needed (ESP32-P4 port)

The world's z values are only ever read by things drawn after it: alias models (including
the view model), sprites and particles. Before the world is scanned, R_ComputeZRows marks,
for every screen row, the column range those can touch (the projected Z-clipped frame
bounding box for each alias model, a conservative bounding sphere for sprites, and the pixel
square of each particle). The world z fill (d_jobs.c / d_scan.c) only writes inside these
ranges, so most of the z-buffer is never written at all. Within the ranges the values are
bit-for-bit identical to what the full fill would have written.
*/

#include <string.h>
#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

short		d_zrowmin[MAXHEIGHT], d_zrowmax[MAXHEIGHT];	// inclusive; min > max: none

extern particle_t	*active_particles;
extern cvar_t		r_drawviewmodel;

static const int zbox_edges[12][2] = {
	{0, 1}, {1, 2}, {2, 3}, {3, 0},
	{4, 5}, {5, 6}, {6, 7}, {7, 4},
	{0, 5}, {1, 4}, {2, 7}, {3, 6}
};

static void R_ZRowsAddRect (int u0, int v0, int u1, int v1)
{
	int		v;

	if (u0 < r_refdef.vrect.x)
		u0 = r_refdef.vrect.x;
	if (v0 < r_refdef.vrect.y)
		v0 = r_refdef.vrect.y;
	if (u1 > r_refdef.vrectright - 1)
		u1 = r_refdef.vrectright - 1;
	if (v1 > r_refdef.vrectbottom - 1)
		v1 = r_refdef.vrectbottom - 1;
	if (u0 > u1 || v0 > v1)
		return;

	for (v = v0 ; v <= v1 ; v++)
	{
		if (u0 < d_zrowmin[v])
			d_zrowmin[v] = u0;
		if (u1 > d_zrowmax[v])
			d_zrowmax[v] = u1;
	}
}

static void R_ZRowsAddAll (void)
{
	R_ZRowsAddRect (r_refdef.vrect.x, r_refdef.vrect.y,
			r_refdef.vrectright - 1, r_refdef.vrectbottom - 1);
}

/*
R_ZRowsAddSphere

Mark the screen rectangle that contains the projection of a sphere (used for sprites).
*/
static void R_ZRowsAddSphere (vec3_t center, float radius)
{
	vec3_t	local;
	float	x, y, z, znear, zfar, xmin, xmax, ymin, ymax;

	VectorSubtract (center, r_origin, local);
	z = DotProduct (local, vpn);
	znear = z - radius;
	zfar = z + radius;
	if (zfar < 1.0f)
		return;
	if (znear < 1.0f)
	{
		R_ZRowsAddAll ();	// touches or crosses the near plane
		return;
	}
	x = DotProduct (local, vright);
	y = DotProduct (local, vup);

	xmin = (x - radius) / ((x - radius) < 0 ? znear : zfar);
	xmax = (x + radius) / ((x + radius) > 0 ? znear : zfar);
	ymin = (y - radius) / ((y - radius) < 0 ? znear : zfar);
	ymax = (y + radius) / ((y + radius) > 0 ? znear : zfar);

// screen v grows downwards; 2 pixels of margin for rounding in the rasterisers
	R_ZRowsAddRect ((int)(xcenter + xmin * xscale) - 2, (int)(ycenter - ymax * yscale) - 2,
			(int)(xcenter + xmax * xscale) + 2, (int)(ycenter - ymin * yscale) + 2);
}

/*
R_ZRowsAddAlias

Project the alias model's current frame bounding box after clipping its 12 edges against
ALIAS_Z_CLIP_PLANE (matching R_AliasCheckBBox / R_AliasPreparePoints). This works for
both world entities and cl.viewent (whose origin is at r_origin).
*/
static void R_ZRowsAddAlias (entity_t *e)
{
	aliashdr_t			*pahdr;
	mdl_t				*pmdl;
	maliasframedesc_t	*pframedesc;
	vec3_t				angles, forward, right, up;
	float				tmatrix[3][4], t2matrix[3][4], rotationmatrix[3][4];
	float				viewmatrix[3][4], xform[3][4];
	float				basepts[8][3], viewaux[20][3];
	int					clipped[20];
	qboolean			zclipped, zfullyclipped;
	int					i, frame, numv;
	float				zi, u, v, umin, umax, vmin, vmax;

	pahdr = (aliashdr_t *)Mod_Extradata (e->model);
	pmdl = (mdl_t *)((byte *)pahdr + pahdr->model);

	frame = e->frame;
	if ((frame >= pmdl->numframes) || (frame < 0))
		frame = 0;
	pframedesc = &pahdr->frames[frame];

	angles[ROLL] = e->angles[ROLL];
	angles[PITCH] = -e->angles[PITCH];
	angles[YAW] = e->angles[YAW];
	AngleVectors (angles, forward, right, up);

	memset (tmatrix, 0, sizeof(tmatrix));
	tmatrix[0][0] = pmdl->scale[0];
	tmatrix[1][1] = pmdl->scale[1];
	tmatrix[2][2] = pmdl->scale[2];
	tmatrix[0][3] = pmdl->scale_origin[0];
	tmatrix[1][3] = pmdl->scale_origin[1];
	tmatrix[2][3] = pmdl->scale_origin[2];

	for (i = 0 ; i < 3 ; i++)
	{
		t2matrix[i][0] = forward[i];
		t2matrix[i][1] = -right[i];
		t2matrix[i][2] = up[i];
		t2matrix[i][3] = e->origin[i] - r_origin[i];
	}

	R_ConcatTransforms (t2matrix, tmatrix, rotationmatrix);

	VectorCopy (vright, viewmatrix[0]);
	VectorCopy (vup, viewmatrix[1]);
	VectorInverse (viewmatrix[1]);
	VectorCopy (vpn, viewmatrix[2]);
	viewmatrix[0][3] = 0;
	viewmatrix[1][3] = 0;
	viewmatrix[2][3] = 0;

	R_ConcatTransforms (viewmatrix, rotationmatrix, xform);

	basepts[0][0] = basepts[1][0] = basepts[2][0] = basepts[3][0] =
			(float)pframedesc->bboxmin.v[0];
	basepts[4][0] = basepts[5][0] = basepts[6][0] = basepts[7][0] =
			(float)pframedesc->bboxmax.v[0];

	basepts[0][1] = basepts[3][1] = basepts[5][1] = basepts[6][1] =
			(float)pframedesc->bboxmin.v[1];
	basepts[1][1] = basepts[2][1] = basepts[4][1] = basepts[7][1] =
			(float)pframedesc->bboxmax.v[1];

	basepts[0][2] = basepts[1][2] = basepts[4][2] = basepts[5][2] =
			(float)pframedesc->bboxmin.v[2];
	basepts[2][2] = basepts[3][2] = basepts[6][2] = basepts[7][2] =
			(float)pframedesc->bboxmax.v[2];

	zclipped = false;
	zfullyclipped = true;
	for (i = 0 ; i < 8 ; i++)
	{
		viewaux[i][0] = DotProduct (basepts[i], xform[0]) + xform[0][3];
		viewaux[i][1] = DotProduct (basepts[i], xform[1]) + xform[1][3];
		viewaux[i][2] = DotProduct (basepts[i], xform[2]) + xform[2][3];

		if (viewaux[i][2] < ALIAS_Z_CLIP_PLANE)
		{
			clipped[i] = 1;
			zclipped = true;
		}
		else
		{
			clipped[i] = 0;
			zfullyclipped = false;
		}
	}

	if (zfullyclipped)
		return;

	numv = 8;
	if (zclipped)
	{
		for (i = 0 ; i < 12 ; i++)
		{
			int i0 = zbox_edges[i][0];
			int i1 = zbox_edges[i][1];
			if (clipped[i0] ^ clipped[i1])
			{
				float frac = (ALIAS_Z_CLIP_PLANE - viewaux[i0][2]) /
						(viewaux[i1][2] - viewaux[i0][2]);
				viewaux[numv][0] = viewaux[i0][0] +
						(viewaux[i1][0] - viewaux[i0][0]) * frac;
				viewaux[numv][1] = viewaux[i0][1] +
						(viewaux[i1][1] - viewaux[i0][1]) * frac;
				viewaux[numv][2] = ALIAS_Z_CLIP_PLANE;
				clipped[numv] = 0;
				numv++;
			}
		}
	}

	umin = vmin = 1e9f;
	umax = vmax = -1e9f;
	for (i = 0 ; i < numv ; i++)
	{
		if (clipped[i])
			continue;
		zi = 1.0f / viewaux[i][2];
		u = (viewaux[i][0] * aliasxscale * zi) + aliasxcenter;
		v = (viewaux[i][1] * aliasyscale * zi) + aliasycenter;
		if (u < umin) umin = u;
		if (u > umax) umax = u;
		if (v < vmin) vmin = v;
		if (v > vmax) vmax = v;
	}

	R_ZRowsAddRect ((int)umin - 2, (int)vmin - 2, (int)umax + 2, (int)vmax + 2);
}

static void R_ZRowsAddEntity (entity_t *e)
{
	model_t		*mod = e->model;
	float		radius, d;

	if (!mod)
		return;

	if (mod->type == mod_alias)
	{
		R_ZRowsAddAlias (e);
	}
	else if (mod->type == mod_sprite)
	{
		radius = Length (mod->mins);
		d = Length (mod->maxs);
		if (d > radius)
			radius = d;
		R_ZRowsAddSphere (e->origin, radius + 1);
	}
}

static void R_ZRowsAddParticles (void)
{
	particle_t	*p;
	vec3_t		local, pright, pup, ppn;
	float		zi;
	int			u, v, izi, pix, rows;

	VectorScale (vright, xscaleshrink, pright);
	VectorScale (vup, yscaleshrink, pup);
	VectorCopy (vpn, ppn);

	for (p = active_particles ; p ; p = p->next)
	{
		if (p->die < cl.time)
			continue;
	// same projection and size calculation as D_DrawParticle
		VectorSubtract (p->org, r_origin, local);
		zi = DotProduct (local, ppn);
		if (zi < PARTICLE_Z_CLIP)
			continue;
		zi = 1.0f / zi;
		u = (int)(xcenter + zi * DotProduct (local, pright) + 0.5f);
		v = (int)(ycenter - zi * DotProduct (local, pup) + 0.5f);
		izi = (int)(zi * 0x8000);
		pix = izi >> d_pix_shift;
		if (pix < d_pix_min)
			pix = d_pix_min;
		else if (pix > d_pix_max)
			pix = d_pix_max;
		rows = pix << d_y_aspect_shift;
		R_ZRowsAddRect (u, v, u + pix - 1, v + rows - 1);
	}
}

void R_ComputeZRows (void)
{
	int		i;

	for (i = r_refdef.vrect.y ; i < r_refdef.vrectbottom ; i++)
	{
		d_zrowmin[i] = 32767;
		d_zrowmax[i] = -1;
	}

	if (r_drawentities.value)
	{
		for (i = 0 ; i < cl_numvisedicts ; i++)
		{
			if (cl_visedicts[i] == &cl_entities[cl.viewentity])
				continue;
			R_ZRowsAddEntity (cl_visedicts[i]);
		}
	}
// (not the view model: it has its own z-buffer and is drawn in front of everything, see
// D_PolysetDrawSpans8Gun)
	R_ZRowsAddParticles ();
}
