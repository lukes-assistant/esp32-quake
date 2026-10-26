/*
r_pipe.h -- frame pipelining between the cores (ESP32-P4 port, see r_pipe.c)
*/

// recorded 2D drawing (draw.c)
typedef enum
{
	DC_CHARACTER, DC_PIC, DC_TRANSPIC, DC_TRANSPICTRANSLATE, DC_CONSOLEBACKGROUND,
	DC_TILECLEAR, DC_FILL, DC_FADESCREEN
} drawop_t;

typedef struct
{
	short		op;			// drawop_t
	short		x, y, w, h;	// w: character, lines or fill colour, depending on op
	void		*p;			// pic, conback pic
	const byte	*translation;	// DC_TRANSPICTRANSLATE: a copy owned by the frame record
} drawcmd_t;

extern qboolean	r_pipe_recording;	// 2D drawing and the present are recorded for core 1
extern qboolean	r_pipe_deferred3d;	// this frame's models and particles are drawn by core 1

void R_PipeSync (void);
void R_PipeStartRecording (void);
qboolean R_PipeDefer3D (void);
void R_PipeAddAlias (aliasitem_t *items, int count);
void R_PipeAddParticle (const particle_t *p);
void R_PipeAddSprite (const entity_t *e);
void R_PipeRecord2D (drawop_t op, int x, int y, int w, int h, void *p, const byte *translation);
void R_PipeSetPalette (const unsigned char *palette);
qboolean R_PipePresent (byte *buffer);
qboolean R_PipePending (void);
void R_PipeRun (void);
void R_PipeSnapshotView (void);

void Draw_Exec (byte *dbuf, const drawcmd_t *c);
