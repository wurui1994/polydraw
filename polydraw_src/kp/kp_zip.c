

//==================== External picture interface ends =======================

	//Brute-force case-insensitive, slash-insensitive, * and ? wildcard matcher
	//Given: string i and string j. string j can have wildcards
	//Returns: 1:matches, 0:doesn't match
static int wildmatch (const char *i, const char *j)
{
	const char *k;
	char c0, c1;

	if (!*j) return(1);
	do
	{
		if (*j == '*')
		{
			for(k=i,j++;*k;k++) if (wildmatch(k,j)) return(1);
			continue;
		}
		if (!*i) return(0);
		if (*j == '?') { i++; j++; continue; }
		c0 = *i; if ((c0 >= 'a') && (c0 <= 'z')) c0 -= 32;
		c1 = *j; if ((c1 >= 'a') && (c1 <= 'z')) c1 -= 32;
		if (c0 == '/') c0 = '\\';
		if (c1 == '/') c1 = '\\';
		if (c0 != c1) return(0);
		i++; j++;
	} while (*j);
	return(!*i);
}

	//Same as: stricmp(st0,st1) except: '/' == '\'
static int filnamcmp (const char *st0, const char *st1)
{
	int i;
	char ch0, ch1;

	for(i=0;st0[i];i++)
	{
		ch0 = st0[i]; if ((ch0 >= 'a') && (ch0 <= 'z')) ch0 -= 32;
		ch1 = st1[i]; if ((ch1 >= 'a') && (ch1 <= 'z')) ch1 -= 32;
		if (ch0 == '/') ch0 = '\\';
		if (ch1 == '/') ch1 = '\\';
		if (ch0 != ch1) return(-1);
	}
	if (!st1[i]) return(0);
	return(-1);
}

//===================== ZIP decompression code begins ========================

	//format: (used by kzaddstack/kzopen to cache file name&start info)
	//[char zipnam[?]\0]
	//[next hashindex/-1][next index/-1][zipnam index][fileoffs][fileleng][iscomp][char filnam[?]\0]
	//[next hashindex/-1][next index/-1][zipnam index][fileoffs][fileleng][iscomp][char filnam[?]\0]
	//...
	//[char zipnam[?]\0]
	//[next hashindex/-1][next index/-1][zipnam index][fileoffs][fileleng][iscomp][char filnam[?]\0]
	//[next hashindex/-1][next index/-1][zipnam index][fileoffs][fileleng][iscomp][char filnam[?]\0]
	//...
#define KZHASHINITSIZE 8192
static char *kzhashbuf = 0;
static int kzhashead[256], kzhashpos, kzlastfnam, kzhashsiz, kzdirnamhead = -1;

static int kzcheckhashsiz (int siz)
{
	int i;

	if (!kzhashbuf) //Initialize hash table on first call
	{
		memset(kzhashead,-1,sizeof(kzhashead));
		kzhashbuf = (char *)malloc(KZHASHINITSIZE); if (!kzhashbuf) return(0);
		kzhashpos = 0; kzlastfnam = -1; kzhashsiz = KZHASHINITSIZE; kzdirnamhead = -1;
	}
	if (kzhashpos+siz > kzhashsiz) //Make sure string fits in kzhashbuf
	{
		i = kzhashsiz; do { i <<= 1; } while (kzhashpos+siz > i);
		kzhashbuf = (char *)realloc(kzhashbuf,i); if (!kzhashbuf) return(0);
		kzhashsiz = i;
	}
	return(1);
}

static int kzcalchash (const char *st)
{
	int i, hashind;
	char ch;

	for(i=0,hashind=0;st[i];i++)
	{
		ch = st[i];
		if ((ch >= 'a') && (ch <= 'z')) ch -= 32;
		if (ch == '/') ch = '\\';
		hashind = (ch - hashind*3);
	}
	return(hashind%(sizeof(kzhashead)/sizeof(kzhashead[0])));
}

static int kzcheckhash (const char *filnam, char **zipnam, int *fileoffs, int *fileleng, char *iscomp)
{
	int i;

	if (!kzhashbuf) return(0);
	if (filnam[0] == '|') filnam++;
	for(i=kzhashead[kzcalchash(filnam)];i>=0;i=(*(int *)&kzhashbuf[i]))
		if (!filnamcmp(filnam,&kzhashbuf[i+21]))
		{
			(*zipnam) = &kzhashbuf[*(int *)&kzhashbuf[i+8]];
			(*fileoffs) = *(int *)&kzhashbuf[i+12];
			(*fileleng) = *(int *)&kzhashbuf[i+16];
			(*iscomp) = kzhashbuf[i+20];
			return(1);
		}
	return(0);
}

