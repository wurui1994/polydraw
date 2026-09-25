/* port/a64/pd_gl_imm.c —— 立即模式**攒批**（arm64 macOS 上那一刀，35 倍）。
 *
 * ## 为什么要它（两个探针量出来的，不是猜的）
 *
 * Apple 的 legacy GL 是拿 Metal 翻出来的：**绑了用户着色器之后，每一趟
 * `glBegin…glEnd` 都要重新发一趟 compute 变换内核**（采样里 81% 落在
 * `qglEnd -> glEnd_Exec -> gldEndPrimitiveBuffer -> AGX…dispatchThreads`）。
 * 同一份几何、同一个着色器，16384 个图元分开发是 **52.46 ms/帧**、攒成一趟是
 * **1.495 ms/帧**（`/tmp` 里那两份探针脚本，`--frames 30`）。
 * 没绑着色器时两者都是 1.0~1.3ms —— 所以这一格**只在有着色器时要命**，
 * 而语料里一半的脚本都带 `@v/@f`。原版在 Windows 的原生 GL 上不付这笔钱
 * （`ken\balls.pss` 参考 2.229 ms/帧，我们先前 25~42）。
 *
 * ## 形状：在**宿主调用那一层**拦，不动 polydraw 一个字节
 *
 * 脚本碰 GL 只有一条路：`myext[]` 那张表里的 `qgl*`。而 JIT 在编译期就知道
 * 被调的是哪个函数指针（`pd_a64_jitc.c` 的 USERFUNC 那一格），于是：
 *
 *   * `qglBegin` / `qglEnd` / `qglVertex` / `qglTexCoord` / `qglColor` / `qglNormal3d`
 *     -> 换成本文件这几格（攒进数组，三角化，**不发 GL**）；
 *   * **别的宿主调用**（glUniform / glbindtexture / cls / printf …）
 *     -> 调用前先发一格 `pd_imm_break()`：把攒着的交出去。
 *     于是"攒着的那批与它录下来的 GL 状态"永远配套 —— 状态一变就先画。
 *
 * 一帧收尾（`pd_a64_parm.c` 里脚本调用返回处）再冲一趟，polydraw 自己那些
 * GL（控制台文字、贴图、清屏）都在那之后，不会插到批中间。
 *
 * ## 三个边角
 *
 *   1. **半格图元被打断**（`glVertexAttrib` 那一族在 begin/end 之间）：
 *      `materialize()` 真开一趟 `glBegin`、把已收的顶点补发进去，这一格之后直通；
 *   2. 开过 `GL_COLOR_ARRAY` 之后"当前色"按规范是未定义的 —— 每趟冲完把
 *      当前色/纹理坐标/法向摆回去（脚本在 begin/end 外头设的那些照旧成立）；
 *   3. `PD_IMM=0` 关掉（A/B 对照用：同一个二进制跑两趟再逐份 join）。
 */
#include <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* polydraw.c 里那一族（`myext[]` 的落点，都是全局符号）。 */
extern double qglBegin (double);
extern double qglEnd (double);
extern double qglVertex2d (double, double);
extern double qglVertex3d (double, double, double);
extern double qglVertex4d (double, double, double, double);
extern double qglTexCoord2d (double, double);
extern double qglTexCoord3d (double, double, double);
extern double qglTexCoord4d (double, double, double, double);
extern double qglColor3d (double, double, double);
extern double qglColor4d (double, double, double, double);
extern double qglNormal3d (double, double, double);

/* 攒着的那一批（已经三角化/拆成线段了）。 */
#define PD_IMM_CAP  60000        /* 攒到这么多顶点就先冲一趟 */

static float *B_pos = 0, *B_col = 0, *B_tex = 0, *B_nrm = 0;
static long B_n = 0, B_cap = 0;
static int B_mode = -1;          /* GL_TRIANGLES / GL_LINES / GL_POINTS，-1 = 空 */

