/* port/a64/pd_win_a64.c —— 那 81 个 win32 函数在 arm64 macOS 上的实现。
 *
 * 分三档（口径：**出图那条路要真的，编辑器那一半空壳**）：
 *   * **真的**：计时（QueryPerformanceCounter -> clock_gettime）、Sleep、
 *     本地时间、GetModuleFileName、ini 那三个（落到一份文本文件）；
 *   * **空壳回成功**：窗口、菜单、光标、字体、GDI、对话框、消息循环 ——
 *     `polydraw.c` 的编辑器那一半靠它们，出图不靠；
 *   * **空壳回失败**：MIDI、命名管道、CreateProcess、线程。
 *
 * GL 上下文（`wgl*` 四个 + ChoosePixelFormat/SetPixelFormat/SwapBuffers）在
 * `pd_gl_cgl.c` 里另写 —— 那一格是真的（CGL 离屏上下文）。
 *
 * 为什么这一份能存在：`polydraw.c` 原文一个字节都没改，缺的符号在链接期补。
 */
#include <windows.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── 真的：计时 ──
 * QueryPerformanceFrequency 报 1e9，Counter 报纳秒 —— 于是 polydraw 的 klock()
 * 直接就是秒，精度比 Windows 那边（通常 1e7 或 TSC）还高一档。 */
BOOL QueryPerformanceFrequency (LARGE_INTEGER *p) { p->QuadPart = 1000000000LL; return(1); }
BOOL QueryPerformanceCounter (LARGE_INTEGER *p)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC,&ts);
	p->QuadPart = (long long)ts.tv_sec*1000000000LL + (long long)ts.tv_nsec;
	return(1);
}
void Sleep (DWORD ms) { usleep((unsigned)ms*1000u); }

static void pd_fill_time (SYSTEMTIME *t, struct tm *tm, long ms)
{
	t->wYear = (WORD)(tm->tm_year+1900); t->wMonth = (WORD)(tm->tm_mon+1);
	t->wDayOfWeek = (WORD)tm->tm_wday;   t->wDay = (WORD)tm->tm_mday;
	t->wHour = (WORD)tm->tm_hour;        t->wMinute = (WORD)tm->tm_min;
	t->wSecond = (WORD)tm->tm_sec;       t->wMilliseconds = (WORD)ms;
}
void GetLocalTime (SYSTEMTIME *t)
{
	struct timespec ts; struct tm tm;
	clock_gettime(CLOCK_REALTIME,&ts); localtime_r(&ts.tv_sec,&tm);
	pd_fill_time(t,&tm,ts.tv_nsec/1000000);
}
void GetSystemTime (SYSTEMTIME *t)
{
	struct timespec ts; struct tm tm;
	clock_gettime(CLOCK_REALTIME,&ts); gmtime_r(&ts.tv_sec,&tm);
	pd_fill_time(t,&tm,ts.tv_nsec/1000000);
}
BOOL GetVersionEx (OSVERSIONINFO *v)
{
	/* polydraw 只拿它判"是不是 win9x"（决定某几条 GL 路子）。报个够新的版本号。 */
	v->dwMajorVersion = 10; v->dwMinorVersion = 0; v->dwBuildNumber = 0;
	v->dwPlatformId = 2 /*VER_PLATFORM_WIN32_NT*/; v->szCSDVersion[0] = 0;
	return(1);
}

/* ── 真的：自己的可执行文件路径（polydraw 拿它找 .ini 与 ken/ 那批脚本） ── */
extern int _NSGetExecutablePath (char *, unsigned int *);
DWORD GetModuleFileName (HMODULE h, LPSTR buf, DWORD n)
{
	unsigned int sz = (unsigned int)n;
	(void)h;
	if (_NSGetExecutablePath(buf,&sz) != 0) { if (n) buf[0] = 0; return(0); }
	return((DWORD)strlen(buf));
}
void ExitProcess (UINT c) { exit((int)c); }
HANDLE GetCurrentThread (void) { return((HANDLE)1); }

/* ── 真的（够用的那种）：ini ──
 * 格式照 win32：`[节]` 一行，`键=值` 一行。polydraw 只用它记窗口大小与几个开关。 */