void kzuninit ()
{
	if (kzhashbuf) { free(kzhashbuf); kzhashbuf = 0; }
	kzhashpos = kzhashsiz = 0; kzdirnamhead = -1;
}

	//If file found, loads internal directory from ZIP/GRP into memory (hash) to allow faster access later
	//If file not found, assumes it's a directory and adds it to an internal list
int kzaddstack (const char *filnam)
{
	FILE *fil;
	int i, j, k, leng, hashind, zipnamoffs, numfiles;
	char tempbuf[260+46];

	fil = fopen(filnam,"rb");
	if (!fil) //if file not found, assume it's a directory
	{
			//Add directory name to internal list (using kzhashbuf for convenience of dynamic allocation)
		i = strlen(filnam)+5; if (!kzcheckhashsiz(i)) return(-1);
		*(int *)&kzhashbuf[kzhashpos] = kzdirnamhead; kzdirnamhead = kzhashpos;
		strcpy(&kzhashbuf[kzhashpos+4],filnam);
		kzhashpos += i;

		return(-1);
	}

		//Write ZIP/GRP/EXE filename to hash
	i = strlen(filnam)+1; if (!kzcheckhashsiz(i)) { fclose(fil); return(-1); }
	strcpy(&kzhashbuf[kzhashpos],filnam);
	zipnamoffs = kzhashpos; kzhashpos += i;

	fread(&i,4,1,fil);
	if (i == LSWAPIB(0x04034b50)) //'PK\3\4' is ZIP file id
	{
		fseek(fil,-22,SEEK_END);
		fread(tempbuf,22,1,fil);
		if (*(int *)&tempbuf[0] == LSWAPIB(0x06054b50)) //Fast way of finding dir info
		{
			numfiles = SSWAPIB(*(short *)&tempbuf[10]);
			fseek(fil,LSWAPIB(*(int *)&tempbuf[16]),SEEK_SET);
		}
		else //Slow way of finding dir info (used when ZIP has junk at end)
		{
			fseek(fil,0,SEEK_SET); numfiles = 0;
			while (1)
			{
				if (!fread(&j,4,1,fil)) { numfiles = -1; break; }
				if (j == LSWAPIB(0x02014b50)) break; //Found central file header :)
				if (j != LSWAPIB(0x04034b50)) { numfiles = -1; break; }
				fread(tempbuf,26,1,fil);
				fseek(fil,LSWAPIB(*(int *)&tempbuf[14]) + SSWAPIB(*(short *)&tempbuf[24]) + SSWAPIB(*(short *)&tempbuf[22]),SEEK_CUR);
				numfiles++;
			}
			if (numfiles < 0) { fclose(fil); return(-1); }
			fseek(fil,-4,SEEK_CUR);
		}
		for(i=0;i<numfiles;i++)
		{
			fread(tempbuf,46,1,fil);
			if (*(int *)&tempbuf[0] != LSWAPIB(0x02014b50)) { fclose(fil); return(0); }

			j = SSWAPIB(*(short *)&tempbuf[28]); //filename length
			fread(&tempbuf[46],j,1,fil);
			tempbuf[j+46] = 0;

				//Write information into hash
			j = strlen(&tempbuf[46])+22; if (!kzcheckhashsiz(j)) { fclose(fil); return(-1); }
			hashind = kzcalchash(&tempbuf[46]);
			*(int *)&kzhashbuf[kzhashpos] = kzhashead[hashind];
			*(int *)&kzhashbuf[kzhashpos+4] = kzlastfnam;
			*(int *)&kzhashbuf[kzhashpos+8] = zipnamoffs;
			*(int *)&kzhashbuf[kzhashpos+12] = LSWAPIB(*(int *)&tempbuf[42]); //fileoffs
			*(int *)&kzhashbuf[kzhashpos+16] = 0; //fileleng not used for ZIPs (reserve space for simplicity)
			*(int *)&kzhashbuf[kzhashpos+20] = 1; //iscomp
			strcpy(&kzhashbuf[kzhashpos+21],&tempbuf[46]);
			kzhashead[hashind] = kzhashpos; kzlastfnam = kzhashpos; kzhashpos += j;

			j  = SSWAPIB(*(short *)&tempbuf[30]); //extra field length
			j += SSWAPIB(*(short *)&tempbuf[32]); //file comment length
			fseek(fil,j,SEEK_CUR);
		}
	}
	else if (i == LSWAPIB(0x536e654b)) //'KenS' is GRP file id
	{
		fread(tempbuf,12,1,fil);
		if ((*(int *)&tempbuf[0] != LSWAPIB(0x65766c69)) || //'ilve'
				(*(int *)&tempbuf[4] != LSWAPIB(0x6e616d72)))   //'rman'
			 { fclose(fil); return(0); }
		numfiles = LSWAPIB(*(int *)&tempbuf[8]); k = ((numfiles+1)<<4);
		for(i=0;i<numfiles;i++,k+=leng)
		{
			fread(tempbuf,16,1,fil);
			leng = LSWAPIB(*(int *)&tempbuf[12]); //File length
			tempbuf[12] = 0;

				//Write information into hash
			j = strlen(tempbuf)+22; if (!kzcheckhashsiz(j)) { fclose(fil); return(-1); }
			hashind = kzcalchash(tempbuf);
			*(int *)&kzhashbuf[kzhashpos] = kzhashead[hashind];
			*(int *)&kzhashbuf[kzhashpos+4] = kzlastfnam;
			*(int *)&kzhashbuf[kzhashpos+8] = zipnamoffs;
			*(int *)&kzhashbuf[kzhashpos+12] = k; //fileoffs
			*(int *)&kzhashbuf[kzhashpos+16] = leng; //fileleng
			*(int *)&kzhashbuf[kzhashpos+20] = 0; //iscomp
			strcpy(&kzhashbuf[kzhashpos+21],tempbuf);
			kzhashead[hashind] = kzhashpos; kzlastfnam = kzhashpos; kzhashpos += j;
		}
	}
	else if ((i&0xffff) == SSWAPIB(0x5a4d)) //'MZ' is EXE file id (EXE with custom data format appended to end)
	{
		int appendeddatastart, offs, fileng;
		unsigned short s, numheads;

			//Get original EXE size (see: http://www.rhinocerus.net/forum/lang-asm-x86/256686-appending-data-exe-file.html)
		fseek(fil,60,SEEK_SET); fread(&offs,4,1,fil); offs = LSWAPIB(offs); fseek(fil,offs,SEEK_SET);
		fread(&i,4,1,fil); if (i != LSWAPIB(0x00004550)) { fclose(fil); return(0); } //PE
		fread(&s,2,1,fil); //dummy
		fread(&numheads,2,1,fil); numheads = SSWAPIB(numheads);
		appendeddatastart = 0;
		fseek(fil,256,SEEK_CUR);

		for(;numheads>0;numheads--) //walk headers
		{
			fread(&i,4,1,fil); i = LSWAPIB(i); //offs
			fread(&j,4,1,fil); j = LSWAPIB(j); //leng
			if (i+j > appendeddatastart) appendeddatastart = i+j;
			fseek(fil,32,SEEK_CUR);
		}

		fseek(fil,appendeddatastart,SEEK_SET);
		if (fread(&numfiles,1,4,fil) != 4) { fclose(fil); return(-1); } //no appended data
		numfiles = LSWAPIB(numfiles);
		for(i=numfiles;i>0;i--) { while (fgetc(fil) > 0); fseek(fil,4,SEEK_CUR); } //Get file list size

		offs = ftell(fil);
		fseek(fil,appendeddatastart+4,SEEK_SET);
		for(i=0;i<numfiles;i++,offs+=leng)
		{
			j = 0; do { k = fgetc(fil); tempbuf[j] = k; j++; } while (k > 0);
			fread(&leng,1,4,fil);

				//Write information into hash
			j = strlen(tempbuf)+22; if (!kzcheckhashsiz(j)) { fclose(fil); return(-1); }
			hashind = kzcalchash(tempbuf);
			*(int *)&kzhashbuf[kzhashpos] = kzhashead[hashind];
			*(int *)&kzhashbuf[kzhashpos+4] = kzlastfnam;
			*(int *)&kzhashbuf[kzhashpos+8] = zipnamoffs;
			*(int *)&kzhashbuf[kzhashpos+12] = offs; //fileoffs
			*(int *)&kzhashbuf[kzhashpos+16] = leng; //fileleng
			*(int *)&kzhashbuf[kzhashpos+20] = 0; //iscomp
			strcpy(&kzhashbuf[kzhashpos+21],tempbuf);
			kzhashead[hashind] = kzhashpos; kzlastfnam = kzhashpos; kzhashpos += j;
		}
	}
	fclose(fil);
	return(0);
}

  //this allows the use of kplib.c with a file that is already open
