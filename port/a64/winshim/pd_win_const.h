/* port/a64/winshim/pd_win_const.h —— win32 的那堆常量（值照真的 win32 抄，不是随便编的）。
 *
 * 为什么要用真值：`pd_win.c` 的 `WndProc` 是拿 `WM_*` 做 switch 的，将来真接上
 * 本机窗口（GUI 那一步）时，我们的事件循环要往里喂这些号 —— 值不真就得两头都改。
 * `PFD_*` / `EM_*` 同理。
 */
#ifndef PD_WIN_CONST_H
#define PD_WIN_CONST_H

/* 窗口消息 */
#define WM_DESTROY        0x0002
#define WM_SIZE           0x0005
#define WM_PAINT          0x000F
#define WM_CLOSE          0x0010
#define WM_QUIT           0x0012
#define WM_SETFONT        0x0030
#define WM_SETTEXT        0x000C
#define WM_ACTIVATEAPP    0x001C
#define WM_SETFOCUS       0x0007
#define WM_KILLFOCUS      0x0008
#define WM_KEYDOWN        0x0100
#define WM_KEYUP          0x0101
#define WM_CHAR           0x0102
#define WM_SYSCHAR        0x0106
#define WM_COMMAND        0x0111
#define WM_LBUTTONDOWN    0x0201
#define WM_LBUTTONUP      0x0202
#define WM_RBUTTONDOWN    0x0204
#define WM_RBUTTONUP      0x0205
#define WM_MBUTTONDOWN    0x0207
#define WM_MBUTTONUP      0x0208
#define PM_REMOVE         0x0001
#define MK_LBUTTON        0x0001
#define SIZE_MINIMIZED    1
#define SIZE_MAXHIDE      4

/* 虚拟键 */
#define VK_RETURN         0x0D
#define VK_INSERT         0x2D
#define VK_F1             0x70
#define VK_F3             0x72

/* 窗口样式 */
#define WS_CHILD          0x40000000
#define WS_VISIBLE        0x10000000
#define WS_CAPTION        0x00C00000
#define WS_SYSMENU        0x00080000
#define WS_SIZEBOX        0x00040000
#define WS_MINIMIZEBOX    0x00020000
#define WS_MAXIMIZEBOX    0x00010000
#define WS_VSCROLL        0x00200000
#define WS_HSCROLL        0x00100000
#define WS_EX_CLIENTEDGE  0x00000200
#define WS_EX_WINDOWEDGE  0x00000100
#define CS_OWNDC          0x0020
#define GWL_WNDPROC       (-4)
#define SW_HIDE           0
#define SW_SHOWNORMAL     1
#define SW_NORMAL         1
#define SW_SHOW           5
#define SW_MAXIMIZE       3
#define SPI_GETWORKAREA   0x0030

/* 编辑框（编辑器那一半用；出图用不着，但值要真） */
#define EM_GETSEL               0x00B0
#define EM_SETSEL               0x00B1
#define EM_GETRECT              0x00B2
#define EM_SETRECTNP            0x00B4
#define EM_SCROLLCARET          0x00B7
#define EM_GETMODIFY            0x00B8
#define EM_SETMODIFY            0x00B9
#define EM_LINESCROLL           0x00B6
#define EM_GETLINE              0x00C4
#define EM_LIMITTEXT            0x00C5
#define EM_LINEINDEX            0x00BB
#define EM_LINELENGTH           0x00C1
#define EM_REPLACESEL           0x00C2
#define EM_LINEFROMCHAR         0x00C9
#define EM_POSFROMCHAR          0x00D6
#define EM_GETFIRSTVISIBLELINE  0x00CE
#define EN_UPDATE               0x0400
#define ES_MULTILINE      0x0004
#define ES_AUTOVSCROLL    0x0040
#define ES_AUTOHSCROLL    0x0080
#define ES_NOHIDESEL      0x0100
#define ES_READONLY       0x0800
#define ES_WANTRETURN     0x1000
#define ES_RIGHT          0x0002

/* 菜单与消息框 */
#define MF_CHECKED        0x0008
#define MF_POPUP          0x0010
#define MF_SEPARATOR      0x0800
#define MF_END            0x0080
#define MB_OK             0x0000
#define MB_YESNOCANCEL    0x0003
#define IDYES             6
#define IDNO              7
#define IDCANCEL          2
#define IDC_ARROW         ((LPCSTR)32512)
#define IDI_APPLICATION   ((LPCSTR)32512)
#define BLACK_BRUSH       4