static FILE *pd_ini_seek (LPCSTR file, LPCSTR sect)
{
	char line[1024];
	FILE *f = fopen(file,"rb");
	if (!f) return(0);
	while (fgets(line,sizeof(line),f))
	{
		char *p = line;
		while ((*p == ' ') || (*p == '\t')) p++;
		if (*p != '[') continue;
		p++;
		if (!strncasecmp(p,sect,strlen(sect)) && (p[strlen(sect)] == ']')) return(f);
	}
	fclose(f); return(0);
}
DWORD GetPrivateProfileString (LPCSTR sect, LPCSTR key, LPCSTR def, LPSTR out, DWORD n, LPCSTR file)
{
	char line[1024];
	FILE *f = pd_ini_seek(file,sect);
	if (f)
	{
		size_t kl = strlen(key);
		while (fgets(line,sizeof(line),f))
		{
			char *p = line, *e;
			while ((*p == ' ') || (*p == '\t')) p++;
			if (*p == '[') break;
			if (strncasecmp(p,key,kl) || (p[kl] != '=')) continue;
			p += kl+1;
			e = p+strlen(p);
			while ((e > p) && ((e[-1] == '\n') || (e[-1] == '\r') || (e[-1] == ' '))) e--;
			*e = 0;
			fclose(f);
			strncpy(out,p,n); if (n) out[n-1] = 0;
			return((DWORD)strlen(out));
		}
		fclose(f);
	}
	strncpy(out,def ? def : "",n); if (n) out[n-1] = 0;
	return((DWORD)strlen(out));
}
UINT GetPrivateProfileInt (LPCSTR sect, LPCSTR key, INT def, LPCSTR file)
{
	char buf[64];
	if (!GetPrivateProfileString(sect,key,"",buf,sizeof(buf),file)) return((UINT)def);
	return((UINT)atoi(buf));
}
/* 写：整份读进来、替换那一行、整份写回去。ini 只有几十行，图省事。 */
BOOL WritePrivateProfileString (LPCSTR sect, LPCSTR key, LPCSTR val, LPCSTR file)
{
	char *buf = 0, line[1024];
	long cap = 1<<16, len = 0;
	int insect = 0, done = 0;
	FILE *f;
	buf = (char *)malloc(cap); if (!buf) return(0);
	f = fopen(file,"rb");
	if (f)
	{
		while (fgets(line,sizeof(line),f))
		{
			char *p = line;
			while ((*p == ' ') || (*p == '\t')) p++;
			if (*p == '[')
			{
				if (insect && !done) { len += sprintf(&buf[len],"%s=%s\n",key,val); done = 1; }
				insect = (!strncasecmp(p+1,sect,strlen(sect)) && (p[1+strlen(sect)] == ']'));
			}
			else if (insect && !strncasecmp(p,key,strlen(key)) && (p[strlen(key)] == '='))
			{
				if (!done) { len += sprintf(&buf[len],"%s=%s\n",key,val); done = 1; }
				continue;
			}
			if (len+1024 > cap) { cap <<= 1; buf = (char *)realloc(buf,cap); if (!buf) { fclose(f); return(0); } }
			len += sprintf(&buf[len],"%s",line);
		}
		fclose(f);
	}
	if (!done)
	{
		if (!insect) len += sprintf(&buf[len],"[%s]\n",sect);
		len += sprintf(&buf[len],"%s=%s\n",key,val);
	}
	f = fopen(file,"wb"); if (!f) { free(buf); return(0); }
	fwrite(buf,1,(size_t)len,f); fclose(f); free(buf);
	return(1);
}

/* ── 空壳：窗口与消息循环 ──
 * 出图那条路（`bench/a64/render.c`）自己驱动帧循环，压根不进 polydraw 的
 * `WinMain`；这些只是为了让链接过去，并且"看起来像成功"以免 polydraw 提早 return。
 * GUI 那一步要真接本机窗口时，这一层就是接口所在（把 NSEvent 翻成 WM_*）。 */
static HWND pd_fake_hwnd = (HWND)0x1;

