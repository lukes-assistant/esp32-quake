/*
r_sprite_c1.c -- core-1 copy of r_sprite.c, and the core-1 state the sprite code uses
(ESP32-P4 port, see sprite_c1_names.h)
*/
#include "sprite_c1_names.h"
#include "r_sprite.c"

vec3_t		r_entorigin;		// = r_entorigin_c1 etc.
clipplane_t	view_clipplanes[4];
float		xscaleinv, yscaleinv;
float		d_sdivzorigin, d_sdivzstepu, d_sdivzstepv;
float		d_tdivzorigin, d_tdivzstepu, d_tdivzstepv;
float		d_ziorigin, d_zistepu, d_zistepv;
pixel_t		*cacheblock;
int			cachewidth;
fixed16_t	sadjust, tadjust, bbextents, bbextentt;

// (r_misc.c's reads core 0's view vectors)
void TransformVector (vec3_t in, vec3_t out)
{
	out[0] = DotProduct (in, vright);
	out[1] = DotProduct (in, vup);
	out[2] = DotProduct (in, vpn);
}
