
//--------------------------------------------------------------------------------------------------

static HANDLE /*gmainthread,*/ gthand, ghevent[3];
static double (__cdecl *gevalfunc)(void) = 0;
static int gevalfuncleng = 0;
static int showtimeout = 0;
static unsigned int __stdcall watchthread (void *_)
{
	while (1)
	{
		WaitForSingleObject(ghevent[0],INFINITE);

			//if script takes too long, temporarily apply self-modifying code to force it to finish much faster
		if (WaitForSingleObject(ghevent[1],popts.timeout) == WAIT_TIMEOUT)
		{
			showtimeout = 1;
			kasm87jumpback(gevalfunc,0);
			WaitForSingleObject(ghevent[1],INFINITE);
			kasm87jumpback(gevalfunc,1);
		}

		SetEvent(ghevent[2]);
	}
}

HMENU gmenu = 0;
static short *menustart (short *sptr) { *sptr++ = 0; *sptr++ = 0; return(sptr); } //MENUITEMTEMPLATEHEADER
static short *menuadd (short *sptr, char *st, int flags, int id)
{
	*sptr++ = flags; //MENUITEMTEMPLATE
	if (!(flags&MF_POPUP)) *sptr++ = id;
	sptr += MultiByteToWideChar(CP_ACP,0,st,-1,(LPWSTR)sptr,strlen(st)+1);
	return(sptr);
}
//--------------------------------------------------------------------------------------------------

	//Hacks to make text editor nicer :)