/* 这一格图元（`glBegin…glEnd` 之间）收下的顶点，`glEnd` 时才三角化。
   **长度不设上限**（按需 realloc）：`glBegin(GL_TRIANGLES)` 里塞五万个顶点那种写法
   一样要走批，掉回真立即模式反而更慢（量过：2.8ms vs 1.1ms）。 */
static float *P_pos = 0, *P_col = 0, *P_tex = 0, *P_nrm = 0;
static long P_n = 0, P_cap = 0;
static int P_mode = -1, P_in = 0, P_raw = 0;

/* "当前"那三格（立即模式的状态机）。 */
static float C_col[4] = {1,1,1,1}, C_tex[4] = {0,0,0,1}, C_nrm[3] = {0,0,1};

/* **这一帧攒不攒**：上一帧的图元数说话（见 `pd_imm_frame_end`）。 */
#define PD_IMM_MINPRIM 64
static long F_prims = 0;
static int F_on = 1;

static int pd_imm_want = -1;

int pd_imm_on (void)
{
	if (pd_imm_want < 0)
	{
		const char *s = getenv("PD_IMM");
		pd_imm_want = (s && (atol(s) == 0)) ? 0 : 1;
	}
	return(pd_imm_want);
}

/* 两组数组按需长大（每格顶点 4+4+4+3 个 float）。 */
static int agrow (float **pp, float **pc, float **pt, float **pnr, long *cap, long need)
{
	long c;
	if (need <= *cap) return(1);
	c = (*cap) ? (*cap)*2 : 4096;
	while (c < need) c *= 2;
	*pp  = (float *)realloc(*pp, (size_t)c*4*sizeof(float));
	*pc  = (float *)realloc(*pc, (size_t)c*4*sizeof(float));
	*pt  = (float *)realloc(*pt, (size_t)c*4*sizeof(float));
	*pnr = (float *)realloc(*pnr,(size_t)c*3*sizeof(float));
	if (!*pp || !*pc || !*pt || !*pnr) { *cap = 0; return(0); }
	*cap = c;
	return(1);
}
/* 把攒着的那一批交出去（客户端数组一趟 `glDrawArrays`）。 */
void pd_imm_flush (void)
{
	if ((B_n <= 0) || (B_mode < 0)) { B_n = 0; B_mode = -1; return; }
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(4,GL_FLOAT,0,B_pos);
	glEnableClientState(GL_COLOR_ARRAY);
	glColorPointer(4,GL_FLOAT,0,B_col);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(4,GL_FLOAT,0,B_tex);
	glEnableClientState(GL_NORMAL_ARRAY);
	glNormalPointer(GL_FLOAT,0,B_nrm);
	glDrawArrays((GLenum)B_mode,0,(GLsizei)B_n);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	/* 开过 COLOR_ARRAY 之后"当前色"按规范未定义 —— 摆回去（见头注第 2 条）。 */
	glColor4f(C_col[0],C_col[1],C_col[2],C_col[3]);
	glTexCoord4f(C_tex[0],C_tex[1],C_tex[2],C_tex[3]);
	glNormal3f(C_nrm[0],C_nrm[1],C_nrm[2]);
	B_n = 0; B_mode = -1;
}

/* 把这一格图元的第 i 个顶点推进攒批里。 */
static void bput (long i)
{
	float *d, *s;
	if (!agrow(&B_pos,&B_col,&B_tex,&B_nrm,&B_cap,B_n+1)) return;
	d = &B_pos[B_n*4]; s = &P_pos[i*4]; d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
	d = &B_col[B_n*4]; s = &P_col[i*4]; d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
	d = &B_tex[B_n*4]; s = &P_tex[i*4]; d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
	d = &B_nrm[B_n*3]; s = &P_nrm[i*3]; d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
	B_n++;
}

