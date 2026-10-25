/*
sprite_c1_names.h -- core-1 names for the sprite drawing code (ESP32-P4 port, see r_pipe.c)

r_sprite_c1.c and d_sprite_c1.c compile r_sprite.c and d_sprite.c a second time for core 1,
which draws a frame's sprites while core 0 sets up the next frame. Everything they write and
the view state they read get core-1 names.
*/
#include "view_c1_names.h"

#define R_DrawSprite                R_DrawSprite_c1
#define R_ClipSpriteFace            R_ClipSpriteFace_c1
#define R_GetSpriteframe            R_GetSpriteframe_c1
#define R_RotateSprite              R_RotateSprite_c1
#define R_SetupAndDrawSprite        R_SetupAndDrawSprite_c1
#define D_DrawSprite                D_DrawSprite_c1
#define D_SpriteCalculateGradients  D_SpriteCalculateGradients_c1
#define D_SpriteDrawSpans           D_SpriteDrawSpans_c1
#define D_SpriteScanLeftEdge        D_SpriteScanLeftEdge_c1
#define D_SpriteScanRightEdge       D_SpriteScanRightEdge_c1
#define r_spritedesc                r_spritedesc_c1
#define TransformVector             TransformVector_c1
#define currententity               currententity_c1
#define modelorg                    modelorg_c1
#define r_refdef                    r_refdef_c1
#define r_entorigin                 r_entorigin_c1
#define view_clipplanes             view_clipplanes_c1
#define xscaleinv                   xscaleinv_c1
#define yscaleinv                   yscaleinv_c1
#define d_sdivzorigin               d_sdivzorigin_c1
#define d_sdivzstepu                d_sdivzstepu_c1
#define d_sdivzstepv                d_sdivzstepv_c1
#define d_tdivzorigin               d_tdivzorigin_c1
#define d_tdivzstepu                d_tdivzstepu_c1
#define d_tdivzstepv                d_tdivzstepv_c1
#define d_ziorigin                  d_ziorigin_c1
#define d_zistepu                   d_zistepu_c1
#define d_zistepv                   d_zistepv_c1
#define cacheblock                  cacheblock_c1
#define cachewidth                  cachewidth_c1
#define sadjust                     sadjust_c1
#define tadjust                     tadjust_c1
#define bbextents                   bbextents_c1
#define bbextentt                   bbextentt_c1
