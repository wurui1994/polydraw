

//----------------------------------------------------------------------------

	//Parse script for '@' lines, generating list of sections
static int txt2sec (char *t, tsec_t *ltsec)
{
	int i, j, i0, ntyp, n, slast[4], scnt[4], olin, lin;
	char *cptr;

	for(i=4-1;i>=0;i--) { slast[i] = -1; scnt[i] = 0; }
	i0 = 0; ntyp = 0; n = 0; olin = 0; lin = 0; ltsec[0].nam[0] = 0;
	for(i=0;t[i];i++)
	{
		if (t[i] == '/')
		{
			if (t[i+1] == '/')
			{
				for(i+=2;(t[i]) && (t[i]!='\n');i++);
				lin++;
			}
			else if (t[i+1] == '*')
			{
				for(i+=2;(t[i]) && ((t[i]!='*') || (t[i+1]!='/'));i++) if (t[i] == '\n') lin++;
				if (!t[i]) break;
				i++;
			}
		}
		else if (t[i] == '\n') lin++;
		else if ((t[i] == '@') && ((!i) || ((t[i-1] == '\r') || (t[i-1] == '\n'))))
		{
			ltsec[n].i0 = i0;
			ltsec[n].i1 = i;
			ltsec[n].typ = ntyp;
			ltsec[n].cnt = scnt[ntyp];
			ltsec[n].linofs = olin;
			ltsec[n].nxt = -1;

			if (slast[ntyp] >= 0) ltsec[slast[ntyp]].nxt = n;
			slast[ntyp] = n;
			scnt[ntyp]++;

			i++; if (t[i] == 'h') ntyp = 0;
			else if (t[i] == 'v') ntyp = 1;
			else if (t[i] == 'g') ntyp = 2;
			else if (t[i] == 'f') ntyp = 3;
			else i--; //reuse ntyp
			i++;

			if ((ntyp == 2) && (t[i] == ',') && (n < TSECMAX-1)) //read options for geometry shader
			{
				i++;
				ltsec[n+1].geo_in     = GL_TRIANGLES;
				ltsec[n+1].geo_out    = GL_TRIANGLE_STRIP;
				ltsec[n+1].geo_nverts = 8; //NOTE:default is 0 in specification

				if ((strlen(&t[i]) >=  9) && (!memicmp(&t[i],"GL_POINTS"                 , 9))) { i +=  9; ltsec[n+1].geo_in = GL_POINTS; }
				if ((strlen(&t[i]) >=  8) && (!memicmp(&t[i],"GL_LINES"                  , 8))) { i +=  8; ltsec[n+1].geo_in = GL_LINES; }
				if ((strlen(&t[i]) >= 18) && (!memicmp(&t[i],"GL_LINES_ADJACENCY"        ,18))) { i += 22; ltsec[n+1].geo_in = GL_LINES_ADJACENCY_EXT; }
				if ((strlen(&t[i]) >= 22) && (!memicmp(&t[i],"GL_LINES_ADJACENCY_EXT"    ,22))) { i += 22; ltsec[n+1].geo_in = GL_LINES_ADJACENCY_EXT; }
				if ((strlen(&t[i]) >= 12) && (!memicmp(&t[i],"GL_TRIANGLES"              ,12))) { i += 12; ltsec[n+1].geo_in = GL_TRIANGLES; }
				if ((strlen(&t[i]) >= 22) && (!memicmp(&t[i],"GL_TRIANGLES_ADJACENCY"    ,22))) { i += 26; ltsec[n+1].geo_in = GL_TRIANGLES_ADJACENCY_EXT; }
				if ((strlen(&t[i]) >= 26) && (!memicmp(&t[i],"GL_TRIANGLES_ADJACENCY_EXT",26))) { i += 26; ltsec[n+1].geo_in = GL_TRIANGLES_ADJACENCY_EXT; }
				if (t[i] == ',')
				{
					i++;
					if ((strlen(&t[i]) >=  9) && (!memicmp(&t[i],"GL_POINTS"                 , 9))) { i +=  9; ltsec[n+1].geo_out = GL_POINTS; }
					if ((strlen(&t[i]) >= 13) && (!memicmp(&t[i],"GL_LINE_STRIP"             ,13))) { i += 13; ltsec[n+1].geo_out = GL_LINE_STRIP; }
					if ((strlen(&t[i]) >= 17) && (!memicmp(&t[i],"GL_TRIANGLE_STRIP"         ,17))) { i += 17; ltsec[n+1].geo_out = GL_TRIANGLE_STRIP; }
					if (t[i] == ',')
					{
						i++;
						ltsec[n+1].geo_nverts = strtol(&t[i],&cptr,0); //~1..1024
						i = cptr-t;
					}
				}
			}

			if ((t[i] == ':') && (n < TSECMAX-1))
			{
				j = 0;
				for(i++;(t[i]) && (t[i]!='\r') && (t[i]!='\n');i++)
				{
					if ((t[i] == '/') && (t[i+1] == '/')) break;
					if (j < sizeof(ltsec[0].nam)-1) { ltsec[n+1].nam[j] = t[i]; j++; }
				}
				while ((j > 0) && (ltsec[n+1].nam[j-1] == ' ')) j--;
				ltsec[n+1].nam[j] = 0;
			} else ltsec[n+1].nam[0] = 0;

			while ((t[i]) && (t[i] != '\n')) i++;
			lin++; olin = lin; i0 = i+1;

			n++; if (n >= TSECMAX) return(n);
		}
	}

	ltsec[n].i0 = i0;
	ltsec[n].i1 = i;
	ltsec[n].typ = ntyp;
	ltsec[n].cnt = scnt[ntyp];
	ltsec[n].linofs = olin;
	ltsec[n].nxt = -1;

	if (slast[ntyp] >= 0) ltsec[slast[ntyp]].nxt = n;
	n++;

	return(n);
}