/* 这一格图元三角化（线那一族拆成独立线段）并推进攒批。 */
static void emit_prim (void)
{
	long i;
	int tgt;
	switch(P_mode)
	{
		case GL_POINTS: tgt = GL_POINTS; break;
		case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: tgt = GL_LINES; break;
		default: tgt = GL_TRIANGLES; break;
	}
	if ((B_mode >= 0) && (B_mode != tgt)) pd_imm_flush();
	B_mode = tgt;
	switch(P_mode)
	{
		case GL_POINTS: for(i=0;i<P_n;i++) bput(i); break;
		case GL_LINES: for(i=0;i+1<P_n;i+=2) { bput(i); bput(i+1); } break;
		case GL_LINE_STRIP: for(i=0;i+1<P_n;i++) { bput(i); bput(i+1); } break;
		case GL_LINE_LOOP:
			for(i=0;i+1<P_n;i++) { bput(i); bput(i+1); }
			if (P_n >= 3) { bput(P_n-1); bput(0); }
			break;
		case GL_TRIANGLES: for(i=0;i+2<P_n;i+=3) { bput(i); bput(i+1); bput(i+2); } break;
		/* 条带的朝向要保住：奇数格换前两个顶点（GL 的规矩）。 */
		case GL_TRIANGLE_STRIP:
			for(i=0;i+2<P_n;i++)
			{
				if (i&1) { bput(i+1); bput(i); } else { bput(i); bput(i+1); }
				bput(i+2);
			}
			break;
		case GL_TRIANGLE_FAN: case GL_POLYGON:
			for(i=1;i+1<P_n;i++) { bput(0); bput(i); bput(i+1); }
			break;
		case GL_QUADS:
			for(i=0;i+3<P_n;i+=4)
			{ bput(i); bput(i+1); bput(i+2); bput(i); bput(i+2); bput(i+3); }
			break;
		/* 四边条带的顶点次序是 (0,1,3,2) —— 拆成 (0,1,3) 与 (0,3,2)。 */
		case GL_QUAD_STRIP:
			for(i=0;i+3<P_n;i+=2)
			{ bput(i); bput(i+1); bput(i+3); bput(i); bput(i+3); bput(i+2); }
			break;
		default: break;
	}
	P_n = 0; P_mode = -1;
	if (B_n >= PD_IMM_CAP) pd_imm_flush();
}
/* 半格图元被外来调用打断：真开一趟 `glBegin`、把已收的顶点补发进去，之后直通。 */
static void materialize (void)
{
	long i;
	pd_imm_flush();
	qglBegin((double)P_mode);
	for(i=0;i<P_n;i++)
	{
		glColor4fv(&P_col[i*4]);
		glTexCoord4fv(&P_tex[i*4]);
		glNormal3fv(&P_nrm[i*3]);
		glVertex4fv(&P_pos[i*4]);
	}
	P_n = 0; P_raw = 1;
}

/* **除了这几格以外**的宿主调用之前都要来一趟（JIT 在调用点发的）：
   状态要变了，攒着的那批得先按旧状态画掉。 */
void pd_imm_break (void)
{
	if (!pd_imm_on()) return;
	if (P_raw) return;                  /* 真图元开着：立即模式里允许这些调用 */
	if (P_in) { materialize(); return; }
	pd_imm_flush();
}

double pd_imm_begin (double mode)
{
	int m = (int)mode;
	if (!pd_imm_on()) return(qglBegin(mode));
	F_prims++;
	P_in = 1; P_n = 0; P_mode = m; P_raw = 0;
	/* 不认的 mode、或者**这一帧图元太少不值得攒**：一律走原路。 */
	if (!F_on || (m < GL_POINTS) || (m > GL_POLYGON)) { P_raw = 1; return(qglBegin(mode)); }
	return(0.0);
}

double pd_imm_end (double d)
{
	if (!pd_imm_on()) return(qglEnd(d));
	P_in = 0;
	if (P_raw) { P_raw = 0; return(qglEnd(d)); }
	emit_prim();
	return(0.0);
}

