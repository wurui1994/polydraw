/* port/a64/winshim/windows.h —— 假的 `windows.h`。
 *
 * 为什么是"假头文件"而不是改 `pd/pd_head.h`：那一份 18KB 全是原文（GL 常量、
 * 那一大张 extern）。只要 `-I port/a64/winshim` 让 `#include <windows.h>` 能找到东西，
 * 原文一个字节都不用动。
 *
 * 这儿只放**类型与常量**，函数的实现在 `port/a64/pd_win_a64.c`：出图那条路上
 * 绝大多数窗口/菜单/对话框的调用都是空壳，只有计时、文件、剪贴板这些是真的。
 */
#ifndef PD_WINSHIM_H
#define PD_WINSHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef int             BOOL;
typedef unsigned char   BYTE;
typedef unsigned short  WORD;
typedef unsigned int    DWORD;
typedef unsigned int    UINT;
typedef int             INT;
typedef long            LONG;
typedef intptr_t        INT_PTR;
typedef uintptr_t       UINT_PTR;
typedef intptr_t        LONG_PTR;
typedef void           *LPVOID;
typedef const void     *LPCVOID;
typedef char           *LPSTR;
typedef const char     *LPCSTR;
typedef char            TCHAR;
typedef void           *HANDLE;
typedef void           *HWND;
typedef void           *HDC;
typedef void           *HGLRC;
typedef void           *HINSTANCE;
typedef void           *HMODULE;
typedef void           *HFONT;
typedef void           *HBITMAP;
typedef void           *HMENU;
typedef void           *HICON;
typedef void           *HCURSOR;
typedef void           *HBRUSH;
typedef void           *HMIDIOUT;
typedef UINT_PTR        WPARAM;
typedef LONG_PTR        LPARAM;
typedef LONG_PTR        LRESULT;
typedef DWORD          *LPDWORD;

#define CALLBACK
#define WINAPI
#define APIENTRY
#define CONST const
#define TRUE 1
#define FALSE 0
#define MAX_PATH 1024

typedef struct { LONG x, y; } POINT;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef union { struct { DWORD LowPart; LONG HighPart; }; long long QuadPart; } LARGE_INTEGER;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; char szCSDVersion[128]; } OSVERSIONINFO;

/* 这几个结构体原文只当"整块传进去"用，字段对不对不影响出图。 */
typedef struct { WORD nSize, nVersion; DWORD dwFlags; BYTE iPixelType, cColorBits, cRedBits, cRedShift,
	cGreenBits, cGreenShift, cBlueBits, cBlueShift, cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits,
	cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits, cStencilBits, cAuxBuffers, iLayerType,
	bReserved; DWORD dwLayerMask, dwVisibleMask, dwDamageMask; } PIXELFORMATDESCRIPTOR;
typedef struct { DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; LPCSTR lpstrFilter, lpstrCustomFilter;
	DWORD nMaxCustFilter, nFilterIndex; LPSTR lpstrFile; DWORD nMaxFile; LPSTR lpstrFileTitle;
	DWORD nMaxFileTitle; LPCSTR lpstrInitialDir, lpstrTitle; DWORD Flags; WORD nFileOffset, nFileExtension;
	LPCSTR lpstrDefExt; LPARAM lCustData; void *lpfnHook; LPCSTR lpTemplateName; } OPENFILENAME;
typedef struct { UINT style; LRESULT (CALLBACK *lpfnWndProc)(HWND,UINT,WPARAM,LPARAM); int cbClsExtra, cbWndExtra;
	HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName, lpszClassName; } WNDCLASS;

#include "pd_win_const.h"
#include "pd_win_api.h"

#endif
