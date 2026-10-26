/*
d_part_c1.c -- core-1 copy of d_part.c (ESP32-P4 port, see r_pipe.c and view_c1_names.h)
*/
#include "view_c1_names.h"
#define D_DrawParticle		D_DrawParticle_c1
#define D_StartParticles	D_StartParticles_c1
#define D_EndParticles		D_EndParticles_c1
#include "d_part.c"