static void addv (double x, double y, double z, double w)
{
	float *d;
	if (P_raw)
	{
		glColor4fv(C_col);
		glTexCoord4fv(C_tex);
		glNormal3fv(C_nrm);
		glVertex4d(x,y,z,w);
		return;
	}
	if (!agrow(&P_pos,&P_col,&P_tex,&P_nrm,&P_cap,P_n+1)) { materialize(); addv(x,y,z,w); return; }
	d = &P_pos[P_n*4]; d[0] = (float)x; d[1] = (float)y; d[2] = (float)z; d[3] = (float)w;
	d = &P_col[P_n*4]; d[0] = C_col[0]; d[1] = C_col[1]; d[2] = C_col[2]; d[3] = C_col[3];
	d = &P_tex[P_n*4]; d[0] = C_tex[0]; d[1] = C_tex[1]; d[2] = C_tex[2]; d[3] = C_tex[3];
	d = &P_nrm[P_n*3]; d[0] = C_nrm[0]; d[1] = C_nrm[1]; d[2] = C_nrm[2];
	P_n++;
}
double pd_imm_vertex2d (double x, double y)                       { addv(x,y,0.0,1.0); return(0.0); }
double pd_imm_vertex3d (double x, double y, double z)              { addv(x,y,z,1.0);   return(0.0); }
double pd_imm_vertex4d (double x, double y, double z, double w)    { addv(x,y,z,w);     return(0.0); }

double pd_imm_texcoord2d (double u, double v)
{ C_tex[0] = (float)u; C_tex[1] = (float)v; C_tex[2] = 0.f; C_tex[3] = 1.f;
  if (P_raw || !pd_imm_on()) glTexCoord4fv(C_tex); return(0.0); }
double pd_imm_texcoord3d (double u, double v, double s)
{ C_tex[0] = (float)u; C_tex[1] = (float)v; C_tex[2] = (float)s; C_tex[3] = 1.f;
  if (P_raw || !pd_imm_on()) glTexCoord4fv(C_tex); return(0.0); }
double pd_imm_texcoord4d (double u, double v, double s, double t)
{ C_tex[0] = (float)u; C_tex[1] = (float)v; C_tex[2] = (float)s; C_tex[3] = (float)t;
  if (P_raw || !pd_imm_on()) glTexCoord4fv(C_tex); return(0.0); }

double pd_imm_color3d (double r, double g, double b)
{ C_col[0] = (float)r; C_col[1] = (float)g; C_col[2] = (float)b; C_col[3] = 1.f;
  if (P_raw || !pd_imm_on()) glColor4fv(C_col); return(0.0); }
double pd_imm_color4d (double r, double g, double b, double a)
{ C_col[0] = (float)r; C_col[1] = (float)g; C_col[2] = (float)b; C_col[3] = (float)a;
  if (P_raw || !pd_imm_on()) glColor4fv(C_col); return(0.0); }
double pd_imm_normal3d (double x, double y, double z)
{ C_nrm[0] = (float)x; C_nrm[1] = (float)y; C_nrm[2] = (float)z;
  if (P_raw || !pd_imm_on()) glNormal3fv(C_nrm); return(0.0); }

/**
 * **这个宿主函数会动 GL 状态吗**（JIT 拿它决定要不要在调用点发 `pd_imm_break`）。
 *
 * 判据是 `myext[]` 里的**名字**：`GL…` 那一族全算，外加两个不叫 GL 的
 * （`PRINTG` 真在画布上画字、`SETFOV` 改投影矩阵）。别的（`KLOCK`/`NOISE`/`PRINTF`/
 * `RND`/`SRAND`/`PLAYNOTE`/`MOUNTZIP`/`XRES`…）一个都不碰 GL。
 *
 * 为什么要这张表：先前"除了攒批那几格以外一律 break"，于是 `tigrou/snake stars.pss`
 * 那种**在 `glBegin…glEnd` 里调脚本函数**（`align(…)`，每个顶点一次）的写法
 * 每个顶点都要 `materialize()` 一趟 —— 2.6 -> 22.9 ms/帧，比不攒批还慢 9 倍。
 * 脚本函数不在这张表里，所以现在不打断；它**自己**要是调了 GL，那一格调用会自己 break
 * （JIT 编出来的那份在编译期判、解释器那份走 `pd_imm_call`）。
 */
