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

#include "quakedef.h"

#if CONFIG_QUAKE_TIMEDEMO_LOOP
// Benchmark mode: the first timedemo is started from the command line (main.c); whenever one
// has finished (its result is on the console, which fills the screen once disconnected),
// wait 3 seconds and run it again.
static void QG_TimeDemoLoop (void)
{
	static float	next = -1;
	static qboolean	started;	// the command line's timedemo has begun

	if (cls.demoplayback || cls.state != ca_disconnected)
	{
		started |= cls.timedemo;
		next = -1;
		return;
	}
	if (!started)
		return;
	if (next < 0)
	{
		next = realtime + 3;
		Con_Printf ("next timedemo demo1 in 3 seconds\n");
	}
	else if (realtime >= next)
	{
		next = -1;
		Cbuf_AddText ("timedemo demo1\n");
	}
}
#endif

void QG_Tick(float duration)
{
	Host_Frame(duration);
#if CONFIG_QUAKE_TIMEDEMO_LOOP
	QG_TimeDemoLoop ();
#endif
}

void QG_Create(int argc, char *argv[])
{
	static quakeparms_t    parms;

#if CONFIG_QUAKE_NATIVE_1280x720
	parms.memsize = 10*1024*1024;	// (native 1280x720: the frame/z buffers take the PSRAM)
#else
	parms.memsize = 16*1024*1024;	// (ESP32-P4 port: was 20 MB; PSRAM is also needed for the frame pipeline, r_pipe.c)
#endif
	parms.membase = malloc (parms.memsize);
	parms.basedir = ".";

	COM_InitArgv (argc, argv);

	parms.argc = com_argc;
	parms.argv = com_argv;

	printf ("Host_Init\n");
	Host_Init (&parms);
}