static void glsl_geterrorlines (char *error, int offs)
{
	int i, j;

	for(i=0;error[i];i++)
	{
		if ((i) && (error[i-1] != '\n')) continue; //Check only at beginning of lines

		if (!memcmp(&error[i],"0(",2)) //NVIDIA style
		{
			j = atol(&error[i+2])-1; if ((unsigned)j >= (unsigned)textsiz) continue;
			j += offs; badlinebits[j>>3] |= (1<<(j&7));
		}
		else if (!memcmp(&error[i],"(",1)) //NVIDIA style (old)
		{
			j = atol(&error[i+1])-1; if ((unsigned)j >= (unsigned)textsiz) continue;
			j += offs; badlinebits[j>>3] |= (1<<(j&7));
		}
		else if (!memcmp(&error[i],"ERROR: ",7)) //ATI(AMD)/Intel style
		{
			i += 7;
			if (!((error[i] >= '0') && (error[i] <= '9'))) { continue; } i++;
			while ((error[i] >= '0') && (error[i] <= '9')) i++;
			if (error[i] != ':') { continue; } i++;
			j = atol(&error[i])-1; if ((unsigned)j >= (unsigned)textsiz) continue;
			j += offs; badlinebits[j>>3] |= (1<<(j&7));
		}
	}
}

