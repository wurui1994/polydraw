#if 0 //To compile, type "nmake polydraw.c" (compile options for VC6)
polydraw.exe: polydraw.obj kplib.obj eval.obj
			link polydraw.obj kplib.obj eval.obj opengl32.lib gdi32.lib user32.lib comdlg32.lib winmm.lib /opt:nowin98
polydraw.obj: polydraw.c; cl /c polydraw.c /Ox /G6Fy /MD
kplib.obj:    kplib.c   ; cl /c kplib.c    /Ox /G6Fy /MD
eval.obj:     eval.c    ; cl /c eval.c     /Ox /G6Fy /MD
!if 0
#endif

#include <windows.h>
#include <process.h>
#include <gl/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <math.h>
#include "eval.h"
#define PI 3.14159265358979323

	//TODO:
	// * Bug: glgettex/glsettex doesn't work with global array?
	// * printbez() to allow drawing to graphic window
	// * GPGPU: allow texture indices to swap read&write
	// * multiple render target (gl_FragData)
	// * stencil operations?
	// * RScript
	// * Support transform feedback?
	// * new GL syntax (VBO/VAO)
	// * pic()&getpicsiz()
	// * fil()&getfilsiz()
	// * keyread()
	// * playtext()/playsound()/playsong()
	// * refresh()
	// * allow mouse to resize render window with <-> cursor

	//KPLIB.H:
	//High-level (easy) picture loading function:
extern void kpzload (const char *, int *, int *, int *z, int *);
	//Low-level PNG/JPG functions:
extern int kpgetdim (const char *, int, int *, int *);
extern int kprender (const char *, int, INT_PTR, int, int, int, int, int);
	//Ken's ZIP functions:
extern int kzaddstack (const char *);
extern void kzuninit ();
extern void kzsetfil (FILE *);
extern int kzopen (const char *);
extern void kzfindfilestart (const char *);
extern int kzfindfile (char *);
extern int kzread (void *, int);
extern int kzfilelength ();
extern int kzseek (int, int);
extern int kztell ();
extern int kzgetc ();
extern int kzeof ();
extern void kzclose ();

#define GLAPIENTRY APIENTRY
#define GL_TEXTURE_WRAP_R           0x8072
#define GL_MIRRORED_REPEAT          0x8370
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_TEXTURE_CUBE_MAP         0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X 0x8516
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y 0x8517
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y 0x8518
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z 0x8519
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_CONSTANT_COLOR                 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR       0x8002
#define GL_CONSTANT_ALPHA                 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA       0x8004
#define GL_TEXTURE_3D               0x806F
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_VERTEX_PROGRAM_ARB       0x8620
#define GL_FRAGMENT_PROGRAM_ARB     0x8804
#define GL_PROGRAM_ERROR_STRING_ARB 0x8874
#define GL_PROGRAM_FORMAT_ASCII_ARB 0x8875
#define GL_TIME_ELAPSED_EXT         0x88BF
#define GL_QUERY_RESULT             0x8866
#define GL_QUERY_RESULT_AVAILABLE   0x8867
#define GL_TEXTURE0                 0x84c0
#define GL_FRAMEBUFFER_EXT                0x8D40
#define GL_COLOR_ATTACHMENT0_EXT          0x8CE0
#define GL_COLOR_ATTACHMENT1_EXT          0x8CE1
#define GL_COLOR_ATTACHMENT2_EXT          0x8CE2
#define GL_COLOR_ATTACHMENT3_EXT          0x8CE3
#define GL_LINES_ADJACENCY_EXT                 0x000A
#define GL_LINE_STRIP_ADJACENCY_EXT            0x000B
#define GL_TRIANGLES_ADJACENCY_EXT             0x000C
#define GL_TRIANGLE_STRIP_ADJACENCY_EXT        0x000D
#define GL_PROGRAM_POINT_SIZE_EXT              0x8642
#define GL_MAX_PROGRAM_OUTPUT_VERTICES         0x8C27
#define GL_MAX_PROGRAM_TOTAL_OUTPUT_COMPONENTS 0x8C28
#define GL_GEOMETRY_SHADER_EXT                 0x8DD9
#define GL_GEOMETRY_VERTICES_OUT_EXT           0x8DDA
#define GL_GEOMETRY_INPUT_TYPE_EXT             0x8DDB
#define GL_GEOMETRY_OUTPUT_TYPE_EXT            0x8DDC
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_FLOAT                          0x1406
#define GL_TEXTURE_RECTANGLE_ARB          0x84F5
#define GL_RGBA32F_ARB                    0x8814
#define GL_LUMINANCE32F_ARB               0x8818
#define GL_LUMINANCE32I_EXT               0x8D86