HWND CreateWindow (LPCSTR a, LPCSTR b, DWORD c, int d, int e, int f, int g, HWND h, HMENU i, HINSTANCE j, LPVOID k)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k; return(pd_fake_hwnd); }
HWND CreateWindowEx (DWORD x, LPCSTR a, LPCSTR b, DWORD c, int d, int e, int f, int g, HWND h, HMENU i, HINSTANCE j, LPVOID k)
{ (void)x; return(CreateWindow(a,b,c,d,e,f,g,h,i,j,k)); }
BOOL DestroyWindow (HWND h) { (void)h; return(1); }
BOOL ShowWindow (HWND h, int c) { (void)h;(void)c; return(1); }
BOOL UpdateWindow (HWND h) { (void)h; return(1); }
BOOL MoveWindow (HWND h, int a, int b, int c, int d, BOOL e) { (void)h;(void)a;(void)b;(void)c;(void)d;(void)e; return(1); }
BOOL GetWindowRect (HWND h, RECT *r) { (void)h; r->left = r->top = 0; r->right = 640; r->bottom = 480; return(1); }
BOOL GetClientRect (HWND h, RECT *r) { return(GetWindowRect(h,r)); }
/* `GetWindowText` 在 pd_main_a64.c 里 —— 它就是那个"假编辑框"：回脚本正文。 */
BOOL SetWindowText (HWND h, LPCSTR s) { (void)h;(void)s; return(1); }
LONG_PTR SetWindowLong (HWND h, int i, LONG_PTR v) { (void)h;(void)i;(void)v; return(0); }
HWND SetFocus (HWND h) { return(h); }
BOOL ClientToScreen (HWND h, POINT *p) { (void)h;(void)p; return(1); }
BOOL GetCursorPos (POINT *p) { p->x = p->y = 0; return(1); }
BOOL PeekMessage (MSG *m, HWND h, UINT a, UINT b, UINT c) { (void)m;(void)h;(void)a;(void)b;(void)c; return(0); }
BOOL TranslateMessage (const MSG *m) { (void)m; return(0); }
LRESULT DispatchMessage (const MSG *m) { (void)m; return(0); }
LRESULT SendMessage (HWND h, UINT m, WPARAM w, LPARAM l) { (void)h;(void)m;(void)w;(void)l; return(0); }
LRESULT DefWindowProc (HWND h, UINT m, WPARAM w, LPARAM l) { (void)h;(void)m;(void)w;(void)l; return(0); }
LRESULT CallWindowProc (void *p, HWND h, UINT m, WPARAM w, LPARAM l) { (void)p;(void)h;(void)m;(void)w;(void)l; return(0); }
void PostQuitMessage (int c) { (void)c; }
WORD RegisterClass (const WNDCLASS *c) { (void)c; return(1); }
UINT RegisterWindowMessage (LPCSTR s) { (void)s; return(0xC000); }
HCURSOR LoadCursor (HINSTANCE h, LPCSTR s) { (void)h;(void)s; return((HCURSOR)1); }
HICON LoadIcon (HINSTANCE h, LPCSTR s) { (void)h;(void)s; return((HICON)1); }
BOOL InvalidateRect (HWND h, const RECT *r, BOOL e) { (void)h;(void)r;(void)e; return(1); }
BOOL IsWindow (HWND h) { return(h != 0); }
BOOL IsDialogMessage (HWND h, MSG *m) { (void)h;(void)m; return(0); }
BOOL SystemParametersInfo (UINT a, UINT b, void *p, UINT c)
{ (void)a;(void)b;(void)c; if (p) { RECT *r = (RECT *)p; r->left = r->top = 0; r->right = 1920; r->bottom = 1080; } return(1); }

/* ── 空壳：菜单、光标、字体、GDI、对话框 ── */
HMENU LoadMenuIndirect (const void *p) { (void)p; return((HMENU)1); }
BOOL SetMenu (HWND h, HMENU m) { (void)h;(void)m; return(1); }
BOOL CheckMenuItem (HMENU m, UINT i, UINT f) { (void)m;(void)i;(void)f; return(1); }
BOOL CreateCaret (HWND h, HBITMAP b, int w, int t) { (void)h;(void)b;(void)w;(void)t; return(1); }
BOOL DestroyCaret (void) { return(1); }
BOOL ShowCaret (HWND h) { (void)h; return(1); }
BOOL HideCaret (HWND h) { (void)h; return(1); }
BOOL SetCaretPos (int x, int y) { (void)x;(void)y; return(1); }
HFONT CreateFont (int a, int b, int c, int d, int e, DWORD f, DWORD g, DWORD h, DWORD i, DWORD j, DWORD k, DWORD l, DWORD m, LPCSTR n)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;(void)m;(void)n; return((HFONT)1); }
HBRUSH GetStockObject (int i) { (void)i; return((HBRUSH)1); }
BOOL DeleteObject (HANDLE h) { (void)h; return(1); }
DWORD SetBkColor (HDC d, DWORD c) { (void)d;(void)c; return(0); }
DWORD SetTextColor (HDC d, DWORD c) { (void)d;(void)c; return(0); }
BOOL ChooseFont (void *p) { (void)p; return(0); }
BOOL FindText (void *p) { (void)p; return(0); }
HWND ReplaceText (FINDREPLACE *p) { (void)p; return(0); }
BOOL GetOpenFileName (OPENFILENAME *p) { (void)p; return(0); }
BOOL GetSaveFileName (OPENFILENAME *p) { (void)p; return(0); }
void MessageBeep (UINT t) { (void)t; }
int MessageBox (HWND h, LPCSTR txt, LPCSTR cap, UINT t)
{ (void)h;(void)t; fprintf(stderr,"[MessageBox] %s: %s\n",cap ? cap : "",txt ? txt : ""); return(IDCANCEL); }