void kzsetfil (FILE *fil)
{
	kzfs.fil = fil;
	kzfs.comptyp = 0;
	kzfs.seek0 = 0;
	kzfs.leng = filelength(_fileno(kzfs.fil));
	kzfs.pos = 0;
	kzfs.i = 0;
}

INT_PTR kzopen (const char *filnam)
{
	FILE *fil;
	int i, j, fileoffs, fileleng;
	char tempbuf[46+260], *zipnam, iscomp;

	//kzfs.fil = 0;
	if (filnam[0] != '|') //Search standalone file first
	{
		kzfs.fil = fopen(filnam,"rb");
		if (kzfs.fil)
		{
			kzfs.comptyp = 0;
			kzfs.seek0 = 0;
			kzfs.leng = filelength(_fileno(kzfs.fil));
			kzfs.pos = 0;
			kzfs.i = 0;
			return((INT_PTR)kzfs.fil);
		}
	}
	if (kzcheckhash(filnam,&zipnam,&fileoffs,&fileleng,&iscomp)) //Then check mounted ZIP/GRP files
	{
		fil = fopen(zipnam,"rb"); if (!fil) return(0);
		fseek(fil,fileoffs,SEEK_SET);
		if (!iscomp) //Must be from GRP file
		{
			kzfs.fil = fil;
			kzfs.comptyp = 0;
			kzfs.seek0 = fileoffs;
			kzfs.leng = fileleng;
			kzfs.pos = 0;
			kzfs.i = 0;
			return((INT_PTR)kzfs.fil);
		}
		else
		{
			fread(tempbuf,30,1,fil);
			if (*(int *)&tempbuf[0] != LSWAPIB(0x04034b50)) { fclose(fil); return(0); }
			fseek(fil,SSWAPIB(*(short *)&tempbuf[26])+SSWAPIB(*(short *)&tempbuf[28]),SEEK_CUR);

			kzfs.fil = fil;
			kzfs.comptyp = SSWAPIB(*(short *)&tempbuf[8]);
			kzfs.seek0 = ftell(fil);
			kzfs.leng = LSWAPIB(*(int *)&tempbuf[22]);
			kzfs.pos = 0;
			switch(kzfs.comptyp) //Compression method
			{
				case 0: kzfs.i = 0; return((INT_PTR)kzfs.fil);
				case 8:
					if (!pnginited) { pnginited = 1; initpngtables(); }
					kzfs.comptell = 0;
					kzfs.compleng = LSWAPIB(*(int *)&tempbuf[18]);

						//WARNING: No file in ZIP can be > 2GB-32K bytes
					gslidew = 0x7fffffff; //Force reload at beginning

					return((INT_PTR)kzfs.fil);
				default: fclose(kzfs.fil); kzfs.fil = 0; return(0);
			}
		}
	}

		//Finally, check mounted dirs
	for(i=kzdirnamhead;i>=0;i=*(int *)&kzhashbuf[i])
	{
		strcpy(tempbuf,&kzhashbuf[i+4]);
		j = strlen(tempbuf);
		if (strlen(filnam)+1+j >= sizeof(tempbuf)) continue; //don't allow long filenames to buffer overrun
		if ((j) && (tempbuf[j-1] != '/') && (tempbuf[j-1] != '\\') && (filnam[0] != '/') && (filnam[0] != '\\'))
#if (defined(__DOS__) || defined(_WIN32))
			strcat(tempbuf,"\\");
#else
			strcat(tempbuf,"/");
#endif
		strcat(tempbuf,filnam);
		kzfs.fil = fopen(tempbuf,"rb");
		if (kzfs.fil)
		{
			kzfs.comptyp = 0;
			kzfs.seek0 = 0;
			kzfs.leng = filelength(_fileno(kzfs.fil));
			kzfs.pos = 0;
			kzfs.i = 0;
			return((INT_PTR)kzfs.fil);
		}
	}

	return(0);
}

