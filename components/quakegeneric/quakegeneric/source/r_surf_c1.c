/*
r_surf_c1.c -- a second, independent copy of r_surf.c for the second core (ESP32-P4 port)

R_DrawSurface keeps its working state in globals. Compiling r_surf.c again with every
global and function renamed gives core 1 its own copy (R_DrawSurface_c1 with
r_drawsurf_c1 etc.), so both cores can build surface-cache blocks at the same time.
Everything they read (textures, light styles, dynamic lights, colormap) is read-only while
a frame is being drawn.
*/

#define r_drawsurf					r_drawsurf_c1
#define lightleft					lightleft_c1
#define sourcesstep					sourcesstep_c1
#define blocksize					blocksize_c1
#define sourcetstep					sourcetstep_c1
#define lightdelta					lightdelta_c1
#define lightdeltastep				lightdeltastep_c1
#define lightright					lightright_c1
#define lightleftstep				lightleftstep_c1
#define lightrightstep				lightrightstep_c1
#define blockdivshift				blockdivshift_c1
#define blockdivmask				blockdivmask_c1
#define prowdestbase				prowdestbase_c1
#define pbasesource					pbasesource_c1
#define surfrowbytes				surfrowbytes_c1
#define r_lightptr					r_lightptr_c1
#define r_stepback					r_stepback_c1
#define r_lightwidth				r_lightwidth_c1
#define r_numhblocks				r_numhblocks_c1
#define r_numvblocks				r_numvblocks_c1
#define r_source					r_source_c1
#define r_sourcemax					r_sourcemax_c1
#define blocklights					blocklights_c1
#define surfmiptable				surfmiptable_c1
#define R_AddDynamicLights			R_AddDynamicLights_c1
#define R_BuildLightMap				R_BuildLightMap_c1
#define R_TextureAnimation			R_TextureAnimation_c1
#define R_DrawSurface				R_DrawSurface_c1
#define R_DrawSurfaceBlock8_mip0	R_DrawSurfaceBlock8_mip0_c1
#define R_DrawSurfaceBlock8_mip1	R_DrawSurfaceBlock8_mip1_c1
#define R_DrawSurfaceBlock8_mip2	R_DrawSurfaceBlock8_mip2_c1
#define R_DrawSurfaceBlock8_mip3	R_DrawSurfaceBlock8_mip3_c1
#define R_GenTurbTile				R_GenTurbTile_c1
#define R_GenTile					R_GenTile_c1

#include "r_surf.c"
