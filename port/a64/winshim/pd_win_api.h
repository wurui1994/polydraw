/* port/a64/winshim/pd_win_api.h —— 那 78 个 win32 函数的原型。
 *
 * 实现在 `port/a64/pd_win_a64.c`。分三类：
 *   * **真的**：计时（QueryPerformanceCounter -> clock_gettime）、Sleep、
 *     GetLocalTime/GetSystemTime、GetModuleFileName、ini 那四个（落到文件）；
 *   * **空壳回成功**：窗口、菜单、光标、字体、对话框 —— 出图那条路不需要它们；
 *   * **空壳回失败**：MIDI、管道、进程。
 */
#ifndef PD_WIN_API_H
#define PD_WIN_API_H

/* 计时 */
BOOL QueryPerformanceCounter (LARGE_INTEGER *);
BOOL QueryPerformanceFrequency (LARGE_INTEGER *);
void Sleep (DWORD);
void GetLocalTime (SYSTEMTIME *);
void GetSystemTime (SYSTEMTIME *);
BOOL GetVersionEx (OSVERSIONINFO *);
DWORD GetModuleFileName (HMODULE, LPSTR, DWORD);
void ExitProcess (UINT);
HANDLE GetCurrentThread (void);
void MessageBeep (UINT);
int MessageBox (HWND, LPCSTR, LPCSTR, UINT);

/* ini */
UINT GetPrivateProfileInt (LPCSTR, LPCSTR, INT, LPCSTR);
DWORD GetPrivateProfileString (LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
BOOL WritePrivateProfileString (LPCSTR, LPCSTR, LPCSTR, LPCSTR);

/* 窗口与消息（出图用不着，空壳） */
HWND CreateWindow (LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
HWND CreateWindowEx (DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
BOOL DestroyWindow (HWND);
BOOL ShowWindow (HWND, int);
BOOL UpdateWindow (HWND);
BOOL MoveWindow (HWND, int, int, int, int, BOOL);
BOOL GetWindowRect (HWND, RECT *);
BOOL GetClientRect (HWND, RECT *);
int GetWindowText (HWND, LPSTR, int);
BOOL SetWindowText (HWND, LPCSTR);
LONG_PTR SetWindowLong (HWND, int, LONG_PTR);
HWND SetFocus (HWND);
BOOL ClientToScreen (HWND, POINT *);
BOOL GetCursorPos (POINT *);
BOOL PeekMessage (MSG *, HWND, UINT, UINT, UINT);
BOOL TranslateMessage (const MSG *);
LRESULT DispatchMessage (const MSG *);
LRESULT SendMessage (HWND, UINT, WPARAM, LPARAM);
void PostQuitMessage (int);
LRESULT DefWindowProc (HWND, UINT, WPARAM, LPARAM);
WORD RegisterClass (const WNDCLASS *);
UINT RegisterWindowMessage (LPCSTR);
HCURSOR LoadCursor (HINSTANCE, LPCSTR);
HICON LoadIcon (HINSTANCE, LPCSTR);
BOOL InvalidateRect (HWND, const RECT *, BOOL);

/* 菜单、光标、字体、GDI（空壳） */
HMENU LoadMenuIndirect (const void *);
BOOL SetMenu (HWND, HMENU);
BOOL CheckMenuItem (HMENU, UINT, UINT);
BOOL CreateCaret (HWND, HBITMAP, int, int);
BOOL DestroyCaret (void);
BOOL ShowCaret (HWND);
BOOL SetCaretPos (int, int);
HFONT CreateFont (int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR);
HBRUSH GetStockObject (int);
BOOL DeleteObject (HANDLE);
DWORD SetBkColor (HDC, DWORD);
DWORD SetTextColor (HDC, DWORD);
HDC GetDC (HWND);
int ReleaseDC (HWND, HDC);
BOOL ChooseFont (void *);
BOOL FindText (void *);
BOOL GetOpenFileName (OPENFILENAME *);
BOOL GetSaveFileName (OPENFILENAME *);

/* 线程与同步（空壳；出图是单线程的） */
HANDLE CreateEvent (void *, BOOL, BOOL, LPCSTR);
BOOL SetEvent (HANDLE);
BOOL CloseHandle (HANDLE);
UINT_PTR _beginthreadex (void *, unsigned, unsigned (*)(void *), void *, unsigned, unsigned *);
HANDLE CreateNamedPipe (LPCSTR, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, void *);
BOOL CreateProcess (LPCSTR, LPSTR, void *, void *, BOOL, DWORD, LPVOID, LPCSTR, void *, void *);
BOOL WriteFile (HANDLE, LPCVOID, DWORD, LPDWORD, void *);
int MultiByteToWideChar (UINT, DWORD, LPCSTR, int, void *, int);

/* MIDI（空壳回失败） */
UINT midiOutOpen (HMIDIOUT *, UINT, DWORD, DWORD, DWORD);
UINT midiOutClose (HMIDIOUT);
UINT midiOutShortMsg (HMIDIOUT, DWORD);

/* GL 上下文：wgl* 那四个由 port/a64/pd_gl_cgl.c 用 CGL 实现 */
HGLRC wglCreateContext (HDC);
BOOL wglDeleteContext (HGLRC);
BOOL wglMakeCurrent (HDC, HGLRC);
void *wglGetProcAddress (LPCSTR);
int ChoosePixelFormat (HDC, const PIXELFORMATDESCRIPTOR *);
BOOL SetPixelFormat (HDC, int, const PIXELFORMATDESCRIPTOR *);
BOOL SwapBuffers (HDC);

/* 第二批（探出来的） */
LRESULT CallWindowProc (void *, HWND, UINT, WPARAM, LPARAM);
BOOL ConnectNamedPipe (HANDLE, void *);
BOOL DisconnectNamedPipe (HANDLE);
BOOL FlushFileBuffers (HANDLE);
EXCEPTION_POINTERS *GetExceptionInformation (void);
BOOL HideCaret (HWND);
BOOL IsDialogMessage (HWND, MSG *);
BOOL IsWindow (HWND);
HWND ReplaceText (FINDREPLACE *);
BOOL ResetEvent (HANDLE);
BOOL SystemParametersInfo (UINT, UINT, void *, UINT);
DWORD WaitForSingleObject (HANDLE, DWORD);
INT_PTR _spawnlp (int, const char *, const char *, ...);

#endif