// --------------------------------------------------------------------------

#if defined(__DOS__)
#define MAX_PATH 260
static struct find_t findata;
#elif defined(_WIN32)
static HANDLE hfind = INVALID_HANDLE_VALUE;
static WIN32_FIND_DATA findata;
#else
#define MAX_PATH 260
static DIR *hfind = NULL;
static struct dirent *findata = NULL;
#endif

	//File find state variables. Example sequence (read top->bot, left->right):
	//   srchstat   srchzoff    srchdoff
	//   0,1,2,3
	//              500,200,-1
	//           4              300
	//   0,1,2,3,4              100
	//   0,1,2,3,4              -1
static int srchstat = -1, srchzoff = 0, srchdoff = -1, wildstpathleng;
static char wildst[MAX_PATH] = "", newildst[MAX_PATH] = "";

void kzfindfilestart (const char *st)
{
#if defined(__DOS__)
#elif defined(_WIN32)
	if (hfind != INVALID_HANDLE_VALUE)
		{ FindClose(hfind); hfind = INVALID_HANDLE_VALUE; }
#else
	if (hfind) { closedir(hfind); hfind = NULL; }
#endif
	strcpy(wildst,st); strcpy(newildst,st);
	srchstat = 0; srchzoff = kzlastfnam; srchdoff = kzdirnamhead;
}

