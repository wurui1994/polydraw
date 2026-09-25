

//--------------------------------------------------------------------------------------------------
	//Find&replace working info found here:
	//ftp://ftp.microsoft.com/developr/drg/WWLive/broadcas/bookchap/nancy/code/cmndlg32/
	//   Download:cmndlg32.c,cmndlg32.h,cmndlg32.rc,resource.h; link:user32,gdi32,comdlg32,comctl32
	//
	//NOTE: 3 hacks must be placed outside this block:
	//1. IsDialogMessage() near PeekMessage/GetMessage.
	//2. if (msg == uFindReplaceMsg) ... in WndProc of edit control.
	//3. Remember to add |ES_NOHIDESEL to 4th parm of edit control's CreateWindow().
static HWND gfind_wnd = 0;
static unsigned int gfind_msg = 0;
static FINDREPLACE gfind_fr; //Must be global/static
static char gfind_findst[256], gfind_replacest[256]; //Must be global/static and sizeof >= 80
static int gfind_inited = 0;
static int gfind_mymemcmp (char *basestart, int baseind, char *findst, int findleng, int frflags)
{
	int i;
	unsigned char ch;
	if (frflags&FR_MATCHCASE) { if (memcmp (&basestart[baseind],findst,findleng)) return(-1); }
								else { if (memicmp(&basestart[baseind],findst,findleng)) return(-1); }
	if (!(frflags&FR_WHOLEWORD)) return(0);
	ch = basestart[baseind+findleng];
	for(i=2;i>0;i--)
	{
		if (((ch >= '0') && (ch <= '9')) || (((ch-'A')&0xdf) <= 'Z'-'A')) return(-1);
		if (!baseind) return(0); ch = basestart[baseind-1];
	}
	return(0);
}
static int findreplace_process (LPFINDREPLACE lpfr)
{
	HWND hwnd;
	int i, j, textleng, findleng, repleng, findsel0, findsel1;

	if (!lpfr) return(0);
	hwnd = lpfr->hwndOwner;

	if (lpfr->Flags&FR_DIALOGTERM) { SetFocus(hwnd); return(0); }
	if (lpfr->Flags&(FR_FINDNEXT|FR_REPLACE|FR_REPLACEALL))
	{
		GetWindowText(hwnd,ttext,textsiz); textleng = strlen(ttext);
		findleng = strlen(lpfr->lpstrFindWhat); repleng = strlen(lpfr->lpstrReplaceWith);

		SendMessage(hwnd,EM_GETSEL,(WPARAM)&findsel0,(LPARAM)&findsel1);

		if (lpfr->Flags&FR_REPLACEALL)
		{
			for(i=textleng-findleng;i>=0;i--)
			{
				if (gfind_mymemcmp(ttext,i,lpfr->lpstrFindWhat,findleng,lpfr->Flags)) continue;
				SendMessage(hwnd,EM_SETSEL,i,i+findleng);
				SendMessage(hwnd,EM_REPLACESEL,0,(LPARAM)lpfr->lpstrReplaceWith);
				i += 1-findleng;
				if (findsel0 < i) findsel0 += repleng-findleng;
				if (findsel1 < i) findsel1 += repleng-findleng;
			}
			SendMessage(hwnd,EM_SETSEL,findsel0,findsel1);
			SendMessage(hwnd,EM_SCROLLCARET,0,0);
			SendMessage(gfind_wnd,WM_CLOSE,0,0);
			MessageBeep(0);
			return(0);
		}

		i = findsel0;

		if ((lpfr->Flags&FR_REPLACE) && (findsel1-findsel0 == findleng) && (findsel0 <= textleng-findleng))
			if (!gfind_mymemcmp(ttext,i,lpfr->lpstrFindWhat,findleng,lpfr->Flags))
			{
				SendMessage(hwnd,EM_REPLACESEL,0,(LPARAM)lpfr->lpstrReplaceWith);
				GetWindowText(hwnd,ttext,textleng); textleng = strlen(ttext);
			}

		for(j=0;j<textleng;j++)
		{
			if (lpfr->Flags&FR_DOWN) { i++; if (i >= textleng) i -= textleng; }
									  else { i--; if (i <         0) i += textleng; }
			if ((i <= textleng-findleng) && (!gfind_mymemcmp(ttext,i,lpfr->lpstrFindWhat,findleng,lpfr->Flags))) break;
		}
		if (j >= textleng) { SendMessage(hwnd,EM_SETSEL,findsel0,findsel0); MessageBeep(0); return(0); }

		SendMessage(hwnd,EM_SETSEL,i,i+findleng);
		SendMessage(hwnd,EM_SCROLLCARET,0,0);

		{ //Move find/replace dialog out of way if covering highlighted text
		RECT rfind, redit;
		POINT p0, pchar;

		GetWindowRect(gfind_wnd,&rfind);

		p0.x = p0.y = 0; ClientToScreen(hwnd,&p0);
		SendMessage(hwnd,EM_GETRECT,0,(LPARAM)&redit);
		j = SendMessage(hwnd,EM_POSFROMCHAR,i,0); pchar.x = LOWORD(j); pchar.y = HIWORD(j);
		j = ((labs(popts.fontheight)*20)>>4); //Estimated character height

		if ((p0.y+pchar.y+j >= rfind.top) && (p0.y+pchar.y < rfind.bottom))
		{
			if ((p0.y+pchar.y+(j>>1))*2 < rfind.top+rfind.bottom)
				  MoveWindow(gfind_wnd,rfind.left,p0.y+pchar.y+j                     ,rfind.right-rfind.left,rfind.bottom-rfind.top,1);
			else MoveWindow(gfind_wnd,rfind.left,p0.y+pchar.y+rfind.top-rfind.bottom,rfind.right-rfind.left,rfind.bottom-rfind.top,1);
		}
		}
	}
	return(0);
}
static void findreplace (HWND hwnd, int isreplace)
{
	if (!gfind_msg)
	{
		gfind_msg = RegisterWindowMessage(FINDMSGSTRING);
		gfind_findst[0] = 0; gfind_replacest[0] = 0;

		gfind_fr.lStructSize = sizeof(gfind_fr);
		gfind_fr.hwndOwner = hwnd;
		gfind_fr.hInstance = ghinst;
		gfind_fr.Flags = FR_DOWN;
		gfind_fr.lpstrFindWhat    = gfind_findst;
		gfind_fr.lpstrReplaceWith = gfind_replacest;
		gfind_fr.wFindWhatLen     = sizeof(gfind_findst);
		gfind_fr.wReplaceWithLen  = sizeof(gfind_replacest);
		gfind_fr.lCustData = 0;
		gfind_fr.lpfnHook = 0;
		gfind_fr.lpTemplateName = 0;

	}
		//|FR_DOWN     |FR_NOUPDOWN   |FR_HIDEUPDOWN
		//|FR_WHOLEWORD|FR_NOWHOLEWORD|FR_HIDEWHOLEWORD
		//|FR_MATCHCASE|FR_NOMATCHCASE|FR_HIDEMATCHCASE
		//|FR_FINDNEXT|FR_REPLACE|FR_REPLACEALL
		//|FR_DIALOGTERM|FR_SHOWHELP|FR_ENABLEHOOK
		//|FR_ENABLETEMPLATE|FR_ENABLETEMPLATEHANDLE
	gfind_fr.Flags &= ~FR_DIALOGTERM; //Need this to prevent bad things on 2nd call

	if (!isreplace) gfind_wnd = FindText(&gfind_fr);
				  else gfind_wnd = ReplaceText(&gfind_fr);
}
static void findnext (int isnext)
{
	if ((!gfind_findst[0]) || (!gfind_msg)) return;
	gfind_fr.Flags &= ~(FR_DIALOGTERM|FR_REPLACE|FR_REPLACEALL);
	gfind_fr.Flags |= FR_FINDNEXT;
	if (isnext) gfind_fr.Flags |= FR_DOWN;
			 else gfind_fr.Flags &=~FR_DOWN;
	findreplace_process(&gfind_fr);
}