/* 字体与通用对话框 */
#define FW_NORMAL         400
#define DEFAULT_CHARSET   1
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY   0
#define DEFAULT_PITCH     0
#define CF_SCREENFONTS    0x00000001
#define CF_INITTOLOGFONTSTRUCT 0x00000040
#define CF_FIXEDPITCHONLY 0x00004000
#define CF_NOSTYLESEL     0x00100000
#define OFN_FILEMUSTEXIST 0x00001000
#define OFN_PATHMUSTEXIST 0x00000800
#define OFN_HIDEREADONLY  0x00000004
#define OFN_OVERWRITEPROMPT 0x00000002
#define OFN_EXPLORER      0x00080000
#define FR_DOWN           0x00000001
#define FR_WHOLEWORD      0x00000002
#define FR_MATCHCASE      0x00000004
#define FR_FINDNEXT       0x00000008
#define FR_REPLACE        0x00000010
#define FR_REPLACEALL     0x00000020
#define FR_DIALOGTERM     0x00000040
#define FINDMSGSTRING     "commdlg_FindReplace"
#define CP_ACP            0

/* 像素格式 */
#define PFD_DRAW_TO_WINDOW 0x00000004
#define PFD_SUPPORT_OPENGL 0x00000020
#define PFD_DOUBLEBUFFER   0x00000001
#define PFD_TYPE_RGBA      0
#define PFD_MAIN_PLANE     0

/* 线程 / 管道 / 进程 */
#define INFINITE            0xFFFFFFFF
#define WAIT_TIMEOUT        0x00000102
#define INVALID_HANDLE_VALUE ((HANDLE)(-1))
#define PIPE_ACCESS_DUPLEX  0x00000003
#define PIPE_TYPE_BYTE      0x00000000
#define PIPE_READMODE_BYTE  0x00000000
#define PIPE_WAIT           0x00000000
#define PIPE_UNLIMITED_INSTANCES 255
#define CREATE_NEW_CONSOLE  0x00000010
#define _P_NOWAIT           1
#define EXCEPTION_EXECUTE_HANDLER 1

/* MIDI */
#define MIDI_MAPPER        (-1)
#define MMSYSERR_NOERROR   0

/* GL：Windows 的 `gl/gl.h` 有这个别名，macOS 的没有（值一样）。 */
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

/* 这几个只当"某种句柄/结构"用，出图那条路不碰里头的字段。 */
typedef struct { DWORD cb; LPSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize,
	dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2;
	BYTE *lpReserved2; HANDLE hStdInput, hStdOutput, hStdError; } STARTUPINFO;
typedef struct { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;
typedef struct { DWORD ExceptionCode, ExceptionFlags; void *ExceptionRecord, *ExceptionAddress;
	DWORD NumberParameters; UINT_PTR ExceptionInformation[15]; } EXCEPTION_RECORD;
typedef struct { DWORD ContextFlags; UINT_PTR Eip, Pc; } CONTEXT;
typedef struct { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS,
	*LPEXCEPTION_POINTERS;
typedef struct { LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
	BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision,
	lfQuality, lfPitchAndFamily; char lfFaceName[32]; } LOGFONT;
typedef struct { DWORD lStructSize; HWND hwndOwner; HDC hDC; void *lpLogFont; INT iPointSize;
	DWORD Flags, rgbColors; LPARAM lCustData; void *lpfnHook; LPCSTR lpTemplateName;
	HINSTANCE hInstance; LPSTR lpszStyle; WORD nFontType, ___MISSING_ALIGNMENT__;
	INT nSizeMin, nSizeMax; } CHOOSEFONT;
typedef struct { DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; DWORD Flags;
	LPSTR lpstrFindWhat, lpstrReplaceWith; WORD wFindWhatLen, wReplaceWithLen;
	LPARAM lCustData; void *lpfnHook; LPCSTR lpTemplateName; } FINDREPLACE, *LPFINDREPLACE;
typedef unsigned short *LPWSTR;

#define LOWORD(l) ((WORD)(((UINT_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((UINT_PTR)(l)) >> 16) & 0xffff))
#define ZeroMemory(p,n) memset((p),0,(n))

#endif