int kzfindfile (char *filnam)
{
	int i;

kzfindfile_beg:;
	filnam[0] = 0;
	if (srchstat == 0)
	{
		if (!newildst[0]) { srchstat = -1; return(0); }
		do
		{
			srchstat = 1;

				//Extract directory from wildcard string for pre-pending
			wildstpathleng = 0;
			for(i=0;newildst[i];i++)
				if ((newildst[i] == '/') || (newildst[i] == '\\'))
					wildstpathleng = i+1;

			memcpy(filnam,newildst,wildstpathleng);

#if defined(__DOS__)
			if (_dos_findfirst(newildst,_A_SUBDIR,&findata))
				{ if (!kzhashbuf) return(0); srchstat = 2; continue; }
			i = wildstpathleng;
			if (findata.attrib&16)
				if ((findata.name[0] == '.') && (!findata.name[1])) continue;
			strcpy(&filnam[i],findata.name);
			if (findata.attrib&16) strcat(&filnam[i],"\\");
#elif defined(_WIN32)
			hfind = FindFirstFile(newildst,&findata);
			if (hfind == INVALID_HANDLE_VALUE)
				{ if (!kzhashbuf) return(0); srchstat = 2; continue; }
			if (findata.dwFileAttributes&FILE_ATTRIBUTE_HIDDEN) continue;
			i = wildstpathleng;
			if (findata.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)
				if ((findata.cFileName[0] == '.') && (!findata.cFileName[1])) continue;
			strcpy(&filnam[i],findata.cFileName);
			if (findata.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) strcat(&filnam[i],"\\");
#else
			if (!hfind)
			{
				char *s = (char *)".";
				if (wildstpathleng > 0) {
					filnam[wildstpathleng] = 0;
					s = filnam;
				}
				hfind = opendir(s);
				if (!hfind) { if (!kzhashbuf) return 0; srchstat = 2; continue; }
			}
			break;   // process srchstat == 1
#endif
			return(1);
		} while (0);
	}
	if (srchstat == 1)
	{
		while (1)
		{
			memcpy(filnam,newildst,wildstpathleng);
#if defined(__DOS__)
			if (_dos_findnext(&findata))
				{ if (!kzhashbuf) return(0); srchstat = 2; break; }
			i = wildstpathleng;
			if (findata.attrib&16)
				if ((findata.name[0] == '.') && (!findata.name[1])) continue;
			strcpy(&filnam[i],findata.name);
			if (findata.attrib&16) strcat(&filnam[i],"\\");
#elif defined(_WIN32)
			if (!FindNextFile(hfind,&findata))
				{ FindClose(hfind); hfind = INVALID_HANDLE_VALUE; if (!kzhashbuf) return(0); srchstat = 2; break; }
			if (findata.dwFileAttributes&FILE_ATTRIBUTE_HIDDEN) continue;
			i = wildstpathleng;
			if (findata.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)
				if ((findata.cFileName[0] == '.') && (!findata.cFileName[1])) continue;
			strcpy(&filnam[i],findata.cFileName);
			if (findata.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) strcat(&filnam[i],"\\");
#else
			if ((findata = readdir(hfind)) == NULL)
				{ closedir(hfind); hfind = NULL; if (!kzhashbuf) return 0; srchstat = 2; break; }
			i = wildstpathleng;
			if (findata->d_type == DT_DIR)
				{ if (findata->d_name[0] == '.' && !findata->d_name[1]) continue; } //skip .
			else if ((findata->d_type == DT_REG) || (findata->d_type == DT_LNK))
				{ if (findata->d_name[0] == '.') continue; } //skip hidden (dot) files
			else continue; //skip devices and fifos and such
			if (!wildmatch(findata->d_name,&newildst[wildstpathleng])) continue;
			strcpy(&filnam[i],findata->d_name);
			if (findata->d_type == DT_DIR) strcat(&filnam[i],"/");
#endif
			return(1);
		}
	}
	while (srchstat == 2)
	{
		if (srchzoff < 0) { srchstat = 3; break; }
		if (wildmatch(&kzhashbuf[srchzoff+21],newildst))
		{
			//strcpy(filnam,&kzhashbuf[srchzoff+21]);
			filnam[0] = '|'; strcpy(&filnam[1],&kzhashbuf[srchzoff+21]);
			srchzoff = *(int *)&kzhashbuf[srchzoff+4];
			return(1);
		}
		srchzoff = *(int *)&kzhashbuf[srchzoff+4];
	}
	while (srchstat == 3)
	{
		if (srchdoff < 0) { srchstat = -1; break; }
		strcpy(newildst,&kzhashbuf[srchdoff+4]);
		i = strlen(newildst);
		if ((i) && (newildst[i-1] != '/') && (newildst[i-1] != '\\') && (filnam[0] != '/') && (filnam[0] != '\\'))
#if (defined(__DOS__) || defined(_WIN32))
			strcat(newildst,"\\");
#else
			strcat(newildst,"/");
#endif
		strcat(newildst,wildst);
		srchdoff = *(int *)&kzhashbuf[srchdoff];
		srchstat = 0; goto kzfindfile_beg;
	}

	return(0);
}