static LRESULT (CALLBACK *ohWndEdit)(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK nhWndEdit (HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static int isoverwrite = 0, malcaret = 0;
	int i, j, k;

	switch (msg)
	{
		case WM_KEYUP: updateshifts(lParam,0);
			i = ((lParam>>16)&127)+((lParam>>17)&128);
			if (dkeystatus[i] != 0.0) dkeystatus[i] = 0.0;
			break;
		case WM_KEYDOWN: updateshifts(lParam,1);
			i = ((lParam>>16)&127)+((lParam>>17)&128);
			if ((i == 1) && (gmehax)) PostQuitMessage(0);
			if (dkeystatus[i] == 0.0) dkeystatus[i] = 1.0;
			if ((wParam&255) == VK_F1) { helpabout(); return(0); }
			if (popts.fullscreen) return(0);
			if ((wParam&255) == VK_F3) { findnext((shkeystatus&0x30000)==0); return(0); }
			if (((wParam&255) == VK_INSERT) && (!shkeystatus))
			{
				isoverwrite = !isoverwrite;
				if (malcaret) { HideCaret(hWndEdit); DestroyCaret(); } else malcaret = 1;

					//Caret size is total hack guess!
				j = ((labs(popts.fontheight)*20)>>4);
				i = labs(popts.fontwidth); if (!i) i = ((j*9)>>4);
				CreateCaret(hWndEdit,0,isoverwrite*i,j);
				ShowCaret(hWndEdit);
			}
			if (((wParam&255) == 0xbb) && (shkeystatus&0xc0000)) //Ctrl+'='
			{
				int i0, i1;
				SendMessage(hWndEdit,EM_GETSEL,(unsigned)&i0,(unsigned)&i1);
				if (i0 < i1)
				{
					GetWindowText(hWndEdit,ttext,textsiz);
					if (eval_highlight(&ttext[i0],i1-i0)) MessageBeep(64); else MessageBeep(16);
				}
			}
			break;
		case WM_SYSCHAR:
			if ((wParam&255) == VK_RETURN)
			{
				popts.fullscreen = !popts.fullscreen;
				CheckMenuItem(gmenu,MENU_FULLSCREEN,popts.fullscreen*MF_CHECKED);
				resetwindows(SW_NORMAL);
				return(0);
			}
			break;
		case WM_CHAR:
			if ((wParam&255) == 10) { dorecompile = 3; return(0); } //Ctrl+Enter
			if (shkeystatus&0x3c0000)
			{
				if ((wParam&255) == 0x0c) { LoadFile(ghwnd); return(0); } //Ctrl+L
				if ((wParam&255) == 0x13) { if (gsavfilnam[0]) Save(gsavfilnam); else SaveFile(ghwnd); return(0); } //Ctrl+S
				if ((wParam&255) == 0x06) { findreplace(hWndEdit,0); shkeystatus = 0; return(0); } //Ctrl+F
				if ((wParam&255) == 0x12) { findreplace(hWndEdit,1); shkeystatus = 0; return(0); } //Ctrl+R
			}

			if (popts.fullscreen) return(0);

			if ((wParam&255) == 9) //Tab/Shift+Tab
			{
				int i0, i1, l, l0, l1;
				SendMessage(hWndEdit,EM_GETSEL,(unsigned)&i0,(unsigned)&i1);
				if (i0 >= i1) //Tab with no highlight..
				{//
				    if (!(shkeystatus&0x30000)) //Tab..
                    {
                        SendMessage(hWndEdit,EM_SETSEL,i0,i0);
                        SendMessage(hWndEdit,EM_REPLACESEL,1,(LPARAM)"   ");
                    }
                    else //Shift+Tab..
                    {
                        char buf[2048]; *(long *)&buf[0] = 2048;
                        l = SendMessage(hWndEdit,EM_LINEFROMCHAR,i0,0);
                        k = SendMessage(hWndEdit,EM_GETLINE,l,(LPARAM)buf);
                        j = SendMessage(hWndEdit,EM_LINEINDEX,l,0);

                        for(i=i0-j-1; i>=0 && i < k && i0-j-i <= 3; i--)
                            if (buf[i] != ' ') break;

                        SendMessage(hWndEdit,EM_SETSEL,i+j+1,i0);
                        SendMessage(hWndEdit,EM_REPLACESEL,1,(LPARAM)"");
                    }
				}
				else //Tab/Shift+Tab with highlight..
				{
					l0 = SendMessage(hWndEdit,EM_LINEFROMCHAR,i0,0);
					l1 = SendMessage(hWndEdit,EM_LINEFROMCHAR,i1-1,0);
					for(l=l0;l<=l1;l++)
					{
						j = SendMessage(hWndEdit,EM_LINEINDEX,l,0);
						if (!(shkeystatus&0x30000)) //Tab..
						{
							i1 += 3; if (j < i0) i0 += 3;
							SendMessage(hWndEdit,EM_SETSEL,j,j);
							SendMessage(hWndEdit,EM_REPLACESEL,1,(LPARAM)"   ");
						}
						else //Shift+Tab..
						{
							char buf[max(4,3)]; *(long *)&buf[0] = 3;
							k = SendMessage(hWndEdit,EM_GETLINE,l,(LPARAM)buf);
							for(i=0;i<k;i++) if (buf[i] != ' ') break;
							i1 -= i; if (j < i0) i0 -= i;
							SendMessage(hWndEdit,EM_SETSEL,j,j+i);
							SendMessage(hWndEdit,EM_REPLACESEL,1,(LPARAM)"");
						}
					}
					SendMessage(hWndEdit,EM_SETSEL,i0,i1);
				}
				return(0);
			}
			if ((isoverwrite) && ((wParam&255) >= 32)) //See: http://www.jeffluther.net/unify/Tech-Newsletter/pdf/1999/0499-6.pdf
			{
				SendMessage(hWndEdit,EM_GETSEL,(unsigned)&i,0); //i = caret index
				j = SendMessage(hWndEdit,EM_LINEINDEX,-1,0);    //j = index to home of caret's line
				k = SendMessage(hWndEdit,EM_LINELENGTH,j,0);    //j+k = index to end of caret's line
				if (i < j+k) SendMessage(hWndEdit,EM_SETSEL,i,i+1);
			}
			break;

		case WM_PAINT: updatelines(0); break;

	 //case WM_SETFOCUS: break; //Edit control has gained the input focus
		case WM_KILLFOCUS: //Edit control has lost the input focus
			if (malcaret) { malcaret = 0; HideCaret(hWndEdit); DestroyCaret(); isoverwrite = 0; }
			break;
		default:
			if ((gfind_msg) && (msg == gfind_msg)) return(findreplace_process((LPFINDREPLACE)lParam)); //Needed for FindText/ReplaceText
	}
	return(CallWindowProc(ohWndEdit,hWnd,msg,wParam,lParam));
}

static LRESULT (CALLBACK *ohWndCons)(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK nhWndCons (HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	int i;
	switch (msg)
	{
		case WM_KEYUP: updateshifts(lParam,0);
			i = ((lParam>>16)&127)+((lParam>>17)&128);
			if (dkeystatus[i] != 0.0) dkeystatus[i] = 0.0;
			break;
		case WM_KEYDOWN: updateshifts(lParam,1);
			i = ((lParam>>16)&127)+((lParam>>17)&128);
			if ((i == 1) && (gmehax)) PostQuitMessage(0);
			if (dkeystatus[i] == 0.0) dkeystatus[i] = 1.0;
			if ((wParam&255) == VK_F1) { helpabout(); return(0); }
			if ((wParam&255) == VK_F3) { findnext((shkeystatus&0x30000)==0); return(0); }
			break;
		case WM_SYSCHAR:
			if ((wParam&255) == VK_RETURN)
			{
				popts.fullscreen = !popts.fullscreen;
				CheckMenuItem(gmenu,MENU_FULLSCREEN,popts.fullscreen*MF_CHECKED);
				resetwindows(SW_NORMAL);
				return(0);
			}
			break;
		case WM_CHAR:
			if ((wParam&255) == 10) { dorecompile = 3; return(0); } //Ctrl+Enter
			if ((wParam&255) == 0x0c) { LoadFile(ghwnd); return(0); } //Ctrl+L
			if ((wParam&255) == 0x13) { if (gsavfilnam[0]) Save(gsavfilnam); else SaveFile(ghwnd); return(0); } //Ctrl+S
			if ((wParam&255) == 0x06) { findreplace(hWndEdit,0); shkeystatus = 0; return(0); } //Ctrl+F
			if ((wParam&255) == 0x12) { findreplace(hWndEdit,1); shkeystatus = 0; return(0); } //Ctrl+R
			break;
	}
	return(CallWindowProc(ohWndCons,hWnd,msg,wParam,lParam));
}

static LRESULT (CALLBACK *ohWndLine)(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK nhWndLine (HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	POINT p0;
	int i;
	switch (msg)
	{
		case WM_SETFOCUS: //clicking line number results in focus changing to edit window
			SendMessage(hWnd,EM_GETSEL,(WPARAM)&i,0);
			i = SendMessage(hWnd,EM_LINEFROMCHAR,i,0) + SendMessage(hWndEdit,EM_GETFIRSTVISIBLELINE,0,0);

				//Must send button down here for highlight to catch correct line
			GetCursorPos(&p0); SendMessage(hWndEdit,WM_LBUTTONDOWN,MK_LBUTTON,(LPARAM)&p0);
			SetFocus(hWndEdit);

				//Place cursor at respective line of hWndEdit
			i = SendMessage(hWndEdit,EM_LINEINDEX,i,0);
			SendMessage(hWndEdit,EM_SETSEL,i,i);

			return(0);
	}
	return(CallWindowProc(ohWndLine,hWnd,msg,wParam,lParam));
}

static void resetwindows (int cmdshow)
{
	static int ooglxres = 0, ooglyres = 0;
	RECT r;
	int i, guiflags, x0[4], y0[4], x1[4], y1[4], linenumwid;


	i = ((labs(popts.fontheight)*20)>>4);
	linenumwid = labs(popts.fontwidth); if (!linenumwid) linenumwid = ((i*9)>>4);
	linenumwid *= 4;

	if (!ghwnd)
	{
		RECT rw;
		int x, y;

		SystemParametersInfo(SPI_GETWORKAREA,0,&rw,0);
		x = ((rw.right -rw.left-xres)>>1) + rw.left;
		y = ((rw.bottom-rw.top -yres)>>1) + rw.top;
		ghwnd = CreateWindow("PolyDraw",prognam,WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SIZEBOX,x,y,xres,yres,0,0,ghinst,0); //|WS_VISIBLE|WS_POPUPWINDOW|WS_CAPTION
		ShowWindow(ghwnd,cmdshow);
	}

	guiflags = WS_VISIBLE |WS_CHILD|WS_VSCROLL; //|WS_HSCROLL|WS_CAPTION|WS_SIZEBOX|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU;
	guiflags |= ES_MULTILINE|ES_WANTRETURN|ES_AUTOHSCROLL|ES_AUTOVSCROLL;

	if (!popts.fullscreen)
	{
		oglxres = ((xres>>1)&~3);
		oglyres = ((oglxres*3)>>2);
		x1[0] = oglxres;      y1[0] =      oglyres;
		x1[1] = oglxres;      y1[1] = yres-oglyres;
		x1[2] = linenumwid             ; y1[2] = yres;
		x1[3] = xres-oglxres-linenumwid; y1[3] = yres;
		if (!(popts.rendcorn&1)) { x0[0] = 0; x0[1] =       0; x0[2] = oglxres; x0[3] = oglxres+linenumwid; } else { x0[0] = xres-oglxres; x0[1] = xres-oglxres; x0[2] = 0; x0[3] = linenumwid; }
		if (!(popts.rendcorn&2)) { y0[0] = 0; y0[1] = oglyres; y0[2] =       0; y0[3] =                  0; } else { y0[0] = yres-oglyres; y0[1] =            0; y0[2] = 0; y0[3] =          0; }
	}
	else
	{
		oglxres = xres;
		oglyres = yres;
		x0[0] =       0; y0[0] =       0; x1[0] = oglxres; y1[0] = oglyres;
		x0[1] =       0; y0[1] = oglyres; x1[1] =       0; y1[1] =       0;
		x0[2] = oglxres; y0[2] =       0; x1[2] =       0; y1[2] =       0;
		x0[3] = oglxres; y0[3] =       0; x1[3] =       0; y1[3] =       0;
	}

	if (hWndDraw) MoveWindow(hWndDraw,x0[0],y0[0],x1[0],y1[0],1); else hWndDraw = CreateWindowEx(               0,"PolyDraw","Render" ,WS_VISIBLE|WS_CHILD             ,x0[0],y0[0],x1[0],y1[0],ghwnd,(HMENU)100,ghinst,0);
	if (hWndCons) MoveWindow(hWndCons,x0[1],y0[1],x1[1],y1[1],1); else hWndCons = CreateWindowEx(WS_EX_CLIENTEDGE,"edit"    ,"Console",guiflags|ES_READONLY|WS_HSCROLL ,x0[1],y0[1],x1[1],y1[1],ghwnd,(HMENU)101,ghinst,0);
	if (hWndLine) MoveWindow(hWndLine,x0[2],y0[2],x1[2],y1[2],1); else hWndLine = CreateWindowEx(WS_EX_WINDOWEDGE,"edit"    ,"Lines"  ,(guiflags|ES_READONLY|ES_RIGHT)&~WS_VSCROLL,x0[2],y0[2],x1[2],y1[2],ghwnd,(HMENU)101,ghinst,0);
	if (hWndEdit) MoveWindow(hWndEdit,x0[3],y0[3],x1[3],y1[3],1); else
	{
		hWndEdit = CreateWindowEx(WS_EX_WINDOWEDGE,"edit"    ,"Script" ,guiflags|ES_NOHIDESEL                        ,x0[3],y0[3],x1[3],y1[3],ghwnd,(HMENU)102,ghinst,0);

		SendMessage(hWndEdit,EM_LIMITTEXT,textsiz-1,0);

			//See subclassing controls here: http://msdn.microsoft.com/en-us/library/bb773183.aspx
			//NOTE:replace these with SetWindowLongPtr if porting to 64-bit windows!

		ohWndCons = (void *)SetWindowLong(hWndCons,GWL_WNDPROC,(long)/*(LONG_PTR)*/nhWndCons);
		ohWndEdit = (void *)SetWindowLong(hWndEdit,GWL_WNDPROC,(long)/*(LONG_PTR)*/nhWndEdit);
		ohWndLine = (void *)SetWindowLong(hWndLine,GWL_WNDPROC,(long)/*(LONG_PTR)*/nhWndLine);

	}
	SendMessage(hWndLine,EM_GETRECT,0,(LPARAM)&r); r.left = -1000; r.right = linenumwid-4; //increase virtual size of line window
	SendMessage(hWndLine,EM_SETRECTNP,0,(LPARAM)&r);

	if ((ooglxres != oglxres) || (ooglyres != oglyres))
	{
		//dorecompile = 1;
		//QueryPerformanceCounter((LARGE_INTEGER *)&qtim0); dnumframes = 0.0; //WinXP/balls.pss needs this!
	}
}

static LRESULT CALLBACK WndProc (HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	POINT p0, p1;
	int i;
	switch (msg)
	{
		case WM_DESTROY: PostQuitMessage(0); break;
		case WM_LBUTTONDOWN: if (fmod(dbstatus,2.0) <  1) dbstatus += 1;
			  p0.x = p0.y = 0; ClientToScreen(hWndDraw,&p0); GetCursorPos(&p1);
			  if (((unsigned)(p1.x-p0.x) < (unsigned)oglxres) && ((unsigned)(p1.y-p0.y) < (unsigned)oglyres)) SetFocus(ghwnd);
			  break;
		case WM_LBUTTONUP:   if (fmod(dbstatus,2.0) >= 1) dbstatus -= 1; break;
		case WM_RBUTTONDOWN: if (fmod(dbstatus,4.0) <  2) dbstatus += 2; break;
		case WM_RBUTTONUP:   if (fmod(dbstatus,4.0) >= 2) dbstatus -= 2; break;
		case WM_MBUTTONDOWN: if (fmod(dbstatus,8.0) <  4) dbstatus += 4; break;
		case WM_MBUTTONUP:   if (fmod(dbstatus,8.0) >= 4) dbstatus -= 4; break;
		case WM_KEYUP: updateshifts(lParam,0);
			i = ((lParam>>16)&127)+((lParam>>17)&128);
			if (dkeystatus[i] != 0.0) dkeystatus[i] = 0.0;
			break;
		case WM_KEYDOWN: updateshifts(lParam,1);
			i = ((lParam>>16)&127)+((lParam>>17)&128);
			if ((i == 1) && (gmehax)) PostQuitMessage(0);
			if (dkeystatus[i] == 0.0) dkeystatus[i] = 1.0;
			if ((wParam&255) == VK_F1) { helpabout(); return(0); }
			if ((wParam&255) == VK_F3) { findnext((shkeystatus&0x30000)==0); return(0); }
			break;
		case WM_SYSCHAR:
			if ((wParam&255) == VK_RETURN)
			{
				popts.fullscreen = !popts.fullscreen;
				CheckMenuItem(gmenu,MENU_FULLSCREEN,popts.fullscreen*MF_CHECKED);
				resetwindows(SW_NORMAL);
				return(0);
			}
			break;
		case WM_CHAR:
			if ((wParam&255) == 10) { dorecompile = 3; return(0); } //Ctrl+Enter
			if ((wParam&255) == 0x0c) { LoadFile(ghwnd); return(0); } //Ctrl+L
			if ((wParam&255) == 0x13) { if (gsavfilnam[0]) Save(gsavfilnam); else SaveFile(ghwnd); return(0); } //Ctrl+S
			if ((wParam&255) == 0x06) { findreplace(hWndEdit,0); shkeystatus = 0; return(0); } //Ctrl+F
			if ((wParam&255) == 0x12) { findreplace(hWndEdit,1); shkeystatus = 0; return(0); } //Ctrl+R
			break;
		case WM_SIZE:
			if (hWnd != ghwnd) break;
			if ((wParam == SIZE_MAXHIDE) || (wParam == SIZE_MINIMIZED)) { ActiveApp = 0; break; }
			ActiveApp = 1;
			xres = LOWORD(lParam);
			yres = HIWORD(lParam);
			if ((oxres != xres) || (oyres != yres)) { oxres = xres; oyres = yres; resetwindows(SW_NORMAL); }
			break;
		case WM_ACTIVATEAPP: ActiveApp = (BOOL)wParam; shkeystatus = 0; break;
#if 0
		case WM_CTLCOLOREDIT:
			SetTextColor(wParam,0xc0c0c0);
			SetBkColor(wParam,0x404040);
			return(GetStockObject(DKGRAY_BRUSH));
		case WM_CTLCOLORSTATIC:
			SetTextColor(wParam,0xc0c0c0);
			SetBkColor(wParam,0x404040);
			return(GetStockObject(DKGRAY_BRUSH));
#endif
		case WM_CLOSE: if (!passasksave()) { return(0); } break;
		case WM_COMMAND:
			switch(LOWORD(wParam)) //process menu
			{
				case MENU_FILENEW+0: case MENU_FILENEW+1: case MENU_FILENEW+2: case MENU_FILENEW+3:
					NewFile(LOWORD(wParam)-MENU_FILENEW); break;
				case MENU_FILEOPEN:    LoadFile(hWnd); break;
				case MENU_FILESAVE:    if (gsavfilnam[0]) { Save(gsavfilnam); break; } //no break intentional
				case MENU_FILESAVEAS:  SaveFile(hWnd); break;
				case MENU_FILEEXIT:    if (passasksave()) { PostQuitMessage(0); } break;
				case MENU_EDITFIND:    findreplace(hWndEdit,0); break;
				case MENU_EDITFINDNEXT: findnext(1); break;
				case MENU_EDITFINDPREV: findnext(0); break;
				case MENU_EDITREPLACE: findreplace(hWndEdit,1); break;
				case MENU_COMPCONTENT: popts.compctrlent ^= 1; if (!popts.compctrlent) dorecompile = 3; CheckMenuItem(gmenu,MENU_COMPCONTENT,popts.compctrlent*MF_CHECKED); break;
				case MENU_EVALHIGHLIGHT:
					{
						int i0, i1;
						SendMessage(hWndEdit,EM_GETSEL,(unsigned)&i0,(unsigned)&i1);
						if (i0 < i1)
						{
							GetWindowText(hWndEdit,ttext,textsiz);
							if (eval_highlight(&ttext[i0],i1-i0)) MessageBeep(64); else MessageBeep(16);
						}
					}
					break;
				case MENU_RENDPLC+0: case MENU_RENDPLC+1: case MENU_RENDPLC+2: case MENU_RENDPLC+3:
					popts.rendcorn = LOWORD(wParam)-MENU_RENDPLC; popts.fullscreen = 0;
					for(i=0;i<4;i++) CheckMenuItem(gmenu,MENU_RENDPLC+i,(LOWORD(wParam)==MENU_RENDPLC+i)*MF_CHECKED);
					CheckMenuItem(gmenu,MENU_FULLSCREEN,popts.fullscreen*MF_CHECKED);
					resetwindows(SW_NORMAL);
					break;
				case MENU_FULLSCREEN:
					popts.fullscreen = !popts.fullscreen;
					CheckMenuItem(gmenu,MENU_FULLSCREEN,popts.fullscreen*MF_CHECKED);
					resetwindows(SW_NORMAL);
					break;
				case MENU_CLEARBUFFER:
					popts.clearbuffer = !popts.clearbuffer;
					CheckMenuItem(gmenu,MENU_CLEARBUFFER,popts.clearbuffer*MF_CHECKED);
					resetwindows(SW_NORMAL);
					break;
				case MENU_FONT:
					{
					CHOOSEFONT cf;
					static LOGFONT lf;

					memset(&cf,0,sizeof(cf));
					cf.lStructSize = sizeof(cf);
					cf.hwndOwner = hWnd;
					cf.lpLogFont = &lf;
					lf.lfHeight = popts.fontheight;
					lf.lfWidth = popts.fontwidth;
					strcpy(lf.lfFaceName,popts.fontname);
					cf.Flags = CF_SCREENFONTS|CF_FIXEDPITCHONLY|CF_INITTOLOGFONTSTRUCT|CF_NOSTYLESEL;
					if (ChooseFont(&cf))
						if (lf.lfFaceName[0])
						{
							if (hfont) DeleteObject(hfont);

							popts.fontheight = lf.lfHeight;
							popts.fontwidth = lf.lfWidth;
							strcpy(popts.fontname,lf.lfFaceName);

							popts.sepchar = '-'; //Many XP fonts do not have solid hyphen char :/
							//     if (!stricmp(popts.fontname,"Consolas"      )) popts.sepchar = 6; //also 151
							//else if (!stricmp(popts.fontname,"Courier"       )) popts.sepchar = 6;
							//else if (!stricmp(popts.fontname,"Courier New"   )) popts.sepchar = 151; //also 6
							//else if (!stricmp(popts.fontname,"Fixedsys"      )) popts.sepchar = 6;
							//else if (!stricmp(popts.fontname,"Lucida Console")) popts.sepchar = 6; //also 151
							//else if (!stricmp(popts.fontname,"Terminal"      )) popts.sepchar = 196;
							//else                                                popts.sepchar = '-';

							hfont = CreateFont(popts.fontheight,popts.fontwidth,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,popts.fontname);

							SendMessage(hWndCons,WM_SETFONT,(WPARAM)hfont,0); ShowWindow(hWndCons,SW_HIDE); UpdateWindow(hWndCons); ShowWindow(hWndCons,SW_SHOWNORMAL);
							SendMessage(hWndEdit,WM_SETFONT,(WPARAM)hfont,0); ShowWindow(hWndEdit,SW_HIDE); UpdateWindow(hWndEdit); ShowWindow(hWndEdit,SW_SHOWNORMAL);
							SendMessage(hWndLine,WM_SETFONT,(WPARAM)hfont,0); ShowWindow(hWndLine,SW_HIDE); UpdateWindow(hWndLine); ShowWindow(hWndLine,SW_SHOWNORMAL);
							resetwindows(SW_NORMAL);
							updatelines(1);
						}
					}
					break;
				case MENU_HELPTEXT:
					{
					int i, j;
					char tbuf[MAX_PATH];
					sprintf(tbuf,"%spolydraw.txt",gexedironly);
					_spawnlp(_P_NOWAIT,"notepad.exe","notepad.exe",tbuf,0);
					}
					break;
				case MENU_HELPABOUT: helpabout(); break;
			}
			switch (HIWORD(wParam))
			{
				//case EN_CHANGE: //break; //Edit control's contents will change
				//case EN_VSCROLL: updatelines(0); break; //doesn't get triggered by mouse dragging; must use WM_PAINT in nhWndEdit instead
				case EN_UPDATE: //Edit control's contents have changed
					if ((HWND)lParam == hWndEdit) updatelines(1); //text changed in edit window (would be cheaper than calling strcmp :P)
					break;
			}
			break;
	}
	return(DefWindowProc(hWnd,msg,wParam,lParam));
}

static void EnableOpenGL (HWND hWnd, HDC *hDC, HGLRC *hRC)
{
	PIXELFORMATDESCRIPTOR pfd;
	int format;

	*hDC = GetDC(hWnd);

	ZeroMemory(&pfd,sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 24;
	pfd.cDepthBits = 16;
	pfd.iLayerType = PFD_MAIN_PLANE;
	format = ChoosePixelFormat(*hDC,&pfd);
	SetPixelFormat(*hDC,format,&pfd);

	*hRC = wglCreateContext(*hDC);
	wglMakeCurrent(*hDC,*hRC);
}

static void DisableOpenGL (HWND hWnd, HDC hDC, HGLRC hRC)
{
	wglMakeCurrent(0,0);
	wglDeleteContext(hRC);
	ReleaseDC(hWnd,hDC);
}

static int checkext (char *extnam)
{
	const char *st = glGetString(GL_EXTENSIONS);
	st = strstr(st,extnam); if (!st) return(0);
	return(st[strlen(extnam)] <= 32);
}

static int cmdline2arg (char *cmdline, char **argv)
{
	int i, j, k, inquote, argc;

		//Convert Windows command line into ANSI 'C' command line...
	argv[0] = "exe"; argc = 1; j = inquote = 0;
	for(i=0;cmdline[i];i++)
	{
		k = (((cmdline[i] != ' ') && (cmdline[i] != '\t')) || (inquote));
		if (cmdline[i] == '\"') inquote ^= 1;
		if (j < k) { argv[argc++] = &cmdline[i+inquote]; j = inquote+1; continue; }
		if ((j) && (!k))
		{
			if ((j == 2) && (cmdline[i-1] == '\"')) cmdline[i-1] = 0;
			cmdline[i] = 0; j = 0;
		}
	}
	if ((j == 2) && (cmdline[i-1] == '\"')) cmdline[i-1] = 0;
	argv[argc] = 0;
	return(argc);
}

int WINAPI WinMain (HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	WNDCLASS wc;
	MSG msg;
	HDC hDC;
	HGLRC hRC;
	RECT rw;
	__int64 q = 0I64, qlast = 0I64;
	int qnum = 0;
	int i, j, k, z, argc, argfilindex = -1, setsel0 = -1, setsel1 = -1, scrolly = -1;
	char buf[1024], *argv[MAX_PATH>>1], *savfilnam = 0;

	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&osvi);

	SystemParametersInfo(SPI_GETWORKAREA,0,&rw,0);
	xres = (((rw.right -rw.left)*3)>>2);
	yres = (((rw.bottom-rw.top )*3)>>2);
	nCmdShow = SW_MAXIMIZE;

	GetModuleFileName(0,gexefullpath,sizeof(gexefullpath));
	for(i=0,j=-1;gexefullpath[i];i++)
		if ((gexefullpath[i] == '\\') || (gexefullpath[i] == '/')) j = i;
	strcpy(gexedironly,gexefullpath); gexedironly[j+1] = 0;
	sprintf(ginifilnam,"%spolydraw.ini",gexedironly);
	loadini();

	kzaddstack(gexedironly);

	argc = cmdline2arg(lpCmdLine,argv);
	for(i=argc-1;i>0;i--)
	{
		if ((argv[i][0] != '/') && (argv[i][0] != '-')) { argfilindex = i; continue; }
		if (!memicmp(&argv[i][1],"bench",5)) { gbenchn = atol(&argv[i][7]); continue; } //Omni benchmark
		if (!stricmp(&argv[i][1],"qme")) { gmehax = 1; popts.fullscreen = 1; continue; } //for integration with MoonEdit
		if (!memicmp(&argv[i][1],"setsel0",7)) { setsel0 = atol(&argv[i][9]); continue; } //hack for seamless pipe restart
		if (!memicmp(&argv[i][1],"setsel1",7)) { setsel1 = atol(&argv[i][9]); continue; } //hack for seamless pipe restart
		if (!memicmp(&argv[i][1],"scrolly",7)) { scrolly = atol(&argv[i][9]); continue; } //hack for seamless pipe restart
		if (!memicmp(&argv[i][1],"savfil",6)) { savfilnam = &argv[i][8]; continue; } //hack for seamless pipe restart
		if ((argv[i][1] >= '0') && (argv[i][1] <= '9'))
		{
			k = 0; z = 0;
			for(j=1;;j++)
			{
				if ((argv[i][j] >= '0') && (argv[i][j] <= '9')) { k = (k*10+argv[i][j]-'0'); continue; }
				switch (z)
				{
					case 0: xres = k; nCmdShow = SW_NORMAL; break;
					case 1: yres = k; break;
				}
				if (!argv[i][j]) break;
				z++; if (z > 2) break;
				k = 0;
			}
		}
	}

	ghinst = hInstance;

	wc.style = CS_OWNDC;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hInstance;
	wc.hIcon = LoadIcon(0,IDI_APPLICATION);
	wc.hCursor = LoadCursor(0,IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszMenuName = 0;
	wc.lpszClassName = "PolyDraw";
	RegisterClass(&wc);

	if (osvi.dwPlatformId < 2) textsiz = 32768;/*<NT*/ else textsiz = 65536;
	text  = malloc(textsiz); if (! text) { MessageBox(ghwnd,"malloc failed",prognam,MB_OK); return(1); }
	otext = malloc(textsiz); if (!otext) { MessageBox(ghwnd,"malloc failed",prognam,MB_OK); return(1); }
	ttext = malloc(textsiz); if (!ttext) { MessageBox(ghwnd,"malloc failed",prognam,MB_OK); return(1); }
	line  = malloc(textsiz); if (! line) { MessageBox(ghwnd,"malloc failed",prognam,MB_OK); return(1); }
	badlinebits = malloc((textsiz+7)>>3); if (!badlinebits) { MessageBox(ghwnd,"malloc failed",prognam,MB_OK); return(1); }
	memset(badlinebits,0,(textsiz+7)>>3);

	resetwindows(nCmdShow);

	hfont = CreateFont(popts.fontheight,popts.fontwidth,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,popts.fontname);
	SendMessage(hWndCons,WM_SETFONT,(WPARAM)hfont,0);
	SendMessage(hWndEdit,WM_SETFONT,(WPARAM)hfont,0);
	SendMessage(hWndLine,WM_SETFONT,(WPARAM)hfont,0);

		//Use MF_POPUP for top entries
		//Use MF_END for last (top or pulldown) entry
		//MF_GRAYED|MF_DISABLED|4=right_justify|MF_CHECKED|MF_MENUBARBREAK|MF_MENUBREAK|MF_OWNERDRAW
	{
	short sbuf[4096], *sptr;
	sptr = menustart(sbuf);
	sptr = menuadd(sptr,"&File"      ,MF_POPUP        ,0);
	sptr = menuadd(sptr,   "&New"    ,MF_POPUP        ,0);
	sptr = menuadd(sptr,      "Blank"     ,0          ,MENU_FILENEW+0);
	sptr = menuadd(sptr,      "GLSL (minimal)",0      ,MENU_FILENEW+1);
	sptr = menuadd(sptr,      "GLSL"      ,0          ,MENU_FILENEW+2);
	sptr = menuadd(sptr,      "ARB ASM"   ,MF_END     ,MENU_FILENEW+3);
	sptr = menuadd(sptr,   "&Open\tCtrl+L",0          ,MENU_FILEOPEN);
	sptr = menuadd(sptr,   "&Save\tCtrl+S",0          ,MENU_FILESAVE);
	sptr = menuadd(sptr,   "Save &As"     ,0          ,MENU_FILESAVEAS);
	sptr = menuadd(sptr,   ""        ,MF_SEPARATOR    ,0);
	sptr = menuadd(sptr,   "E&xit\tAlt+F4" ,MF_END    ,MENU_FILEEXIT);
	sptr = menuadd(sptr,"&Edit"      ,MF_POPUP        ,0);
	sptr = menuadd(sptr,   "&Find...\tCtrl+F"   ,0    ,MENU_EDITFIND);
	sptr = menuadd(sptr,   "Find &Next\tF3"      ,0   ,MENU_EDITFINDNEXT);
	sptr = menuadd(sptr,   "Find &Previous\tShift+F3",0,MENU_EDITFINDPREV);
	sptr = menuadd(sptr,   "&Replace...\tCtrl+R",MF_END,MENU_EDITREPLACE);
	sptr = menuadd(sptr,"&Options"   ,MF_POPUP        ,0);
	sptr = menuadd(sptr,   "Compile on Ctrl+Enter",popts.compctrlent*MF_CHECKED,MENU_COMPCONTENT);
	sptr = menuadd(sptr,   "Evaluate highlighted text\tCtrl+'='",0,MENU_EVALHIGHLIGHT);
	sptr = menuadd(sptr,   "Select Render corner",MF_POPUP,0);
	sptr = menuadd(sptr,      "Top Left"    ,(popts.rendcorn==0)*MF_CHECKED       ,MENU_RENDPLC+0);
	sptr = menuadd(sptr,      "Top Right"   ,(popts.rendcorn==1)*MF_CHECKED       ,MENU_RENDPLC+1);
	sptr = menuadd(sptr,      "Bottom Left" ,(popts.rendcorn==2)*MF_CHECKED       ,MENU_RENDPLC+2);
	sptr = menuadd(sptr,      "Bottom Right",(popts.rendcorn==3)*MF_CHECKED|MF_END,MENU_RENDPLC+3);
	sptr = menuadd(sptr,   "Fullscreen Render\tAlt+Enter"    ,(popts.fullscreen!=0)*MF_CHECKED,MENU_FULLSCREEN);
	sptr = menuadd(sptr,   "Clear screen every frame"              ,(popts.clearbuffer!=0)*MF_CHECKED,MENU_CLEARBUFFER);
	sptr = menuadd(sptr,   "Select &Font.."   ,MF_END    ,MENU_FONT);
	sptr = menuadd(sptr,"&Help"               ,MF_POPUP|MF_END,0);
	sptr = menuadd(sptr,"   PolyDraw.txt.."   ,0              ,MENU_HELPTEXT);
	sptr = menuadd(sptr,"   &About\tF1"       ,MF_END         ,MENU_HELPABOUT);
	gmenu = LoadMenuIndirect(sbuf);
	SetMenu(ghwnd,gmenu);
	}

	EnableOpenGL(hWndDraw,&hDC,&hRC);

	kputs("GL_VENDOR:   ",0); kputs(glGetString(GL_VENDOR),1);
	kputs("GL_RENDERER: ",0); kputs(glGetString(GL_RENDERER),1);
	kputs("GL_VERSION:  ",0); kputs(glGetString(GL_VERSION),1);
	//if ((!checkext("GL_ARB_vertex_program")) || (!checkext("GL_ARB_fragment_program")))
	//if (checkext("GL_EXT_geometry_shader4")) ... ?
	if (!wglGetProcAddress("glCreateShaderObjectARB"))
	{
		NewFile(3);
	}
	else
	{
		NewFile(2);
		kputs("GLSL_VERSION:",0); kputs(glGetString(0x8B8C /*GL_SHADING_LANGUAGE_VERSION*/),1);

		//glGetIntegerv(0x84e2 /*GL_MAX_TEXTURE_UNITS*/,&i); sprintf(buf,"GL_MAX_TEXTURE_UNITS=%d",i); kputs(buf,1); //4 (obsolete/wrong/never use :P)
		//glGetIntegerv(0x8872 /*GL_MAX_TEXTURE_IMAGE_UNITS*/,&i); sprintf(buf,"GL_MAX_TEXTURE_IMAGE_UNITS=%d",i); kputs(buf,1); //32
		//glGetIntegerv(0x8824 /*GL_MAX_DRAW_BUFFERS*/,&i); sprintf(buf,"GL_MAX_DRAW_BUFFERS=%d",i); kputs(buf,1); //8
		//glGetIntegerv(0x8871 /*GL_MAX_TEXTURE_COORDS*/,&i); sprintf(buf,"GL_MAX_TEXTURE_COORDS=%d",i); kputs(buf,1); //8
		//if (!memicmp(glGetString(GL_VENDOR),"NVIDIA",6))
		//{
		//   glGetIntegerv(0x9048 /*NV tot mem kb*/,&i); sprintf(buf,"NV_TOT_MEM=%dKBy",i); kputs(buf,1); //1048576
		//   glGetIntegerv(0x9049 /*NV cur mem kb*/,&i); sprintf(buf,"NV_CUR_MEM=%dKBy",i); kputs(buf,1); //991504
		//}

		//kputs(glGetString(GL_EXTENSIONS),1); //List too long!
	}
	kputs("----------------------------------------",1);

	//supporttimerquery = (checkext("GL_EXT_timer_query") || checkext("GL_ARB_timer_query"));

	for(i=0;i<NUMGLFUNC;i++)
	{
		if (!useoldglfuncs)
		{
			glfp[i] = (glfp_t)wglGetProcAddress(glnames[i]); if (glfp[i]) continue;
			useoldglfuncs = 1;
		}
		glfp[i] = (glfp_t)wglGetProcAddress(glnames_old[i]); if (glfp[i]) continue;
		sprintf(buf,"%s() / %s() not supported. :/",glnames[i],glnames_old[i]);
		if (i < glCreateShader) { MessageBox(ghwnd,buf,prognam,MB_OK); ExitProcess(0); }
		if (i < glGenQueries) { kputs(buf,1); kputs("NOTE:This machine is limited to ARB ASM :/",1); supporttimerquery = 0; usearbasmonly = 1; break; }
		if (i < glGetQueryObjectui64vEXT) { supporttimerquery = 0; break; }
	}
	if (supporttimerquery) ((PFNGLGENQUERIESPROC)glfp[glGenQueries])(1,queries);
	if (glfp[wglSwapIntervalEXT]) ((PFNWGLSWAPINTERVALEXTPROC)glfp[wglSwapIntervalEXT])(1);

	noiseinit();

	text[0] = 0; otext[0] = 0;

	if (argfilindex >= 0) Load(argv[argfilindex],hWndEdit);

	SetFocus(hWndEdit);
	if ((scrolly >= 0) || (setsel0 >= 0) || (setsel1 >= 0))
	{
		if (scrolly >= 0) SendMessage(hWndEdit,EM_LINESCROLL,0,scrolly);
		if ((setsel0 >= 0) && (setsel1 >= 0)) SendMessage(hWndEdit,EM_SETSEL,setsel0,setsel1);
	}
	if (savfilnam)
	{
		strcpy(gsavfilnam,savfilnam); gsavfilnamptr = 0;
		kputs("File name is: ",0); kputs(gsavfilnam,1);
	}

	QueryPerformanceFrequency((LARGE_INTEGER *)&qper);
	QueryPerformanceCounter((LARGE_INTEGER *)&qtim0);

	printg_init();
	//CreateEmptyTexture(0,32,32,1,KGL_BGRA32); //avoid harmless gl error at glUniform1i(..("tex0")..,0)

	while (1)
	{
		while (PeekMessage(&msg,0,0,0,PM_REMOVE))
		{
			if (msg.message == WM_QUIT) goto quitit;
			if ((gfind_wnd) && (IsWindow(gfind_wnd)) && (IsDialogMessage(gfind_wnd,&msg))) continue; //Needed for FindText/ReplaceText (keyboard shortcuts)
			TranslateMessage(&msg); DispatchMessage(&msg);
		}
		if ((!ActiveApp) && (!gbenchn)) { Sleep(100); continue; } //Omni benchmark: no focus wait

		glClearColor(0.f,0.f,0.f,0.f);
		if(popts.clearbuffer)
			glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);	

		glEnable(GL_DEPTH_TEST);
		glViewport(0,0,oglxres,oglyres);
		glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluPerspective(gfov,(float)oglxres/(float)oglyres,0.1,1000.0);
		glMatrixMode(GL_MODELVIEW); glLoadIdentity();

		GetWindowText(hWndEdit,text,textsiz); tsecn = txt2sec(text,tsec);

		setShaders(ghwnd,hWndEdit);
		if (shadn[2]) Draw(ghwnd,hWndEdit);
		if ((!shadn[2]) || (!gevalfunc))
		{
			if (!gbenchn) Sleep(1); //Omni benchmark: this politeness sleep is 15.6ms with the
			                        //default timer granularity, which pins shader-less scripts
			                        //to ~63fps and hides their real cost.
			if ((popts.rendcorn == 4) && (gmehax))
			{
				CheckMenuItem(gmenu,MENU_FULLSCREEN,0);
				popts.fullscreen = 0; resetwindows(SW_NORMAL);
			}
		}
		strcpy(otext,text); otsecn = tsecn; memcpy(otsec,tsec,tsecn*sizeof(tsec_t));

		SwapBuffers(hDC);

			//Omni benchmark: 30 warmup frames (script compile, textures), then wall clock.
		if (gbenchn)
		{
			gbenchi++;
			if (gbenchi == 1) ((PFNWGLSWAPINTERVALEXTPROC)glfp[wglSwapIntervalEXT])(0); //vsync off
			if (gbenchi == 30) QueryPerformanceCounter((LARGE_INTEGER *)&gbenchq0);
			if (gbenchi >= gbenchn+30)
			{
				__int64 qq; double dt; FILE *fp;
				QueryPerformanceCounter((LARGE_INTEGER *)&qq);
				dt = ((double)(qq-gbenchq0))/((double)qper);
				fp = fopen("polydraw_bench.txt","a");
				if (fp)
				{
					fprintf(fp,"%s\t%.2f\t%.3f\n",gsavfilnam,((double)gbenchn)/dt,dt*1000.0/((double)gbenchn));
					fclose(fp);
				}
				goto quitit;
			}
		}

		QueryPerformanceCounter((LARGE_INTEGER *)&q); qnum++;
		if ((q-qlast > qper) || (!gsavfilnamptr))
		{
			gsavfilnamptr = gsavfilnam;
			for(i=0;gsavfilnam[i];i++) if (gsavfilnam[i] == '\\') gsavfilnamptr = &gsavfilnam[i+1];

			i = sprintf(buf,"%s",prognam);
			if (gsavfilnam[0]) i += sprintf(&buf[i]," - %s",gsavfilnamptr);
			if (SendMessage(hWndEdit,EM_GETMODIFY,0,0)) i += sprintf(&buf[i]," *");
			i += sprintf(&buf[i]," (%.1f fps)",((double)qper)*((double)qnum)/((double)(q-qlast)));

			qlast = q; qnum = 0;
			SetWindowText(ghwnd,buf);
		}
	}
quitit:;
	//passasksave();

	if (supporttimerquery) ((PFNGLDELETEQUERIESPROC)glfp[glDeleteQueries])(1,queries);
	playnoteuninit();
	DisableOpenGL(hWndDraw,hDC,hRC);
	DestroyWindow(ghwnd);

	if (!gmehax) saveini();
	if (badlinebits) { free(badlinebits); badlinebits = 0; }
	if ( line) { free( line);  line = 0; }
	if (ttext) { free(ttext); ttext = 0; }
	if (otext) { free(otext); otext = 0; }
	if ( text) { free( text);  text = 0; }

	return(0);
}