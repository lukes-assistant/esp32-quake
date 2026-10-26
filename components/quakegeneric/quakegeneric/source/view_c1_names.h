/*
view_c1_names.h -- core-1 names for the view state read by the model and particle drawers
(ESP32-P4 port, see r_pipe.c)

Core 1 draws a frame's models and particles while core 0 already sets up the next frame, so
the core-1 copies of the drawing code read their own snapshot of the view (R_PipeSnapshotView
copies it). Included by alias_c1_names.h and d_part_c1.c before any other header.
*/

#define r_origin                    r_origin_c1
#define vpn                         vpn_c1
#define vright                      vright_c1
#define vup                         vup_c1
#define r_pright                    r_pright_c1
#define r_pup                       r_pup_c1
#define r_ppn                       r_ppn_c1
#define xcenter                     xcenter_c1
#define ycenter                     ycenter_c1
#define xscale                      xscale_c1
#define yscale                      yscale_c1
#define aliasxcenter                aliasxcenter_c1
#define aliasycenter                aliasycenter_c1
#define aliasxscale                 aliasxscale_c1
#define aliasyscale                 aliasyscale_c1
#define r_aliastransition           r_aliastransition_c1
#define r_resfudge                  r_resfudge_c1
#define r_recursiveaffinetriangles  r_recursiveaffinetriangles_c1
#define r_alias_split               r_alias_split_c1
#define d_zwidth                    d_zwidth_c1
#define screenwidth                 screenwidth_c1
#define d_vrectx                    d_vrectx_c1
#define d_vrecty                    d_vrecty_c1
#define d_vrectright_particle       d_vrectright_particle_c1
#define d_vrectbottom_particle      d_vrectbottom_particle_c1
#define d_pix_min                   d_pix_min_c1
#define d_pix_max                   d_pix_max_c1
#define d_pix_shift                 d_pix_shift_c1
#define d_y_aspect_shift            d_y_aspect_shift_c1
#define d_pzbuffer                  d_pzbuffer_c1
#define d_scantable                 d_scantable_c1
#define d_viewbuffer                d_viewbuffer_c1
#define zspantable                  zspantable_c1
#define R_ALIAS_TIME                r_time_c1

extern float r_time_c1;
