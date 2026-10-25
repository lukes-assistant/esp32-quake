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

void QG_Tick(float duration)
{
	Host_Frame(duration);
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