extern void updatelines(int);
static void setShaders (HWND h, HWND hWndEdit)
{
	static const char shadnam[3][5] = {"vert","geom","frag"};
	static const int shadconst[3] = {GL_VERTEX_SHADER,GL_GEOMETRY_SHADER_EXT,GL_FRAGMENT_SHADER};
	int i, j, k, i0, i1, compiled, needloop = 0, needrecompile, tseci;
	char ch, *cptr, tbuf[4096];
	const char *erst;

	if (gshaderstuck) return;

#if 0
	if (dkeystatus[0x2a]) //debug only!
	{
		dkeystatus[0x2a] = 0;
		sprintf(tbuf,"tsecn=%d",tsecn); kputs(tbuf,1);
		for(i=0;i<tsecn;i++)
		{
			sprintf(tbuf,"%d %d %d %d %d %d [%d %d %d]|%s|",tsec[i].i0,tsec[i].i1,tsec[i].typ,tsec[i].cnt,tsec[i].linofs,tsec[i].nxt,tsec[i].geo_in,tsec[i].geo_out,tsec[i].geo_nverts,tsec[i].nam);
			kputs(tbuf,1);
		}
	}
#endif

	if (dorecompile&2) { dorecompile &= ~2; needrecompile = 1; }
	else if (popts.compctrlent) needrecompile = 0;
	else
	{
		needrecompile = 0;
		for(tseci=0;tseci<tsecn;tseci++)
		{
				//Compare block
			if (!tsec[tseci].typ) continue;
			if ((tseci >= otsecn) || (tsec[tseci].typ != otsec[tseci].typ)) { needrecompile = 1; break; }
			if (tsec[tseci].i1-tsec[tseci].i0 != otsec[tseci].i1-otsec[tseci].i0) { needrecompile = 1; break; }
			if (memcmp(&text[tsec[tseci].i0],&otext[otsec[tseci].i0],tsec[tseci].i1-tsec[tseci].i0)) { needrecompile = 1; break; }
		}
	}
	if (!needrecompile) return;

	memset(badlinebits,0,(textsiz+7)>>3);

	if ((usearbasm) || (usearbasmonly))
	{
		((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_FRAGMENT_PROGRAM_ARB,0);
		((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_VERTEX_PROGRAM_ARB  ,0);
		for(i=0;i<=2;i+=2) for(;shadn[i]>0;shadn[i]--) ((PFNGLDELETEPROGRAMSARBPROC)glfp[glDeleteProgramsARB])(1,&shad[i][shadn[i]-1]);
	}
	else
	{
		//((PFNGLDETACHSHADERPROC)glfp[glDetachShader])(shadprog[i],shad[2][i]); //necessary?
		//((PFNGLDETACHSHADERPROC)glfp[glDetachShader])(shadprog[i],shad[0][i]);

		for(i=0;i<3;i++)
			for(;shadn[i]>0;shadn[i]--) ((PFNGLDELETESHADERPROC)glfp[glDeleteShader])(shad[i][shadn[i]-1]);
		for(;shadprogn>0;shadprogn--) ((PFNGLDELETEPROGRAMPROC)glfp[glDeleteProgram])(shadprog[shadprogn-1]);
	}

	for(tseci=0;tseci<tsecn;tseci++)
	{
		if (!tsec[tseci].typ) continue;

			//Compare block
		//if ((tseci >= otsecn) || (tsec[tseci].typ != otsec[tseci].typ)) needrecompile = 1;
		//else if (tsec[tseci].i1-tsec[tseci].i0 != otsec[tseci].i1-otsec[tseci].i0) needrecompile = 1;
		//else if (memcmp(&text[tsec[tseci].i0],&otext[otsec[tseci].i0],tsec[tseci].i1-tsec[tseci].i0)) needrecompile = 1;
		//else needrecompile = 0;

		j = tsec[tseci].typ-1;
		if (((text[tsec[tseci].i0] == '!') && (text[tsec[tseci].i0+1] == '!')) || (usearbasmonly))
		{
			if ((!usearbasm) && (!usearbasmonly)) ((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(0);
			usearbasm = 1;
			glGetError(); //flush errors (could be from script)

			if (tsec[tseci].nam[0]) sprintf(tbuf,"compile %s_asm %s",shadnam[j],tsec[tseci].nam);
									 else sprintf(tbuf,"compile %s_asm#%d",shadnam[j],tsec[tseci].cnt);
			if (tsec[tseci].typ&1) kputs(tbuf,1);

				  if (tsec[tseci].typ == 1) { i = GL_VERTEX_PROGRAM_ARB; }
			else if (tsec[tseci].typ == 3) { i = GL_FRAGMENT_PROGRAM_ARB; }
			else i = 0;
			if (i)
			{
				glEnable(i);
				((PFNGLGENPROGRAMSARBPROC)glfp[glGenProgramsARB])(1,&j);
				((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(i,j);
				if (i == GL_VERTEX_PROGRAM_ARB) shad[0][shadn[0]] = j;
													else shad[2][shadn[2]] = j;
				((PFNGLPROGRAMSTRINGARBPROC)glfp[glProgramStringARB])(i,GL_PROGRAM_FORMAT_ASCII_ARB,tsec[tseci].i1-tsec[tseci].i0,&text[tsec[tseci].i0]);
				if (glGetError() != GL_NO_ERROR)
				{
					erst = glGetString(GL_PROGRAM_ERROR_STRING_ARB); kputs(erst,1);
					for(j=0;erst[j];j++)
					{
						if ((j) && (erst[j-1] != '\n')) continue;
							  if (!memicmp(&erst[j],"line "       , 5)) k = atol(&erst[j+ 5])-1;
						else if (!memicmp(&erst[j],"Error, line ",12)) k = atol(&erst[j+12])-1;
						else continue;
						if ((unsigned)k < (unsigned)textsiz) badlinebits[(tsec[tseci].linofs+k)>>3] |= (1<<((tsec[tseci].linofs+k)&7));
					}
					updatelines(1); return;
				}
			}
		}
		else if ((j == 1) && (!glfp[glProgramParameteri]))
		{
			kputs("ERROR: Geometry shader not supported by this hardware :/",1);
		}
		else
		{
			usearbasm = 0;

			if (tsec[tseci].nam[0]) sprintf(tbuf,"compile %s %s",shadnam[j],tsec[tseci].nam);
									 else sprintf(tbuf,"compile %s#%d",shadnam[j],tsec[tseci].cnt);
			kputs(tbuf,1);

			if (j == 1) geo2blocki[tsec[tseci].cnt] = tseci; //map shader to block for geometry (to access geo_in, geo_out, geo_nverts)

			i = ((PFNGLCREATESHADERPROC)(glfp[glCreateShader]))(shadconst[j]);
			shad[j][shadn[j]] = i;
			cptr = &text[tsec[tseci].i0]; ch = text[tsec[tseci].i1]; text[tsec[tseci].i1] = 0;
			((PFNGLSHADERSOURCEPROC)glfp[glShaderSource])(i,1,&cptr,0);
			text[tsec[tseci].i1] = ch;

			((PFNGLCOMPILESHADERPROC)glfp[glCompileShader])(i);
			((PFNGLGETSHADERIVPROC)glfp[glGetShaderiv])(i,GL_COMPILE_STATUS,&compiled);
			if (!compiled)
			{
				((PFNGLGETINFOLOGARBPROC)glfp[glGetInfoLogARB])(i,sizeof(tbuf),0,tbuf); kputs(tbuf,1);
				glsl_geterrorlines(tbuf,tsec[tseci].linofs);
				updatelines(1); return;
			}
		}

		shadn[tsec[tseci].typ-1]++;
	}

	gcurshader = 0;
	setshader_int(0,-1,0);
	updatelines(1);
}

	//This cover function protects SOME cases
static EXCEPTION_RECORD gexception_record;
static CONTEXT gexception_context;
static int myexception_getaddr (LPEXCEPTION_POINTERS pxi)
{
	memcpy(&gexception_record,pxi->ExceptionRecord,sizeof(EXCEPTION_RECORD));
	memcpy(&gexception_context,pxi->ContextRecord,sizeof(CONTEXT));
	return(EXCEPTION_EXECUTE_HANDLER);
}
void safeevalfunc (void)
{
	__try
	{
		gevalfunc();
	}
	__except(myexception_getaddr(GetExceptionInformation()))
	{
		char tbuf[256];
		sprintf(tbuf,"\nShader Exception 0x%08x @ 0x%08x :/",gexception_record.ExceptionCode,gexception_record.ExceptionAddress);
		kputs(tbuf,1);
		gshadercrashed = 1; MessageBeep(16); //evil
	}
}

static void Draw (HWND hWnd, HWND hWndEdit)
{       
    static double kglcolorbufferbit   = GL_COLOR_BUFFER_BIT;
    static double kgldepthbufferbit   = GL_DEPTH_BUFFER_BIT;
    static double kglstencilbufferbit = GL_STENCIL_BUFFER_BIT;
	static double kgl_points     = 0.0, kgl_lines = 1.0, kgl_line_loop  = 2.0;
	static double kgl_line_strip = 3.0, kgl_tris  = 4.0, kgl_tri_strip  = 5.0;
	static double kgl_tri_fan    = 6.0, kgl_quads = 7.0, kgl_quad_strip = 8.0;
	static double kgl_polygon    = 9.0, kgl_texture0 = GL_TEXTURE0;
	static double kgl_lines_adjacency = 10.0, kgl_line_strip_adjacency = 11.0;
	static double kgl_triangles_adjacency = 12.0, kgl_triangle_strip_adjacency = 13.0;
	static double kgl_none = GL_NONE, kgl_front = GL_FRONT, kgl_back = GL_BACK, kgl_frontback = GL_FRONT_AND_BACK;
	static double kglzero             = GL_ZERO          , kglone                   = GL_ONE;
	static double kglsrccolor         = GL_SRC_COLOR     , kgloneminussrccolor      = GL_ONE_MINUS_SRC_COLOR;
	static double kgldstcolor         = GL_DST_COLOR     , kgloneminusdstcolor      = GL_ONE_MINUS_DST_COLOR;
	static double kglsrcalpha         = GL_SRC_ALPHA     , kgloneminussrcalpha      = GL_ONE_MINUS_SRC_ALPHA;
	static double kgldstalpha         = GL_DST_ALPHA     , kgloneminusdstalpha      = GL_ONE_MINUS_DST_ALPHA;
	static double kglconstantcolor    = GL_CONSTANT_COLOR, kgloneminusconstantcolor = GL_ONE_MINUS_CONSTANT_COLOR;
	static double kglconstantalpha    = GL_CONSTANT_ALPHA, kgloneminusconstantalpha = GL_ONE_MINUS_CONSTANT_ALPHA;
	static double kglsrcalphasaturate = GL_SRC_ALPHA_SATURATE;
	static double kgldepthtest = GL_DEPTH_TEST;
	static double kglbgra32 = KGL_BGRA32, kglchar = KGL_CHAR, kglshort = KGL_SHORT, kglint = KGL_INT, kglfloat = KGL_FLOAT, kglvec4 = KGL_VEC4;
	static double kgllinear = KGL_LINEAR, kglnearest = KGL_NEAREST;
	static double kglmipmap0 = KGL_MIPMAP0, kglmipmap1 = KGL_MIPMAP1, kglmipmap2 = KGL_MIPMAP2, kglmipmap3 = KGL_MIPMAP3;
	static double kglrepeat = KGL_REPEAT, kglclamp = KGL_CLAMP, kglclamptoedge = KGL_CLAMP_TO_EDGE;

	static double dxres, dyres, dmousx, dmousy;
	static evalextyp myext[] =
	{
		{"GL_COLOR_BUFFER_BIT"     ,&kglcolorbufferbit   },
		{"GL_DEPTH_BUFFER_BIT"     ,&kgldepthbufferbit   },
		{"GL_STENCIL_BUFFER_BIT"   ,&kglstencilbufferbit },
    
		{"GL_POINTS"          ,&kgl_points    },
		{"GL_LINES"           ,&kgl_lines     },
		{"GL_LINE_LOOP"       ,&kgl_line_loop },
		{"GL_LINE_STRIP"      ,&kgl_line_strip},
		{"GL_TRIANGLES"       ,&kgl_tris      },
		{"GL_TRIANGLE_STRIP"  ,&kgl_tri_strip },
		{"GL_TRIANGLE_FAN"    ,&kgl_tri_fan   },
		{"GL_QUADS"           ,&kgl_quads     }, //x
		{"GL_QUAD_STRIP"      ,&kgl_quad_strip}, //x
		{"GL_POLYGON"         ,&kgl_polygon   }, //x

		{"GL_LINES_ADJACENCY"         ,&kgl_lines_adjacency},
		{"GL_LINE_STRIP_ADJACENCY"    ,&kgl_line_strip_adjacency},
		{"GL_TRIANGLES_ADJACENCY"     ,&kgl_triangles_adjacency},
		{"GL_TRIANGLE_STRIP_ADJACENCY",&kgl_triangle_strip_adjacency},
		{"GL_LINES_ADJACENCY_EXT"         ,&kgl_lines_adjacency},
		{"GL_LINE_STRIP_ADJACENCY_EXT"    ,&kgl_line_strip_adjacency},
		{"GL_TRIANGLES_ADJACENCY_EXT"     ,&kgl_triangles_adjacency},
		{"GL_TRIANGLE_STRIP_ADJACENCY_EXT",&kgl_triangle_strip_adjacency},

        {"GLCLEAR()"          ,qglClear       }, //X?
	 	{"GLBEGIN()"          ,qglBegin       }, //X?
		{"GLEND()"            ,qglEnd         }, //X?

		{"GLVERTEX(,)"        ,qglVertex2d    }, //X?
		{"GLVERTEX(,,)"       ,qglVertex3d    }, //X?
		{"GLVERTEX(,,,)"      ,qglVertex4d    }, //X?
		{"GLTEXCOORD(,)"      ,qglTexCoord2d  }, //X?
		{"GLTEXCOORD(,,)"     ,qglTexCoord3d  }, //X?
		{"GLTEXCOORD(,,,)"    ,qglTexCoord4d  }, //X?
		{"GLCOLOR(,,)"        ,qglColor3d     }, //X?
		{"GLCOLOR(,,,)"       ,qglColor4d     }, //X?
		{"GLNORMAL(,,)"       ,qglNormal3d    }, //X?

		{"GLPROGRAMLOCALPARAM(,,,,)",kglProgramLocalParam}, //for arb asm
		{"GLPROGRAMENVPARAM(,,,,)",kglProgramEnvParam}, //for arb asm

		{"GLGETUNIFORMLOC($)",kglGetUniformLoc},
		{"GLUNIFORM1F(,)"     ,kglUniform1f   },
		{"GLUNIFORM2F(,,)"    ,kglUniform2f   },
		{"GLUNIFORM3F(,,,)"   ,kglUniform3f   },
		{"GLUNIFORM4F(,,,,)"  ,kglUniform4f   },
		{"GLUNIFORM1I(,)"     ,kglUniform1i   },
		{"GLUNIFORM2I(,,)"    ,kglUniform2i   },
		{"GLUNIFORM3I(,,,)"   ,kglUniform3i   },
		{"GLUNIFORM4I(,,,,)"  ,kglUniform4i   },
		{"GLUNIFORM1FV(,,&)"  ,kglUniform1fv  },
		{"GLUNIFORM2FV(,,&)"  ,kglUniform2fv  },
		{"GLUNIFORM3FV(,,&)"  ,kglUniform3fv  },
		{"GLUNIFORM4FV(,,&)"  ,kglUniform4fv  },
		{"GLUNIFORM1IV(,,&)"  ,kglUniform1iv  },
		{"GLUNIFORM2IV(,,&)"  ,kglUniform2iv  },
		{"GLUNIFORM3IV(,,&)"  ,kglUniform3iv  },
		{"GLUNIFORM4IV(,,&)"  ,kglUniform4iv  },

		{"GLGETATTRIBLOC($)"  ,kglGetAttribLoc},
		{"GLVERTEXATTRIB1F(,)",kglVertexAttrib1f},
		{"GLVERTEXATTRIB2F(,,)",kglVertexAttrib2f},
		{"GLVERTEXATTRIB3F(,,,)" ,kglVertexAttrib3f},
		{"GLVERTEXATTRIB4F(,,,,)",kglVertexAttrib4f},

		{"GLPUSHMATRIX()"     ,qglPushMatrix  }, //x
		{"GLPOPMATRIX()"      ,qglPopMatrix   }, //x
		{"GLMULTMATRIX(&)"    ,qglMultMatrixd }, //x
		{"GLTRANSLATE(,,)"    ,qglTranslated  }, //x
		{"GLROTATE(,,,)"      ,qglRotated     }, //x
		{"GLSCALE(,,)"        ,qglScaled      }, //x
		{"GLUPERSPECTIVE(,,,)",kgluPerspective},
		{"GLULOOKAT(,,,,,,,,)",qgluLookAt     }, //?
		{"SETFOV()"           ,ksetfov        },

		{"KGL_BGRA32"         ,&kglbgra32     },
		{"KGL_CHAR"           ,&kglchar       },
		{"KGL_SHORT"          ,&kglshort      },
		{"KGL_INT"            ,&kglint        },
		{"KGL_FLOAT"          ,&kglfloat      },
		{"KGL_VEC4"           ,&kglvec4       },
		{"KGL_LINEAR"         ,&kgllinear     },
		{"KGL_NEAREST"        ,&kglnearest    },
		{"KGL_MIPMAP"         ,&kglmipmap3    },
		{"KGL_MIPMAP0"        ,&kglmipmap0    },
		{"KGL_MIPMAP1"        ,&kglmipmap1    },
		{"KGL_MIPMAP2"        ,&kglmipmap2    },
		{"KGL_MIPMAP3"        ,&kglmipmap3    },
		{"KGL_REPEAT"         ,&kglrepeat     },
		{"KGL_CLAMP"          ,&kglclamp      },
		{"KGL_CLAMP_TO_EDGE"  ,&kglclamptoedge},
		{"GLCAPTURE()"        ,qglCapture     }, //?
		{"GLCAPTURE(,,,)"     ,kglCapture     }, //?
		{"GLCAPTUREEND()"     ,qglEndCapture  }, //?
		{"GLSETTEX(,$)"       ,kglsettex      }, //?
		{"GLSETTEX(,$,)"      ,kglsettex2     }, //?
		{"GLSETTEX(,&,,)"     ,kglsettexarray1}, //?
		{"GLSETTEX(,&,,,)"    ,kglsettexarray2}, //?
		{"GLSETTEX(,&,,,,)"   ,kglsettexarray3}, //?
		{"GLGETTEX(,&,,,)"    ,kglgettexarray2}, //?
		{"GLQUAD()"           ,qglQuad        }, //?
		{"GLBINDTEXTURE()"    ,qglBindTex     },
		{"GL_TEXTURE0"        ,&kgl_texture0  },
		{"GLACTIVETEXTURE()"  ,kglActiveTex   },
		{"GLTEXTDISABLE()"    ,qglEndTex      }, //Deprecated:it's a nop!
		{"GL_NONE"            ,&kgl_none      },
		{"GL_FRONT"           ,&kgl_front     },
		{"GL_BACK"            ,&kgl_back      },
		{"GL_FRONT_AND_BACK"  ,&kgl_frontback },
		{"GLCULLFACE()"       ,kglCullFace    },

		{"GL_ZERO"                    ,&kglzero                 }, //dst default
		{"GL_SRC_COLOR"               ,&kglsrccolor             },
		{"GL_DST_COLOR"               ,&kgldstcolor             },
		{"GL_SRC_ALPHA"               ,&kglsrcalpha             }, //src for transparency
		{"GL_DST_ALPHA"               ,&kgldstalpha             },
		{"GL_CONSTANT_COLOR"          ,&kglconstantcolor        },
		{"GL_CONSTANT_ALPHA"          ,&kglconstantalpha        },
		{"GL_SRC_ALPHA_SATURATE"      ,&kglsrcalphasaturate     },
		{"GL_ONE"                     ,&kglone                  }, //src default
		{"GL_ONE_MINUS_SRC_COLOR"     ,&kgloneminussrccolor     },
		{"GL_ONE_MINUS_DST_COLOR"     ,&kgloneminusdstcolor     },
		{"GL_ONE_MINUS_SRC_ALPHA"     ,&kgloneminussrcalpha     }, //dst for transparency
		{"GL_ONE_MINUS_DST_ALPHA"     ,&kgloneminusdstalpha     },
		{"GL_ONE_MINUS_CONSTANT_COLOR",&kgloneminusconstantcolor},
		{"GL_ONE_MINUS_CONSTANT_ALPHA",&kgloneminusconstantalpha},
		{"GLBLENDFUNC(,)"     ,kglBlendFunc   },
		{"GL_DEPTH_TEST"      ,&kgldepthtest  },
		{"GLENABLE()"         ,kglEnable      },
		{"GLDISABLE()"        ,kglDisable     },

		{"GLALPHAENABLE()"    ,qglAlphaEnable }, //Deprecated:use glBlendFunc/glEnable(GL_DEPTH_TEST) instead
		{"GLALPHADISABLE()"   ,qglAlphaDisable}, //Deprecated:use glBlendFunc/glDisable(GL_DEPTH_TEST) instead

		{"GLLINEWIDTH()"      ,qglLineWidth   },

		{"GLSETSHADER()"      ,qglsetshader   }, //vshad=  0 ,gshad= -1 ,fshad=[ 0] (old style)
		{"GLSETSHADER($,$)"   ,kglsetshader2  }, //vshad=[$0],gshad= -1 ,fshad=[$1]
		{"GLSETSHADER($,$,$)" ,kglsetshader3  }, //vshad=[$0],gshad=[$1],fshad=[$2]

		{"GLKLOCKSTART()"     ,glklockstart   },
		{"GLKLOCKELAPSED()"   ,glklockelapsed },
		{"KLOCK()"            ,myklock        },
		{"NUMFRAMES"          ,&dnumframes    },

		{"XRES"               ,&dxres         },
		{"YRES"               ,&dyres         },
		{"MOUSX"              ,&dmousx        },
		{"MOUSY"              ,&dmousy        },
		{"BSTATUS"            ,&dbstatus      },
		{"KEYSTATUS[256]"     ,dkeystatus     },

		{"RGB(,,)"            ,kmyrgb         },     //convert r,g,b to 24-bit col
		{"RGBA(,,,)"          ,kmyrgba        },     //convert r,g,b,a to 32-bit col
		{"NOISE()"            ,noise1d        },     //x     (Tom's noise function)
		{"NOISE(,)"           ,noise2d        },     //x,y   (Tom's noise function)
		{"NOISE(,,)"          ,noise3d        },     //x,y,z (Tom's noise function)
		{"PRINTF($,.)"        ,myprintf       },
		{"PRINTG(,,,$,.)"     ,myprintg       },
		{"SRAND()"            ,mysrand        },
		{"SLEEP()"            ,mysleep        },
		{"GLSWAPINTERVAL()"   ,glswapinterval },
		{"PLAYNOTE(,,)"       ,myplaynote     },
		{"MOUNTZIP($)"        ,mykzaddstack   },
	};
	POINT p0, p1;
	int i, j, needrecompile, tseci;
	char ch;

		//Find host block (use only last one if multiple found)
	for(tseci=0;tsec[tseci].typ;tseci++) if (tseci >= tsecn) return;
	while (tsec[tseci].nxt >= 0) tseci = tsec[tseci].nxt;

	if ((tseci >= otsecn) || (otsec[tseci].typ) || (otsec[tseci].nxt >= 0)) needrecompile = 1;
	else if (tsec[tseci].i1-tsec[tseci].i0 != otsec[tseci].i1-otsec[tseci].i0) needrecompile = 1;
	else if (memcmp(&text[tsec[tseci].i0],&otext[otsec[tseci].i0],tsec[tseci].i1-tsec[tseci].i0)) needrecompile = 1;
	else needrecompile = 0;

	if (popts.compctrlent) needrecompile = 0;
	if (dorecompile&1) { dorecompile &= ~1; needrecompile = 1; }
	if (needrecompile)
	{
		gshadercrashed = 0;

		qglAlphaDisable(0.0);
		kglCullFace(GL_NONE);

		QueryPerformanceCounter((LARGE_INTEGER *)&qtim0); dnumframes = 0.0;

		if (gevalfunc) kasm87free(gevalfunc);
		kasm87addext(myext,sizeof(myext)/sizeof(myext[0]));

		ch = text[tsec[tseci].i1]; text[tsec[tseci].i1] = 0; gevalfunc = (double (__cdecl *)(void))kasm87(&text[tsec[tseci].i0]); text[tsec[tseci].i1] = ch;
		gevalfuncleng = kasm87leng;

		//NOTE: use tsec[tseci].linofs as offset when adding support for line of error

		kputs("compile eval",1);
		kasm87addext(0,0);

		songtime = 0;
		if (gthand) { for(i=0;i<3;i++) ResetEvent(ghevent[i]); }
	}

	dxres = (double)oglxres;
	dyres = (double)oglyres;
	p0.x = p0.y = 0; ClientToScreen(hWndDraw,&p0); GetCursorPos(&p1);
	dmousx = p1.x-p0.x;
	dmousy = p1.y-p0.y;

	if ((gevalfunc) && (!gshaderstuck) && (!gshadercrashed))
	{
		qglsetshader(0);
#if 0
		gevalfunc();
#else
		if (!gthand)
		{
			unsigned win98requiresme;
			//gmainthread = GetCurrentThread();
			ghevent[0] = CreateEvent(0,0,0,0);
			ghevent[1] = CreateEvent(0,0,0,0);
			ghevent[2] = CreateEvent(0,0,0,0);
			gthand = (HANDLE)_beginthreadex(0,4096,watchthread,(void *)0,0,&win98requiresme);
		}

		SetEvent(ghevent[0]);
		safeevalfunc();
		SetEvent(ghevent[1]);
		if (WaitForSingleObject(ghevent[2],1000) == WAIT_TIMEOUT)
		{
			gshaderstuck = 1; MessageBeep(16); //evil
			kputs("\nShader stuck! Now would be a good time to save & quit :/",1);
#if 1
			{ //auto-restart on deadlock
			HANDLE hpipe;
			unsigned long u;
			char buf[1024];

			hpipe = CreateNamedPipe("\\\\.\\pipe\\txtbuf",PIPE_ACCESS_DUPLEX,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,PIPE_UNLIMITED_INSTANCES,sizeof(buf),sizeof(buf),0,0);
			if (hpipe != INVALID_HANDLE_VALUE)
			{
				PROCESS_INFORMATION pi;
				STARTUPINFO si;
				int scrolly, setsel0, setsel1;

				ZeroMemory(&si,sizeof(STARTUPINFO));
				si.cb = sizeof(STARTUPINFO);
				si.wShowWindow = SW_SHOW;

				scrolly = SendMessage(hWndEdit,EM_GETFIRSTVISIBLELINE,0,0);
				SendMessage(hWndEdit,EM_GETSEL,(unsigned)&setsel0,(unsigned)&setsel1);

				sprintf(buf,"%s \\\\.\\pipe\\txtbuf /scrolly=%d /setsel0=%d /setsel1=%d /savfil=%s",gexefullpath,scrolly,setsel0,setsel1,gsavfilnam);
				CreateProcess(0,buf,0,0,1,CREATE_NEW_CONSOLE,0,0,&si,&pi);

				if (ConnectNamedPipe(hpipe,0))
				{
					u = strlen(text);
					if (!WriteFile(hpipe,text,u,&u,0)) { kputs("WriteFile failed",1); return; }

					FlushFileBuffers(hpipe);
					DisconnectNamedPipe(hpipe);
				}
				CloseHandle(hpipe);
				ExitProcess(0);
			}
			}
#endif
		}

		if (showtimeout) { showtimeout = 0; kputs("timeout!",1); }

		//FIXME:uninit:
		//   if (ghevent[2] != (HANDLE)-1) { CloseHandle(ghevent[2]); ghevent[2] = (HANDLE)-1; }
		//   if (ghevent[1] != (HANDLE)-1) { CloseHandle(ghevent[1]); ghevent[1] = (HANDLE)-1; }
		//   if (ghevent[0] != (HANDLE)-1) { CloseHandle(ghevent[0]); ghevent[0] = (HANDLE)-1; }
		//   also: close thread: gthand!
#endif
	}
	else if (needrecompile) { kputs(kasm87err,1); }

	dnumframes++;
}

extern void SaveFile (HWND);
static int passasksave (void)
{
	if (!SendMessage(hWndEdit,EM_GETMODIFY,0,0)) return(1);
	switch(MessageBox(ghwnd,"Save changes?",prognam,MB_YESNOCANCEL))
	{
		case IDYES: SaveFile(ghwnd); return(1);
		case IDNO: return(1);
		case IDCANCEL: break;
	}
	return(0);
}

static void Load (char *filename, HWND hWndEdit)
{
	FILE *fil;
	int i, j, k, ind[4], leng, mal, fileformat;
	char *buf = 0;

	if (!passasksave()) return;
	fil = fopen(filename,"rb");
	if (fil)
	{
		if (!memicmp(filename,"\\\\.\\pipe\\",9)) //load from pipe instead of file
		{
			leng = 0; i = 0; //For pipes, file size is not known in advance
			while (!ferror(fil))
			{
				k = fgetc(fil); if (k == EOF) break;
				j = i; i = ((k == 13) || (k == 10));
				if (i < j) { text[leng] = 13; text[leng+1] = 10; leng += 2; }
				if (i) continue;
				text[leng] = k; leng++;
			}
			text[leng] = 0;
			fileformat = 3;
		}
		else
		{
			char buf5[5];

			strcpy(gsavfilnam,filename); gsavfilnamptr = 0;

				//Autodetect file format... (0:65536 byte file with tons of 0's, 1:4 null-terminated strings)
			fseek(fil,0,SEEK_END); leng = ftell(fil); fseek(fil,0,SEEK_SET);
			fseek(fil,leng-sizeof(buf5),SEEK_SET); fread(buf5,sizeof(buf5),1,fil); fseek(fil,0,SEEK_SET);
			for(i=sizeof(buf5)-1;i>=0;i--) if (buf5[i] != 0) break;
			if ((leng == 65536) && (i < 0)) fileformat = 0; //Tigrou's original file format
			else if (i < sizeof(buf5)-1) fileformat = 1; //any ASCII 0's is binary
			else fileformat = 2;
		}

		switch(fileformat)
		{
			case 0:
				buf = malloc(65536);
				fread(buf,1,65536,fil); //vertex,fragment,eval
				sprintf(text,"%s\r\n\r\n@v: //================================\r\n\r\n%s\r\n\r\n@f: //================================\r\n\r\n%s",&buf[32768],&buf[0],&buf[16384]);

				for(i=0;text[i];i++) if (text[i] > 32) break;
				if (text[i] == '{') { memmove(&text[2],text,strlen(text)+1); text[0] = '('; text[1] = ')'; } //Hack attempting to fix many scripts that lack () at beginning

				free(buf);
				break;
			case 1:
				buf = malloc(leng);
				fread(buf,1,leng,fil);
				i = 0;
				j = strlen(&buf[i])+1; ind[0] = i; i += j;
				j = strlen(&buf[i])+1; ind[1] = i; i += j;
				j = strlen(&buf[i])+1; ind[2] = i; i += j;
				j = strlen(&buf[i])+1; ind[3] = i; i += j;
				sprintf(text,"%s\r\n\r\n@v: //================================\r\n\r\n%s\r\n\r\n@f: //================================\r\n\r\n%s",&buf[ind[2]],&buf[ind[0]],&buf[ind[1]]);
				free(buf);
				break;
			case 2:
				fread(text,1,leng,fil); text[leng] = 0;
				break;
			case 3: break; //same as format 2, but for pipe
		}
		fclose(fil);

			//Convert tabs to 3 spaces in-place
		j = 0; for(i=0;text[i];i++) if (text[i] == 9) j += 2;
		j += i; if (j >= textsiz-1) { kputs("file too long",1); MessageBeep(16);/*evil*/ return; }
		text[j] = 0;
		for(i--;i>=0;i--)
		{
			if (text[i] == 9) { j -= 3; text[j+2] = ' '; text[j+1] = ' '; text[j] = ' '; continue; }
			j--; text[j] = text[i];
		}

		SetWindowText(hWndEdit,text);
	}

	//otext[0] = text[0]^1; otext[1] = 0; //force recompile and reset time

	kglActiveTex(0.0); qglBindTex(0.0);
	ksetfov(90.0); captexsiz = 512; dorecompile = 3;
	glLineWidth(1.0f);

	if (glfp[wglSwapIntervalEXT]) ((PFNWGLSWAPINTERVALEXTPROC)glfp[wglSwapIntervalEXT])(1);

	{ char tbuf[512]; sprintf(tbuf,"\nloaded %s",filename); kputs(tbuf,1); }

	updatelines(1); SendMessage(hWndEdit,EM_SETMODIFY,0,0);
}

static void updatelines (int force)
{
	static int firstvisline = 0;
	int i, j, k, m, n, lin, totlin;

	lin = SendMessage(hWndEdit,EM_GETFIRSTVISIBLELINE,0,0);
	if ((lin == firstvisline) && (!force)) return;
	firstvisline = lin;

	GetWindowText(hWndEdit,ttext,textsiz);

	j = 0; k = 1; totlin = 0;
	for(i=0;1;i++)
	{
		if ((ttext[i] == '@') && ((!i) || (ttext[i-1] == '\r') || (ttext[i-1] == '\n'))) { k = 0; }
		if ((ttext[i] == '\n') || (!ttext[i]))
		{
			char buf[32];
			if (lin > 0) { lin--; }
			else
			{
				if (k == 0)
				{
					for(m=8;m>0;m--) { line[j] = popts.sepchar; j++; }
				}
				else
				{
					for(m=k,n=0;m>0;n++,m/=10) buf[n] = (m%10)+'0';
					if (badlinebits[totlin>>3]&(1<<(totlin&7)))
					{
						while (n < 4) { buf[n] = '*'; n++; }
						buf[n] = '*'; n++;
					}
					for(n--;n>=0;n--) { line[j] = buf[n]; j++; }
				}
				line[j] = '\r'; j++;
				line[j] = '\n'; j++;
			}
			if (!ttext[i]) break;
			k++; totlin++;
		}
	}
	line[j] = 0;
	SetWindowText(hWndLine,line);
}

static void NewFile (int mode)
{
	if (!passasksave()) return;
	if (!mode)
	{
		SetWindowText(hWndEdit,"");
	}
	else if (mode == 1)
	{
		SetWindowText(hWndEdit,
			"glquad(1);\r\n"
			"@v\r\n"
			"void main(){gl_Position=ftransform();}\r\n"
			"@f\r\n"
			"void main(){gl_FragColor=gl_FragCoord*.001;}");
	}
	else if (mode == 2)
	{
		SetWindowText(hWndEdit,
			"   //Host code (EVAL)\r\n"
			"if (numframes == 0)\r\n"
			"{\r\n"
			"   glsettex(0,\"earth.jpg\"); static env; env = glGetUniformLoc(\"env\");\r\n"
			"}\r\n"
			"\r\n"
			"t = klock();\r\n"
			"glbindtexture(0);\r\n"
			"glUniform4f(env,cos(t/2),sin(t/2),0,0);\r\n"
			"glUniform4f(env+1,noise(t,0.5)+1,noise(t,1.5)+1,noise(t,2.5)+1,1);\r\n"
			"glBegin(GL_QUADS);\r\n"
			"glTexCoord(0,0); glVertex(-2,-1,-6);\r\n"
			"glTexCoord(1,0); glVertex(+2,-1,-6);\r\n"
			"glTexCoord(1,1); glVertex(+2,-1,-2);\r\n"
			"glTexCoord(0,1); glVertex(-2,-1,-2);\r\n"
			"glEnd();\r\n"
			"printg(xres-64,0,0xffffff,\"%.2f fps\",numframes/t);\r\n"
			"\r\n"
			"@v:vertex_shader //================================\r\n"
			"varying vec4 p, v, c, t;\r\n"
			"varying vec3 n;\r\n"
			"void main ()\r\n"
			"{\r\n"
			"   gl_Position = ftransform();\r\n"
			"   p = gl_Position;\r\n"
			"   v = gl_Vertex;\r\n"
			"   n = gl_Normal;\r\n"
			"   c = gl_Color;\r\n"
			"   t = gl_MultiTexCoord0;\r\n"
			"}\r\n"
			"\r\n"
			"@f:fragment_shader //================================\r\n"
			"varying vec4 p, v, c, t;\r\n"
			"varying vec3 n;\r\n"
			"uniform sampler2D tex0;\r\n"
			"uniform vec4 env[2];\r\n"
			"void main ()\r\n"
			"{\r\n"
			"   gl_FragColor = texture2D(tex0,t.xy+env[0].xy)*env[1].rgba;\r\n"
			"}");
	}
	else if (mode == 3)
	{
		SetWindowText(hWndEdit,
			"   //Host code (EVAL)\r\n"
			"if (numframes == 0)\r\n"
			"{\r\n"
			"   glsettex(0,\"earth.jpg\");\r\n"
			"}\r\n"
			"\r\n"
			"t = klock();\r\n"
			"glbindtexture(0);\r\n"
			"glProgramEnvParam(0,cos(t/2),sin(t/2),0,0);\r\n"
			"glProgramEnvParam(1,noise(t,0.5)+1,noise(t,1.5)+1,noise(t,2.5)+1,1);\r\n"
			"glBegin(GL_QUADS);\r\n"
			"glTexCoord(0,0); glVertex(-2,-1,-6);\r\n"
			"glTexCoord(1,0); glVertex(+2,-1,-6);\r\n"
			"glTexCoord(1,1); glVertex(+2,-1,-2);\r\n"
			"glTexCoord(0,1); glVertex(-2,-1,-2);\r\n"
			"glEnd();\r\n"
			"printg(xres-64,0,0xffffff,\"%.2f fps\",numframes/t);\r\n"
			"\r\n"
			"@v:vertex_shader //================================\r\n"
			"!!ARBvp1.0\r\n"
			"PARAM ModelViewProj[4] = {state.matrix.mvp};\r\n"
			"TEMP temp;\r\n"
			"DP4 temp.x, ModelViewProj[0], vertex.position;\r\n"
			"DP4 temp.y, ModelViewProj[1], vertex.position;\r\n"
			"DP4 temp.z, ModelViewProj[2], vertex.position;\r\n"
			"DP4 temp.w, ModelViewProj[3], vertex.position;\r\n"
			"MOV result.position, temp;\r\n"
			"MOV result.color, vertex.color;\r\n"
			"MOV result.texcoord[0], vertex.texcoord[0];\r\n"
			"END\r\n"
			"\r\n"
			"@f:fragment_shader //================================\r\n"
			"!!ARBfp1.0\r\n"
			"TEMP pos, col;\r\n"
			"ADD pos, fragment.texcoord[0], program.env[0]; #pan texcoord\r\n"
			"TEX col, pos, texture[0], 2D;\r\n"
			"MUL result.color.xyzw, col, program.env[1].xyzw; #scale color\r\n"
			"END");
	}
	kglActiveTex(0.0); qglBindTex(0.0);
	ksetfov(90.0); captexsiz = 512; dorecompile = 3;
	updatelines(1);
	gsavfilnam[0] = 0; gsavfilnamptr = 0;
	SendMessage(hWndEdit,EM_SETMODIFY,0,0);
}

static void LoadFile (HWND lwnd)
{
	OPENFILENAME ofn;
	char szFileName[MAX_PATH] = "";

	ZeroMemory(&ofn,sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = lwnd;
	ofn.lpstrFilter = "Polydraw Shader Script (*.pss;*.bin)\0*.pss;*.bin\0All Files (*.*)\0*.*\0";
	ofn.lpstrFile = szFileName;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_HIDEREADONLY;
	ofn.lpstrDefExt = "pss";
	if (GetOpenFileName(&ofn)) Load(ofn.lpstrFile,hWndEdit);
	shkeystatus = 0; memset(dkeystatus,0,sizeof(dkeystatus));
}

static void Save (char *filename)
{
	int i;
	FILE *fil = fopen(filename,"wb");
	if (fil)
	{
		strcpy(gsavfilnam,filename); gsavfilnamptr = 0;

		fwrite(text,1,strlen(text),fil);
		fclose(fil);
		MessageBeep(48);
		SendMessage(hWndEdit,EM_SETMODIFY,0,0);
	}
	{ char tbuf[512]; sprintf(tbuf,"\nsaved %s",filename); kputs(tbuf,1); }
}

static void SaveFile (HWND lwnd)
{
	OPENFILENAME ofn;
	char filnam[MAX_PATH] = "";

	strcpy(filnam,gsavfilnam);

	ZeroMemory(&ofn,sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = lwnd;
	ofn.lpstrFilter = "Polydraw Shader Script (*.pss)\0*.pss\0All Files (*.*)\0*.*\0";
	ofn.lpstrFile = filnam;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT|OFN_HIDEREADONLY;
	ofn.lpstrDefExt = "pss";
	if (GetSaveFileName(&ofn)) Save(ofn.lpstrFile);
	shkeystatus = 0; memset(dkeystatus,0,sizeof(dkeystatus));
}

static void resetwindows (int cmdshow);
static void updateshifts (LPARAM lParam, int mode)
{
	if (!mode)
	{
		switch (lParam&0x17f0000)
		{
			case 0x02a0000: shkeystatus &= ~(3<<16); break; //0x2a
			case 0x0360000: shkeystatus &= ~(3<<16); break; //0x36
			case 0x01d0000: shkeystatus &= ~(1<<18); break; //0x1d
			case 0x11d0000: shkeystatus &= ~(1<<19); break; //0x9d
			case 0x0380000: shkeystatus &= ~(1<<20); break; //0x38
			case 0x1380000: shkeystatus &= ~(1<<21); break; //0xb8
		}
	}
	else
	{
		switch (lParam&0x17f0000)
		{
			case 0x02a0000: shkeystatus |= (1<<16); break; //0x2a
			case 0x0360000: shkeystatus |= (1<<17); break; //0x36
			case 0x01d0000: shkeystatus |= (1<<18); break; //0x1d
			case 0x11d0000: shkeystatus |= (1<<19); break; //0x9d
			case 0x0380000: shkeystatus |= (1<<20); break; //0x38
			case 0x1380000: shkeystatus |= (1<<21); break; //0xb8
		}
	}
}

static void helpabout (void)
{
	char tbuf[1024];
	sprintf(tbuf,"PolyDraw, an Opengl scripting tool. Compiled: %s\r\n"
					 "\r\n"
					 "Get latest version here:\r\n"
					 "   http://advsys.net/ken/download.htm#polydraw\r\n"
					 "\r\n"
					 "Authors:\r\n"
					 "\r\n"
					 "   Ken Silverman (http://advsys.net/ken):\r\n"
					 "      EVAL compiler, GUI cleanup, fixes, enhancements\r\n"
					 "\r\n"
					 "   Tigrou (tigrou.ind@gmail.com):\r\n"
					 "      Original author & concept. His version here:\r\n"
					 "         http://pouet.net/prod.php?which=54245\r\n"
					 "         ftp://ftp.untergrund.net/users/ind/polydraw.zip\r\n"
					 ,__DATE__);
	MessageBox(ghwnd,tbuf,prognam,MB_OK);
}