typedef int GLsizei;
typedef char GLchar;
typedef char GLcharARB;
typedef unsigned int GLhandleARB;
typedef unsigned __int64 GLuint64EXT;
typedef GLuint (GLAPIENTRY *PFNGLCREATESHADERPROC      )(GLenum type);
typedef GLuint (GLAPIENTRY *PFNGLCREATEPROGRAMPROC     )(void);
typedef void   (GLAPIENTRY *PFNGLSHADERSOURCEPROC      )(GLuint shader, GLsizei count, const GLchar **strings, const GLint *lengths);
typedef void   (GLAPIENTRY *PFNGLCOMPILESHADERPROC     )(GLuint shader);
typedef void   (GLAPIENTRY *PFNGLATTACHSHADERPROC      )(GLuint program, GLuint shader);
typedef void   (GLAPIENTRY *PFNGLLINKPROGRAMPROC       )(GLuint program);
typedef void   (GLAPIENTRY *PFNGLUSEPROGRAMPROC        )(GLuint program);
typedef void   (GLAPIENTRY *PFNGLGETPROGRAMIVPROC      )(GLuint program, GLenum pname, GLint *param);
typedef void   (GLAPIENTRY *PFNGLGETSHADERIVPROC       )(GLuint shader, GLenum pname, GLint *param);
typedef void   (GLAPIENTRY *PFNGLGETINFOLOGARBPROC     )(GLhandleARB obj, GLsizei maxLength, GLsizei *length, GLcharARB *infoLog);
typedef void   (GLAPIENTRY *PFNGLDETACHSHADERPROC      )(GLuint program, GLuint shader);
typedef void   (GLAPIENTRY *PFNGLDELETEPROGRAMPROC     )(GLuint program);
typedef void   (GLAPIENTRY *PFNGLDELETESHADERPROC      )(GLuint shader);
typedef GLint  (GLAPIENTRY *PFNGLGETUNIFORMLOCATIONPROC)(GLuint program, const GLchar *name);
typedef void   (GLAPIENTRY *PFNGLUNIFORM1FPROC         )(GLint location, GLfloat v0);
typedef void   (GLAPIENTRY *PFNGLUNIFORM2FPROC         )(GLint location, GLfloat v0, GLfloat v1);
typedef void   (GLAPIENTRY *PFNGLUNIFORM3FPROC         )(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
typedef void   (GLAPIENTRY *PFNGLUNIFORM4FPROC         )(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
typedef void   (GLAPIENTRY *PFNGLUNIFORM1IPROC         )(GLint location, GLint v0);
typedef void   (GLAPIENTRY *PFNGLUNIFORM2IPROC         )(GLint location, GLint v0, GLint v1);
typedef void   (GLAPIENTRY *PFNGLUNIFORM3IPROC         )(GLint location, GLint v0, GLint v1, GLint v2);
typedef void   (GLAPIENTRY *PFNGLUNIFORM4IPROC         )(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
typedef void   (GLAPIENTRY *PFNGLUNIFORM1FVPROC        )(GLint location, GLsizei count, const GLfloat *value);
typedef void   (GLAPIENTRY *PFNGLUNIFORM2FVPROC        )(GLint location, GLsizei count, const GLfloat *value);
typedef void   (GLAPIENTRY *PFNGLUNIFORM3FVPROC        )(GLint location, GLsizei count, const GLfloat *value);
typedef void   (GLAPIENTRY *PFNGLUNIFORM4FVPROC        )(GLint location, GLsizei count, const GLfloat *value);
typedef void   (GLAPIENTRY *PFNGLUNIFORM1IVPROC        )(GLint location, GLsizei count, const GLint *value);
typedef void   (GLAPIENTRY *PFNGLUNIFORM2IVPROC        )(GLint location, GLsizei count, const GLint *value);
typedef void   (GLAPIENTRY *PFNGLUNIFORM3IVPROC        )(GLint location, GLsizei count, const GLint *value);
typedef void   (GLAPIENTRY *PFNGLUNIFORM4IVPROC        )(GLint location, GLsizei count, const GLint *value);
typedef GLint  (GLAPIENTRY *PFNGLGETATTRIBLOCATIONPROC )(GLuint program, const GLchar *name);
typedef void   (GLAPIENTRY *PFNGLVERTEXATTRIB1FPROC    )(GLuint index, GLfloat v0);
typedef void   (GLAPIENTRY *PFNGLVERTEXATTRIB2FPROC    )(GLuint index, GLfloat v0, GLfloat v1);
typedef void   (GLAPIENTRY *PFNGLVERTEXATTRIB3FPROC    )(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2);
typedef void   (GLAPIENTRY *PFNGLVERTEXATTRIB4FPROC    )(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
typedef void   (GLAPIENTRY *PFNGLACTIVETEXTUREPROC     )(GLuint texture);
typedef void   (GLAPIENTRY *PFNGLTEXIMAGE3DPROC        )(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
typedef void   (GLAPIENTRY *PFNGLTEXSUBIMAGE3DPROC     )(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *);
typedef GLint  (GLAPIENTRY *PFNWGLSWAPINTERVALEXTPROC  )(GLint interval);
typedef void   (GLAPIENTRY *PFNGLGENPROGRAMSARBPROC            )(GLsizei n, GLuint *programs);
typedef void   (GLAPIENTRY *PFNGLBINDPROGRAMARBPROC            )(GLenum target, GLuint program);
typedef void   (GLAPIENTRY *PFNGLGETPROGRAMSTRINGARBPROC       )(GLenum target, GLenum pname, GLvoid *string);
typedef void   (GLAPIENTRY *PFNGLPROGRAMSTRINGARBPROC          )(GLenum target, GLenum format, GLsizei len, const GLvoid *string);
typedef void   (GLAPIENTRY *PFNGLPROGRAMLOCALPARAMETER4FARBPROC)(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
typedef void   (GLAPIENTRY *PFNGLPROGRAMENVPARAMETER4FARBPROC  )(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
typedef void   (GLAPIENTRY *PFNGLPROGRAMPARAMETERIEXTPROC) (GLuint program, GLenum pname, GLint value);
typedef void   (GLAPIENTRY *PFNGLDELETEPROGRAMSARBPROC         )(GLsizei n, const GLuint *programs);
typedef void   (GLAPIENTRY *PFNGLGENQUERIESPROC)             (GLsizei n, GLuint *ids);
typedef void   (GLAPIENTRY *PFNGLDELETEQUERIESPROC)          (GLsizei n, const GLuint *ids);
typedef void   (GLAPIENTRY *PFNGLBEGINQUERYPROC)             (GLenum target, GLuint id);
typedef void   (GLAPIENTRY *PFNGLENDQUERYPROC)               (GLenum target);
typedef void   (GLAPIENTRY *PFNGLGETQUERYIVPROC)             (GLenum target, GLenum pname, GLint *params);
typedef void   (GLAPIENTRY *PFNGLGETQUERYOBJECTIVPROC)       (GLuint id, GLenum pname, GLint *params);
typedef void   (GLAPIENTRY *PFNGLGETQUERYOBJECTUIVPROC)      (GLuint id, GLenum pname, GLuint *params);
typedef void   (GLAPIENTRY *PFNGLGETQUERYOBJECTUI64VEXTPROC) (GLuint id, GLenum pname, GLuint64EXT *params);
typedef void   (GLAPIENTRY *PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void   (GLAPIENTRY *PFNGLBINDFRAMEBUFFEREXTPROC)     (GLenum, GLuint);
typedef void   (GLAPIENTRY *PFNGLGENFRAMEBUFFERSEXTPROC)     (GLsizei, GLuint *);

	//         char  1 *        GL_LUMINANCE8  GL_LUMINANCE    GL_SIGNED_BYTE
	//        cvec4  4 *  4 (GL_SIGNED_RGBA8)       GL_RGBA    GL_SIGNED_BYTE
	//        uchar  1 *        GL_LUMINANCE8  GL_LUMINANCE  GL_UNSIGNED_BYTE
	//       ucvec4  4 *         4 (GL_RGBA8)       GL_RGBA  GL_UNSIGNED_BYTE
	//argb32         4 *         4 (GL_RGBA8)   GL_BGRA_EXT  GL_UNSIGNED_BYTE
	//        short  2 *       GL_LUMINANCE16  GL_LUMINANCE   GL_SIGNED_SHORT
	//        svec4  8             GL_RGBA16I       GL_RGBA   GL_SIGNED_SHORT //new gpu only
	//       ushort  2 *       GL_LUMINANCE16  GL_LUMINANCE GL_UNSIGNED_SHORT
	//       usvec4  8            GL_RGBA16UI       GL_RGBA GL_UNSIGNED_SHORT //new gpu only
	//    *     int  4 *  GL_LUMINANCE32I_EXT  GL_LUMINANCE     GL_SIGNED_INT //new gpu only
	//    *   ivec4 16             GL_RGBA32I       GL_RGBA     GL_SIGNED_INT //new gpu only
	//    *    uint  4 * GL_LUMINANCE32UI_EXT  GL_LUMINANCE   GL_UNSIGNED_INT //new gpu only
	//    *   uvec4 16            GL_RGBA32UI       GL_RGBA   GL_UNSIGNED_INT //new gpu only
	//    *   float  4 *  GL_LUMINANCE32F_ARB  GL_LUMINANCE          GL_FLOAT
	//    *    vec4 16 *       GL_RGBA32F_ARB       GL_RGBA          GL_FLOAT
	//
	//public: KGL_BGRA32, KGL_FLOAT, KGL_VEC4

enum { KGL_BGRA32=0, KGL_CHAR, KGL_SHORT, KGL_INT/*only supported on newest cards*/, KGL_FLOAT, KGL_VEC4, KGL_NUM};
enum { KGL_LINEAR = (0<<4), KGL_NEAREST = (1<<4), KGL_MIPMAP = (2<<4),
		 KGL_MIPMAP3 = (2<<4), KGL_MIPMAP2 = (3<<4), KGL_MIPMAP1 = (4<<4), KGL_MIPMAP0 = (5<<4)};
enum { KGL_REPEAT = (0<<8), KGL_MIRRORED_REPEAT = (1<<8), KGL_CLAMP = (2<<8), KGL_CLAMP_TO_EDGE = (3<<8)};

static int usearbasm = 0, usearbasmonly = 0; //1 if "!!" is detected
static int useoldglfuncs = 0;
const static char *glnames[] =
{
	"glGenProgramsARB","glBindProgramARB",                      //ARB ASM...
	"glGetProgramStringARB","glProgramStringARB",
	"glProgramLocalParameter4fARB",
	"glProgramEnvParameter4fARB",
	"glDeleteProgramsARB",

	"glActiveTexture","glTexImage3D","glTexSubImage3D",         //multi/extended texture
	"wglSwapIntervalEXT",                                       //limit refresh/sleep

	"glCreateShader","glCreateProgram",                         //compile
	"glShaderSource","glCompileShader",                         //
	"glAttachShader","glLinkProgram","glUseProgram",            //link
	"glGetShaderiv","glGetProgramiv","glGetInfoLogARB",         //get info
	"glDetachShader","glDeleteProgram","glDeleteShader",        //decompile
	"glGetUniformLocation","glGetAttribLocation",               //host->shader
	"glUniform1f" ,"glUniform2f" ,"glUniform3f" ,"glUniform4f",
	"glUniform1i" ,"glUniform2i" ,"glUniform3i" ,"glUniform4i",
	"glUniform1fv","glUniform2fv","glUniform3fv","glUniform4fv",
	"glUniform1iv","glUniform2iv","glUniform3iv","glUniform4iv",
	"glVertexAttrib1f","glVertexAttrib2f","glVertexAttrib3f","glVertexAttrib4f",

	"glGenQueries","glDeleteQueries",
	"glBeginQuery","glEndQuery",
	"glGetQueryObjectiv",
	"glGetQueryObjectuiv",

	"glFramebufferTexture2DEXT","glBindFramebufferEXT","glGenFramebuffersEXT",

	"glGetQueryObjectui64vEXT",

	"glProgramParameteriEXT",
};
const static char *glnames_old[] =
{
	"glGenProgramsARB","glBindProgramARB",                          //ARB ASM...
	"glGetProgramStringARB","glProgramStringARB",
	"glProgramLocalParameter4fARB",
	"glProgramEnvParameter4fARB",
	"glDeleteProgramsARB",

	"glActiveTexture","glTexImage3D","glTexSubImage3D",             //texture unit
	"wglSwapIntervalEXT",                                           //limit refresh/sleep

	"glCreateShaderObjectARB","glCreateProgramObjectARB",           //compile
	"glShaderSourceARB","glCompileShaderARB",                       //
	"glAttachObjectARB","glLinkProgramARB","glUseProgramObjectARB", //link
	"glGetObjectParameterivARB","glGetObjectParameterivARB","glGetInfoLogARB", //get info
	"glDetachObjectARB","glDeleteObjectARB","glDeleteObjectARB",    //decompile
	"glGetUniformLocationARB","glGetAttribLocationARB",             //host->shader
	"glUniform1fARB" ,"glUniform2fARB" ,"glUniform3fARB" ,"glUniform4fARB",
	"glUniform1iARB" ,"glUniform2iARB" ,"glUniform3iARB" ,"glUniform4iARB",
	"glUniform1fvARB","glUniform2fvARB","glUniform3fvARB","glUniform4fvARB",
	"glUniform1ivARB","glUniform2ivARB","glUniform3ivARB","glUniform4ivARB",
	"glVertexAttrib1fARB","glVertexAttrib2fARB","glVertexAttrib3fARB","glVertexAttrib4fARB",

	"glGenQueriesARB","glDeleteQueriesARB",
	"glBeginQueryARB","glEndQueryARB",
	"glGetQueryObjectivARB",
	"glGetQueryObjectuivARB",

	"glFramebufferTexture2DEXT","glBindFramebufferEXT","glGenFramebuffersEXT",

	"glGetQueryObjectui64vEXT",

	"glProgramParameteriEXT",
};
enum
{
	glGenProgramsARB,glBindProgramARB,             //ARB ASM...
	glGetProgramStringARB,glProgramStringARB,
	glProgramLocalParameter4fARB,
	glProgramEnvParameter4fARB,
	glDeleteProgramsARB,

	glActiveTexture,glTexImage3D,glTexSubImage3D,  //multi/extended texture
	wglSwapIntervalEXT,                            //limit refesh/sleep

	glCreateShader,glCreateProgram,                //compile
	glShaderSource,glCompileShader,                //
	glAttachShader,glLinkProgram,glUseProgram,     //link
	glGetShaderiv,glGetProgramiv,glGetInfoLogARB,  //get info
	glDetachShader,glDeleteProgram,glDeleteShader, //decompile
	glGetUniformLocation,glGetAttribLocation,      //host->shader
	glUniform1f, glUniform2f, glUniform3f, glUniform4f,
	glUniform1i, glUniform2i, glUniform3i, glUniform4i,
	glUniform1fv,glUniform2fv,glUniform3fv,glUniform4fv,
	glUniform1iv,glUniform2iv,glUniform3iv,glUniform4iv,
	glVertexAttrib1f,glVertexAttrib2f,glVertexAttrib3f,glVertexAttrib4f,

	glGenQueries,glDeleteQueries,
	glBeginQuery,glEndQuery,
	glGetQueryObjectiv,
	glGetQueryObjectuiv,

	glFramebufferTexture2DEXT,glBindFramebufferEXT,glGenFramebuffersEXT,

	glGetQueryObjectui64vEXT,

	glProgramParameteri,

	NUMGLFUNC
};
typedef void (*glfp_t)(void);
static glfp_t glfp[NUMGLFUNC] = {0};

static int textsiz = 0;
static char *text = 0, *otext = 0, *ttext = 0, *line = 0, *badlinebits = 0;

typedef struct
{
	int i0, i1; //text index range:{i0<=i<i1} ('@' lines not included)
	int typ; //0=host,1=vert,2=geom,3=frag
	int cnt; //0,1,..
	int linofs; //absolute starting line (need for error line)
	int nxt; //index to next of same typ

	int geo_in;     //GL_POINTS,GL_LINES,GL_LINES_ADJACENCY,GL_TRIANGLES,GL_TRIANGLES_ADJACENCY
	int geo_out;    //GL_POINTS,GL_LINE_STRIP,GL_TRIANGLE_STRIP
	int geo_nverts; //1..1024

	char nam[64];
} tsec_t;
#define TSECMAX 256
static tsec_t otsec[TSECMAX], tsec[TSECMAX];
static int otsecn = 0, tsecn = 0;

#define MAXUSERTEX 256
static int captexsiz = 512;
typedef struct
{
	char nam[MAX_PATH];
	int tar, coltype, sizx, sizy, sizz;
} tex_t;
static tex_t tex[MAXUSERTEX+1/*+1 for font*/] = {0};
static char *gbmp = 0;
static int gbmpmal = 0;

#define SHADMAX 256


#if 0
!endif
#endif