//File searching code (supports inside ZIP files!) How to use this code:
//   char filnam[MAX_PATH];
//   kzfindfilestart("vxl/*.vxl");
//   while (kzfindfile(filnam)) puts(filnam);
//NOTES:
// * Directory names end with '\' or '/' (depending on system)
// * Files inside zip begin with '|'

// --------------------------------------------------------------------------

static char *gzbufptr;
static void putbuf4zip (const unsigned char *buf, int uncomp0, int uncomp1)
{
	int i0, i1;
		//              uncomp0 ... uncomp1
		//  &gzbufptr[kzfs.pos] ... &gzbufptr[kzfs.endpos];
	i0 = max(uncomp0,kzfs.pos);
	i1 = min(uncomp1,kzfs.endpos);
	if (i0 < i1) memcpy(&gzbufptr[i0],&buf[i0-uncomp0],i1-i0);
}

	//returns number of bytes copied
int kzread (void *buffer, int leng)
{
	int i, j, k, bfinal, btype, hlit, hdist;

	if ((!kzfs.fil) || (leng <= 0)) return(0);

	if (kzfs.comptyp == 0)
	{
		if (kzfs.pos != kzfs.i) //Seek only when position changes
			fseek(kzfs.fil,kzfs.seek0+kzfs.pos,SEEK_SET);
		i = min(kzfs.leng-kzfs.pos,leng);
		fread(buffer,i,1,kzfs.fil);
		kzfs.i += i; //kzfs.i is a local copy of ftell(kzfs.fil);
	}
	else if (kzfs.comptyp == 8)
	{
		zipfilmode = 1;

			//Initialize for putbuf4zip
		gzbufptr = (char *)buffer; gzbufptr = &gzbufptr[-kzfs.pos];
		kzfs.endpos = min(kzfs.pos+leng,kzfs.leng);
		if (kzfs.endpos == kzfs.pos) return(0); //Guard against reading 0 length

		if (kzfs.pos < gslidew-32768) // Must go back to start :(
		{
			if (kzfs.comptell) fseek(kzfs.fil,kzfs.seek0,SEEK_SET);

			gslidew = 0; gslider = 16384;
			kzfs.jmpplc = 0;

				//Initialize for suckbits/peekbits/getbits
			kzfs.comptell = min((unsigned)kzfs.compleng,sizeof(olinbuf));
			fread(&olinbuf[0],kzfs.comptell,1,kzfs.fil);
				//Make it re-load when there are < 32 bits left in FIFO
			bitpos = -(((int)sizeof(olinbuf)-4)<<3);
				//Identity: filptr + (bitpos>>3) = &olinbuf[0]
			filptr = &olinbuf[-(bitpos>>3)];
		}
		else
		{
			i = max(gslidew-32768,0); j = gslider-16384;

				//HACK: Don't unzip anything until you have to...
				//   (keeps file pointer as low as possible)
			if (kzfs.endpos <= gslidew) j = kzfs.endpos;

				//write uncompoffs on slidebuf from: i to j
			if (!((i^j)&32768))
				putbuf4zip(&slidebuf[i&32767],i,j);
			else
			{
				putbuf4zip(&slidebuf[i&32767],i,j&~32767);
				putbuf4zip(slidebuf,j&~32767,j);
			}

				//HACK: Don't unzip anything until you have to...
				//   (keeps file pointer as low as possible)
			if (kzfs.endpos <= gslidew) goto retkzread;
		}

		switch (kzfs.jmpplc)
		{
			case 0: goto kzreadplc0;
			case 1: goto kzreadplc1;
			case 2: goto kzreadplc2;
			case 3: goto kzreadplc3;
		}
kzreadplc0:;
		do
		{
			bfinal = getbits(1); btype = getbits(2);

#if 0
				//Display Huffman block offsets&lengths of input file - for debugging only!
			{
			static int ouncomppos = 0, ocomppos = 0;
			if (kzfs.comptell == sizeof(olinbuf)) i = 0;
			else if (kzfs.comptell < kzfs.compleng) i = kzfs.comptell-(sizeof(olinbuf)-4);
			else i = kzfs.comptell-(kzfs.comptell%(sizeof(olinbuf)-4));
			i += ((char *)&filptr[bitpos>>3])-((char *)(&olinbuf[0]));
			i = (i<<3)+(bitpos&7)-3;
			if (gslidew) printf(" ULng:0x%08x CLng:0x%08x.%x",gslidew-ouncomppos,(i-ocomppos)>>3,((i-ocomppos)&7)<<1);
			printf("\ntype:%d, Uoff:0x%08x Coff:0x%08x.%x",btype,gslidew,i>>3,(i&7)<<1);
			if (bfinal)
			{
				printf(" ULng:0x%08x CLng:0x%08x.%x",kzfs.leng-gslidew,((kzfs.compleng<<3)-i)>>3,(((kzfs.compleng<<3)-i)&7)<<1);
				printf("\n        Uoff:0x%08x Coff:0x%08x.0",kzfs.leng,kzfs.compleng);
				ouncomppos = ocomppos = 0;
			}
			else { ouncomppos = gslidew; ocomppos = i; }
			}
#endif

			if (btype == 0)
			{
				  //Raw (uncompressed)
				suckbits((-bitpos)&7);  //Synchronize to start of next byte
				i = getbits(16); if ((getbits(16)^i) != 0xffff) return(-1);
				for(;i;i--)
				{
					if (gslidew >= gslider)
					{
						putbuf4zip(&slidebuf[(gslider-16384)&32767],gslider-16384,gslider); gslider += 16384;
						if (gslider-16384 >= kzfs.endpos)
						{
							kzfs.jmpplc = 1; kzfs.i = i; kzfs.bfinal = bfinal;
							goto retkzread;
kzreadplc1:;         i = kzfs.i; bfinal = kzfs.bfinal;
						}
					}
					slidebuf[(gslidew++)&32767] = (char)getbits(8);
				}
				continue;
			}
			if (btype == 3) continue;

			if (btype == 1) //Fixed Huffman
			{
				hlit = 288; hdist = 32; i = 0;
				for(;i<144;i++) clen[i] = 8; //Fixed bit sizes (literals)
				for(;i<256;i++) clen[i] = 9; //Fixed bit sizes (literals)
				for(;i<280;i++) clen[i] = 7; //Fixed bit sizes (EOI,lengths)
				for(;i<288;i++) clen[i] = 8; //Fixed bit sizes (lengths)
				for(;i<320;i++) clen[i] = 5; //Fixed bit sizes (distances)
			}
			else  //Dynamic Huffman
			{
				hlit = getbits(5)+257; hdist = getbits(5)+1; j = getbits(4)+4;
				for(i=0;i<j;i++) cclen[ccind[i]] = getbits(3);
				for(;i<19;i++) cclen[ccind[i]] = 0;
				hufgencode(cclen,19,ibuf0,nbuf0);

				j = 0; k = hlit+hdist;
				while (j < k)
				{
					i = hufgetsym(ibuf0,nbuf0);
					if (i < 16) { clen[j++] = i; continue; }
					if (i == 16)
						{ for(i=getbits(2)+3;i;i--) { clen[j] = clen[j-1]; j++; } }
					else
					{
						if (i == 17) i = getbits(3)+3; else i = getbits(7)+11;
						for(;i;i--) clen[j++] = 0;
					}
				}
			}

			hufgencode(clen,hlit,ibuf0,nbuf0);
			qhufgencode(ibuf0,nbuf0,qhufval0,qhufbit0,LOGQHUFSIZ0);

			hufgencode(&clen[hlit],hdist,ibuf1,nbuf1);
			qhufgencode(ibuf1,nbuf1,qhufval1,qhufbit1,LOGQHUFSIZ1);

			while (1)
			{
				if (gslidew >= gslider)
				{
					putbuf4zip(&slidebuf[(gslider-16384)&32767],gslider-16384,gslider); gslider += 16384;
					if (gslider-16384 >= kzfs.endpos)
					{
						kzfs.jmpplc = 2; kzfs.bfinal = bfinal; goto retkzread;
kzreadplc2:;      bfinal = kzfs.bfinal;
					}
				}

				k = peekbits(LOGQHUFSIZ0);
				if (qhufbit0[k]) { i = qhufval0[k]; suckbits((int)qhufbit0[k]); }
				else i = hufgetsym(ibuf0,nbuf0);

				if (i < 256) { slidebuf[(gslidew++)&32767] = (char)i; continue; }
				if (i == 256) break;
				i = getbits(hxbit[i+30-257][0]) + hxbit[i+30-257][1];

				k = peekbits(LOGQHUFSIZ1);
				if (qhufbit1[k]) { j = qhufval1[k]; suckbits((int)qhufbit1[k]); }
				else j = hufgetsym(ibuf1,nbuf1);

				j = getbits(hxbit[j][0]) + hxbit[j][1];
				for(;i;i--,gslidew++) slidebuf[gslidew&32767] = slidebuf[(gslidew-j)&32767];
			}
		} while (!bfinal);

		gslider -= 16384;
		if (!((gslider^gslidew)&32768))
			putbuf4zip(&slidebuf[gslider&32767],gslider,gslidew);
		else
		{
			putbuf4zip(&slidebuf[gslider&32767],gslider,gslidew&~32767);
			putbuf4zip(slidebuf,gslidew&~32767,gslidew);
		}
kzreadplc3:; kzfs.jmpplc = 3;
	}

retkzread:;
	i = kzfs.pos;
	kzfs.pos += leng; if (kzfs.pos > kzfs.leng) kzfs.pos = kzfs.leng;
	return(kzfs.pos-i);
}

int kzfilelength ()
{
	if (!kzfs.fil) return(0);
	return(kzfs.leng);
}

	//WARNING: kzseek(<-32768,SEEK_CUR); or:
	//         kzseek(0,SEEK_END);       can make next kzread very slow!!!
int kzseek (int offset, int whence)
{
	if (!kzfs.fil) return(-1);
	switch (whence)
	{
		case SEEK_CUR: kzfs.pos += offset; break;
		case SEEK_END: kzfs.pos = kzfs.leng+offset; break;
		case SEEK_SET: default: kzfs.pos = offset;
	}
	if (kzfs.pos < 0) kzfs.pos = 0;
	if (kzfs.pos > kzfs.leng) kzfs.pos = kzfs.leng;
	return(kzfs.pos);
}

int kztell ()
{
	if (!kzfs.fil) return(-1);
	return(kzfs.pos);
}

int kzgetc ()
{
	char ch;
	if (!kzread(&ch,1)) return(-1);
	return((int)ch);
}

int kzeof ()
{
	if (!kzfs.fil) return(-1);
	return(kzfs.pos >= kzfs.leng);
}

void kzclose ()
{
	if (kzfs.fil) { fclose(kzfs.fil); kzfs.fil = 0; }
}