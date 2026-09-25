/**************************************************************************************************
KPLIB.C: Ken's Picture LIBrary written by Ken Silverman
Copyright (c) 1998-2008 Ken Silverman
Ken Silverman's official web site: http://advsys.net/ken

Features of KPLIB.C:
	* Routines for decoding JPG/PNG/GIF/PCX/TGA/BMP/DDS/CEL.
		See kpgetdim(), kprender(), and optional helper function: kpzload().
	* Routines for reading files out of ZIP/GRP files. All ZIP/GRP functions start with "kz".
	* Multi-platform support: Dos/Windows/Linux/Mac/etc..
	* Compact code, all in a single source file. Yeah, bad design on my part... but makes life
		  easier for everyone else - you simply add a single C file to your project, throw a few
		  externs in there, add the function calls, and you're done!

Brief history:
1998?: Wrote KPEG, a JPEG viewer for DOS
2000: Wrote KPNG, a PNG viewer for DOS
2001: Combined KPEG & KPNG, ported to Visual C, and made it into a library called KPLIB.C
2002: Added support for TGA,GIF,CEL,ZIP
2003: Added support for BMP
05/18/2004: Added support for 8&24 bit PCX
12/09/2005: Added support for progressive JPEG
01/05/2006: Added support for DDS
07/28/2007: Added support for GRP (Build Engine archive)

I offer this code to the community for free use - all I ask is that my name be included in the
credits.

-Ken S.
**************************************************************************************************/

#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

enum //kpgetdim() return values:
{
	KPLIB_NONE=0,
	KPLIB_PNG, KPLIB_JPG, KPLIB_GIF, KPLIB_CEL,
	KPLIB_BMP, KPLIB_PCX, KPLIB_DDS, KPLIB_TGA,
};

#if defined(__POWERPC__)
#define BIGENDIAN 1
#endif

#ifdef BIGENDIAN