/* ── 空壳回失败：线程、管道、进程、MIDI ──
 * 出图是单线程的；polydraw 那几处都判返回值，回失败它就走单线程那条路。 */
/* 这一格是"脚本跑死了"的看门狗（`pd_script.c:554-568`）。协议比看着简单：
   **脚本本来就是主线程自己调的**（`:564` 的 `safeevalfunc()`），那条线程只负责
   "超时就报 stuck"。所以不需要真线程，只要：
     * `_beginthreadex` 回一个**非 0** 的假句柄 —— 不然 `gthand` 一直是 0，
       每帧都重新走一遍创建；
     * `WaitForSingleObject` 回 `WAIT_OBJECT_0`（0）—— 回 WAIT_TIMEOUT 的话
       第一帧之后就 `gshaderstuck = 1`，脚本再也不跑了。
   踩过：先前这两个回 0 / WAIT_TIMEOUT，于是**脚本只在第 0 帧画了一次**，
   后面每帧被 glClear 清成空的 —— 查了半天"画面全黑"的最后一格就是这儿。
   代价：没有看门狗了，脚本死循环会把整个进程挂住（出图那条路可以接受）。 */
static int pd_ev = 0;
HANDLE CreateEvent (void *a, BOOL b, BOOL c, LPCSTR d)
{ (void)a;(void)b;(void)c;(void)d; return((HANDLE)(intptr_t)(++pd_ev)); }
BOOL SetEvent (HANDLE h) { (void)h; return(1); }
BOOL ResetEvent (HANDLE h) { (void)h; return(1); }
BOOL CloseHandle (HANDLE h) { (void)h; return(1); }
DWORD WaitForSingleObject (HANDLE h, DWORD ms) { (void)h;(void)ms; return(0 /*WAIT_OBJECT_0*/); }
UINT_PTR _beginthreadex (void *a, unsigned b, unsigned (*c)(void *), void *d, unsigned e, unsigned *f)
{ (void)a;(void)b;(void)c;(void)d;(void)e; if (f) *f = 0; return(1); }
HANDLE CreateNamedPipe (LPCSTR a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, void *h)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; return(INVALID_HANDLE_VALUE); }
BOOL ConnectNamedPipe (HANDLE h, void *o) { (void)h;(void)o; return(0); }
BOOL DisconnectNamedPipe (HANDLE h) { (void)h; return(0); }
BOOL FlushFileBuffers (HANDLE h) { (void)h; return(0); }
BOOL WriteFile (HANDLE h, LPCVOID p, DWORD n, LPDWORD w, void *o)
{ (void)h;(void)p;(void)o; if (w) *w = 0; (void)n; return(0); }
BOOL CreateProcess (LPCSTR a, LPSTR b, void *c, void *d, BOOL e, DWORD f, LPVOID g, LPCSTR h, void *i, void *j)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j; return(0); }
INT_PTR _spawnlp (int m, const char *a, const char *b, ...) { (void)m;(void)a;(void)b; return(-1); }
int MultiByteToWideChar (UINT cp, DWORD f, LPCSTR s, int n, void *o, int on)
{
	/* polydraw 只用它把文件名转成宽字符喂 GetOpenFileName —— 那个是空壳，所以
	   这儿只要长度对得上。ASCII 逐字节抬成 16 位。 */
	int i, len = (n < 0) ? (int)strlen(s)+1 : n;
	unsigned short *w = (unsigned short *)o;
	(void)cp; (void)f;
	if (!o || !on) return(len);
	if (len > on) len = on;
	for(i=0;i<len;i++) w[i] = (unsigned char)s[i];
	return(len);
}
UINT midiOutOpen (HMIDIOUT *h, UINT d, DWORD a, DWORD b, DWORD c)
{ (void)d;(void)a;(void)b;(void)c; if (h) *h = 0; return(1 /*非 MMSYSERR_NOERROR*/); }
UINT midiOutClose (HMIDIOUT h) { (void)h; return(1); }
UINT midiOutShortMsg (HMIDIOUT h, DWORD m) { (void)h;(void)m; return(1); }