int pd_imm_isgl (const char *nm)
{
	if (!nm) return(1);                       /* 认不出名字：保守起见当会动 */
	if (((nm[0] == 'G') || (nm[0] == 'g')) && ((nm[1] == 'L') || (nm[1] == 'l'))) return(1);
	if (!strcasecmp(nm,"PRINTG")) return(1);
	if (!strcasecmp(nm,"SETFOV")) return(1);
	return(0);
}

/**
 * **编译期换落点**：JIT 在 USERFUNC 那一格问一句"这个宿主函数有没有替代实现"。
 * 回 0 = 没有（那一格调用之前要不要 break 由 `pd_imm_isgl` 说）。
 */
void *pd_imm_hook (void *fn)
{
	if (!pd_imm_on()) return(0);
	if (fn == (void *)qglBegin)      return((void *)pd_imm_begin);
	if (fn == (void *)qglEnd)        return((void *)pd_imm_end);
	if (fn == (void *)qglVertex2d)   return((void *)pd_imm_vertex2d);
	if (fn == (void *)qglVertex3d)   return((void *)pd_imm_vertex3d);
	if (fn == (void *)qglVertex4d)   return((void *)pd_imm_vertex4d);
	if (fn == (void *)qglTexCoord2d) return((void *)pd_imm_texcoord2d);
	if (fn == (void *)qglTexCoord3d) return((void *)pd_imm_texcoord3d);
	if (fn == (void *)qglTexCoord4d) return((void *)pd_imm_texcoord4d);
	if (fn == (void *)qglColor3d)    return((void *)pd_imm_color3d);
	if (fn == (void *)qglColor4d)    return((void *)pd_imm_color4d);
	if (fn == (void *)qglNormal3d)   return((void *)pd_imm_normal3d);
	return(0);
}

/**
 * **一帧收尾**（`kasm87c`/`kasm87cp` 返回处叫）：交出攒着的那批，并且按这一帧的图元数
 * 定**下一帧还攒不攒**。
 *
 * 为什么要这一格：攒批只有在"图元多"的时候赚（Apple 的 legacy GL 为每趟
 * `glBegin…glEnd` 重发一趟 compute 内核）。图元少的脚本反倒亏 —— 一趟
 * `glDrawArrays` + 客户端数组那一套要让驱动换一种顶点布局，量出来
 * `creepers_asm` 2.14 -> 3.37、`interference` 2.49 -> 3.20 ms/帧。
 * 所以这一帧的图元数 < `PD_IMM_MINPRIM` 就下一帧整帧直通（行为与 `PD_IMM=0` 相同）。
 */
void pd_imm_frame_end (void)
{
	if (!pd_imm_on()) return;
	pd_imm_flush();
	F_on = (F_prims >= PD_IMM_MINPRIM);
	F_prims = 0;
}

/**
 * **解释器那条路**（`pd_a64_run.c` 的 USERFUNC 一档，`tools/mkrun.mjs` 插的那一句）用的：
 * 换落点，换不了就先把攒着的交出去。与 JIT 那边的差别是它在**运行期**问，
 * 所以顺手把 `break` 也办了。
 *
 * 为什么解释器这条路也得过一趟：脚本自己的函数（`align(…)` 那种）走的是解释器，
 * 它里头要是调了 GL，直接调真 `qglVertex` 就没有 `glBegin` 与之配套了。
 */
void *pd_imm_call (void *fn)
{
	void *h = pd_imm_hook(fn);
	if (h) return(h);
	pd_imm_break();
	return(fn);
}

