/* port/a64/kasm_main_a64.c —— `eval/kasm_main.c` 的 arm64 替身。
 *
 * 与原文的差别**只有一处**：`#if (COMPILE != 0)` 那条分支的收尾。
 * `diff -u polydraw_src/eval/kasm_main.c port/a64/kasm_main_a64.c` 看得见全部改动。
 * 原文一个字节都没动 —— 这一份是新材料，只有 arm64 的缝合文件包含它。
 */


void *kasm87 (char *bakz)
{
	long i, j, k, l, z, oi, got, pcnt, bcnt, scnt, whitespc, funcnt, inquotes, funptr, *lptr;
	long codebytes, databytes, ogevalextnum, globenumcharplc, globenumnum, globnewvarplc, globnewvarnum;
	evalextyp *ogevalext;
	char ch, *tbuf, *tbufmal, *cptr;
	void *v;

	if (!cpuinited)
	{
		cpuinited = 1; cputype = getcputype();
		for(i=0;i<2048;i++)
		{
				  if (i < 1044) round0msk[i][0] = 0;
			else if (i >=1075) round0msk[i][0] = -1;
			else               round0msk[i][0] = -(1<<(1075-i));

				  if (i < 1023) round0msk[i][1] = 0;
			else if (i >=1043) round0msk[i][1] = -1;
			else               round0msk[i][1] = (-(1<<(1043-i)))|0xfff00000;
		}
	}

	kasm87err0 = -1; kasm87err1 = -1;

	i = strlen(bakz)+1+2;
	texttransn = (((i+31)>>5)<<2); //# bytes for texttrans bit buf, aligned to 32-bits
	if (texttransn > texttransmal)
	{
		if (texttrans) free((void *)texttrans);
		texttransmal = max(max(texttransn,1024),texttransmal<<1);
		texttrans = (long *)malloc(texttransmal);
	}
	if (!texttrans) { strcpy(kasm87err,"ERROR: malloc failed"); return(0); }
	memset(texttrans,0,texttransn);

	tbufmal = (char *)malloc(i); //2 more to store "()" if necessary
	if (!tbufmal) { strcpy(kasm87err,"ERROR: malloc failed"); return(0); }
	tbufmal[0] = '('; tbufmal[1] = ')'; tbuf = tbufmal+2;

		//Compact white space, remove comments, handle quotes & char constants
	l = 0; got = 0; whitespc = 0; inquotes = 0;
	for(i=0;bakz[i];i++)
	{
		if (!inquotes)
		{
			if ((bakz[i] == 32) || (bakz[i] == 9)) { whitespc = 1; continue; } //strip Space/Tab
			if ((bakz[i] == 10) || (bakz[i] == 13)) { whitespc = 1; if (got == 1) got = 0; continue; } //strip CR,LF
			if ((bakz[i] == '/') && (bakz[i+1] == '/') && (!got)) got = 1;
			if ((bakz[i] == '/') && (bakz[i+1] == '*') && (!got)) { whitespc = 1; got = 2; }
			if ((bakz[i] == '*') && (bakz[i+1] == '/') && (got == 2)) { got = 0; i++; continue; }
			if (got) continue;

			if (whitespc)
			{
				tbuf[l++] = ' '; whitespc = 0;
				texttrans[(i-1)>>5] |= (1<<(i-1)); //WARNING:Uses 32-bit x86 shift trick
			}

				//Skip "#opt(..)"
			if ((bakz[i] == '#') && (bakz[i+1] == 'o') && (bakz[i+2] == 'p') && (bakz[i+3] == 't') && (bakz[i+4] == '('))
				{ for(i+=5;(bakz[i]) && (bakz[i] != ')');i++); continue; }

			ch = bakz[i]; if ((ch >= 'a') && (ch <= 'z')) ch -= 32;
		} else ch = bakz[i];

		tbuf[l++] = ch;
		texttrans[i>>5] |= (1<<i); //WARNING:Uses 32-bit x86 shift trick

		if ((ch == '\"') && ((l < 2) || (tbuf[l-2] != '\\'))) inquotes ^= 1;
		if (inquotes) continue;

		if (ch == '\'') //Convert character numbers: ' ', '$', 'a', etc...
		{
			if ((bakz[i+1] < 32) || (bakz[i+2] != '\'')) { sprintf(kasm87err,"ERROR: ' used wrong"); kasm87err0 = i; kasm87err1 = i+2; free(tbufmal); return(0); }
			j = sprintf(&tbuf[l-1],"%d",bakz[i+1]);
			switch (j)
			{
				case 3: texttrans[(i+2)>>5] |= (1<<(i+2)); //no break intentinoal //WARNING:Uses 32-bit x86 shift trick
				case 2: texttrans[(i+1)>>5] |= (1<<(i+1)); //no break intentinoal //WARNING:Uses 32-bit x86 shift trick
				case 1: break;
			}
			l += j-1; i += 2; continue;
		}
	}
	tbuf[l] = 0;

		//Split function definitions & global sections. Algo:
		// * find first '(' not inside "", {}, []
		// * extract function name by searching backwards like this:  !isvarchar  whitespc  name  whitespc  (
		// * if char before name isn't ';', '}' or start of buffer, then:
		//         Whole script is treated as single function; add "()" at beginning.
		//      Note: This case only valid when finding the first function. If detected later, report an error.
		// * else if name is blank, then it becomes the main function: everything before it is treated as global.
	pcnt = 0; scnt = 0; bcnt = 0; funcnt = 0; inquotes = 0; oi = 0; got = 0;
	globi = 0; if (!maxfuncst) { checkfuncst(0); if (globi < 0) { free(tbufmal); return(0); } }
	for(i=0;i<l;i++)
	{
		ch = tbuf[i];
		if ((ch == '\"') && (!((i) && (tbuf[i-1] == '\\')))) inquotes ^= 1;
		if (inquotes) continue;
		if (ch == '{') { bcnt++; continue; }
		if (ch == '}') { bcnt--; if (bcnt < 0) { strcpy(kasm87err,"ERROR: too many }"); /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }
										 if (pcnt    ) { strcpy(kasm87err,"ERROR: missing )");  /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }
							  continue; }
		if (ch == '[') { scnt++; continue; }
		if (ch == ']') { scnt--; if (scnt < 0) { strcpy(kasm87err,"ERROR: too many ]"); /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }
							  continue; }
		if ((ch == '(') && (!bcnt) && (!scnt)) //Found first '(' not inside "", {}, []
		{
			checkfuncst((funcnt+2)*4); if (globi < 0) { /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }

				//check if it's a function. Extract function name by searching backwards like this:  !isvarchar  whitespc  name  whitespc  (
			k = i;
			if ((k > oi) && (tbuf[k-1] == ' ')) k--; //skip whitespc
			for(j=k;(k) && (isvarchar(tbuf[k-1]));k--); //find string: &tbuf[k<=?<j]

				//insert global section
			if (oi < k-1) { funcst[funcnt*4+0] = -1; funcst[funcnt*4+1] = 0; funcst[funcnt*4+2] = oi; funcst[funcnt*4+3] = k; funcnt++; }

			if (j == k) { funcst[funcnt*4+0] = i; } //no name; ltrim space
					 else { funcst[funcnt*4+0] = k; if (!got) break;/*Hack for simple cases like "cos(3)+4"*/ }
			funcst[funcnt*4+1] = i;

			if ((k > oi) && (tbuf[k-1] == ' ')) k--; //m = char before string (skipping whitespc)
			if ((k > oi) && (tbuf[k-1] != ';') && (tbuf[k-1] != '}'))
			{
				if (got) { strcpy(kasm87err,"ERROR: global definition missing ; or }"); /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }
				break; //Simple script
			}

			got = 1;

				//Find ')' (end of function parameter list)
			for(pcnt=1,i++;i<l;i++)
			{
				if (tbuf[i] == '(') { pcnt++; continue; }
				if (tbuf[i] == ')') { pcnt--; if (!pcnt) { funcst[funcnt*4+2] = i+1; break; } }
			}
			if (i >= l) { strcpy(kasm87err,"ERROR: function definition missing )"); /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }

				//Find '}' (end of function body)
			i++; if (tbuf[i] == 32) i++;
			if (tbuf[i] != '{') { funcst[funcnt*4+3] = l; funcnt++; oi = l; break; } //Hack to support a single function without {} around body
			for(bcnt=1,i++;i<l;i++)
			{
				if (tbuf[i] == '{') { bcnt++; continue; }
				if (tbuf[i] == '}') { bcnt--; if (!bcnt) { funcst[funcnt*4+3] = i+1; oi = i+1; funcnt++; break; } }
			}
			if (i >= l) { strcpy(kasm87err,"ERROR: function missing } at end"); /*setecurs(k,i);*/ free(tbufmal); return(0); }
		}
	}
		//Simple script
	if (!got) { tbuf -= 2; l += 2; funcst[0*4+0] = 0; funcst[0*4+1] = 0; funcst[0*4+2] = 2; funcst[0*4+3] = l; funcnt = 1; }
	else
	{
			//insert global section
		if (oi < l-1) { funcst[funcnt*4+0] = -1; funcst[funcnt*4+1] = 0; funcst[funcnt*4+2] = oi; funcst[funcnt*4+3] = l; funcnt++; }
	}

	ogevalext = gevalext; ogevalextnum = gevalextnum;
	gevalext = (evalextyp *)malloc((gevalextnum+funcnt)*sizeof(evalextyp));
	if (!gevalext) { strcpy(kasm87err,"ERROR: malloc failed"); free(tbufmal); return(0); }

#if (COMPILE != 0)
	patchnum = 0;
#endif

#if 0
		//funcst[?*4+{0...1....2......3}]
		//           |func(x,y){x^2+y}|
		//           |func(x,y)|
		//               |(x,y){x^2+y}|
	for(i=0;i<funcnt;i++)
	{
		if (funcst[i*4+0] < 0)
		{
			ch = tbuf[funcst[i*4+3]]; tbuf[funcst[i*4+3]] = 0; printf("FUNC%d:glob parse sees:|%s|\n",i,&tbuf[funcst[i*4+2]]); tbuf[funcst[i*4+3]] = ch;
		}
		else
		{
			ch = tbuf[funcst[i*4+2]]; tbuf[funcst[i*4+2]] = 0; printf("FUNC%d: gevalext proto:|%s|\n",i,&tbuf[funcst[i*4+0]]); tbuf[funcst[i*4+2]] = ch;
			ch = tbuf[funcst[i*4+3]]; tbuf[funcst[i*4+3]] = 0; printf("FUNC%d:kasm87comp sees:|%s|\n",i,&tbuf[funcst[i*4+1]]); tbuf[funcst[i*4+3]] = ch;
		}
	}
#endif


	globi = 0; arrnum = 0;
	if (!maxops)
	{
		if (oprio[0] != 255) //Initialize operator precedence LUT
		{
			memset(oprio,255,sizeof(oprio));
			oprio[POW] = 0;
			oprio[TIMES] = oprio[SLASH] = oprio[PERC] = 1;
			oprio[PLUS] = oprio[MINUS] = 2;
			oprio[LES] = oprio[LESEQ] = oprio[MOR] = oprio[MOREQ] = 3;
			oprio[EQU] = oprio[NEQU] = 4;
			oprio[LAND] = 5;
			oprio[LOR] = 6;
		}

		checkops(0); checkstrings(0); checkinitvals(0); checkrxi(0);
		checkenum(0); checkenumchars(0);
		checkvars(0); checkvarchars(0);
		checklabs(0); checklabchars(0);
		checkjumpbacks(0);
#if (COMPILE != 0)
		checkpatch(0);
#endif
		if (globi < 0)
		{
			free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum; free(tbufmal);
			strcpy(kasm87err,"ERROR: malloc failed"); return(0);
		}
	}

		//Init for global parse_static() -> parse_dimensions() -> kasmoptimizations()
	gecnt = 0; gccnt = 0; gstnum = 0; ginitvalnum = 0;

		//Init for parse_static()
	memset(newvarhash,-1,sizeof(newvarhash));
	newvarnam[0] = 0; newvarplc = 0; newvarnum = 0;

		//parse global enum&static declarations
	enumcharplc = 0; enumnum = 0;
	for(i=j=0;i<funcnt;i++)
	{
		if (funcst[i*4+0] < 0)
		{
			ch = tbuf[funcst[i*4+3]]; tbuf[funcst[i*4+3]] = 0;
			for(z=funcst[i*4+2];tbuf[z];z++)
			{
					  if ((!strncmp(&tbuf[z],"ENUM"  ,4)) && (!isvarchar(tbuf[z+4]))) z = parse_enum(tbuf,z);
				else if ((!strncmp(&tbuf[z],"STATIC",6)) && (!isvarchar(tbuf[z+6]))) z = parse_static(tbuf,z);
				else continue;
				if (globi < 0) { free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum; free(tbufmal); return(0); }
				if (!tbuf[z]) break;
			}
			tbuf[funcst[i*4+3]] = ch;
		}
		else
		{
			funcst[j*4+0] = funcst[i*4+0];
			funcst[j*4+1] = funcst[i*4+1];
			funcst[j*4+2] = funcst[i*4+2];
			funcst[j*4+3] = funcst[i*4+3];
			j++;
		}
	}
	funcnt = j;
	globenumnum = enumnum; globenumcharplc = enumcharplc;
	memcpy(newvarhash_glob,newvarhash,sizeof(newvarhash));
	globnewvarplc = newvarplc; globnewvarnum = newvarnum;
	if (arrnum) //allocate gstatmem block & copy ginitval to it before kasm87comp destroys ginitval
	{
		gstatmem = (long)malloc(arrnum);
		if (!gstatmem)
		{
			free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum; free(tbufmal);
			strcpy(kasm87err,"ERROR: malloc failed"); return(0);
		}

			//Fill gstatmem with assigned values or 0's
		k = (long)gstatmem;
		for(i=j=0;i<arrnum;i+=8,k+=8)
		{
			if ((j < ginitvalnum) && (i == ginitval[j].i))
				{ *(double *)k = ginitval[j].v; j++; }
			else *(double *)k = 0;
		}
	} else gstatmem = 0;
	for(i=0;i<globnewvarnum;i++)
		if ((newvar[i].r&0xf0000000) == KARR)
			newvar[i].r = (newvar[i].r&0x0fffffff)+KGLB;


	jumpbacknum = 0;
	memcpy(gevalext,ogevalext,ogevalextnum*sizeof(evalextyp));
	gevalextnum += funcnt-1;
		//Main can't be called since it doesn't have a name
	for(i=funcnt-1;i>0;i--)
	{
		j = gevalextnum-i;

		ch = tbuf[funcst[i*4+2]]; tbuf[funcst[i*4+2]] = 0;
		gevalext[j].nam = strdup(&tbuf[funcst[i*4+0]]);
		if (!gevalext[j].nam)
		{
			for(j--;j>=ogevalextnum;j--) free(gevalext[j].nam);
			if (gstatmem) { free((void *)gstatmem); gstatmem = 0; }
			free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum;
			free(tbufmal); strcpy(kasm87err,"ERROR: strdup failed"); return(0);
		}
		//printf("ext_proto:|%s|\n",gevalext[j].nam);
		tbuf[funcst[i*4+2]] = ch;
	}

	codebytes = databytes = 0;
	for(i=funcnt-1;i>=0;i--)
	{
		j = gevalextnum-i;

		enumnum = globenumnum; enumcharplc = globenumcharplc;
		memcpy(newvarhash,newvarhash_glob,sizeof(newvarhash));
		newvarplc = globnewvarplc; newvarnum = globnewvarnum;

		tbuf[funcst[i*4+3]] = 0;
		//printf("compiling:|%s|\n",&tbuf[funcst[i*4+1]]);
		gevalext[j].ptr = (EVALFUNC)kasm87comp(&tbuf[funcst[i*4+1]]);

#if 0
		{ //DEBUG ONLY!
			char debuf[65536];
			extern char *kdisasm (char *, long, char *, long, long *);
			kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n\n%d:\n%s",i,debuf);
			debuf[0] = debuf[sizeof(debuf)-1] = 0;
			l = 0; kdisasm((char *)compcode,kasm87leng,debuf,sizeof(debuf)-1,&l);
			printf("\n%s",debuf);
		}
#endif

		if (!gevalext[j].ptr)
		{
			for(j--;j>=ogevalextnum;j--) free((void *)(((long)gevalext[j].ptr)-FUNCBYTEOFFS));
			for(j=gevalextnum-1;j>=ogevalextnum;j--) free(gevalext[j].nam);
			if (gstatmem) { free((void *)gstatmem); gstatmem = 0; }
			free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum;
			free(tbufmal); return(0);
		}

		funptr = (long)gevalext[j].ptr; lptr = (long *)(funptr-FUNCBYTEOFFS);
		codebytes += lptr[1];
		databytes += lptr[3];
	}
	v = (void *)gevalext[gevalextnum].ptr;

#if (COMPILE != 0)

		//merge all functions together
	kasm87leng = codebytes + databytes; if (databytes) kasm87leng += CODEDATADIST;
	v = malloc(FUNCBYTEOFFS + kasm87leng + jumpbacknum*sizeof(jumpback_t));
	if (!v)
	{
		for(i=0;i<funcnt;i++) { j = gevalextnum-i; free((void *)(((long)gevalext[j].ptr)-FUNCBYTEOFFS)); }
		for(j=gevalextnum-1;j>=ogevalextnum;j--) free(gevalext[j].nam);
		if (gstatmem) { free((void *)gstatmem); gstatmem = 0; }
		free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum;
		free(tbufmal); return(0);
	}
	v = (void *)(((long)v)+FUNCBYTEOFFS);
	cptr = (char *)v; l = patchnum-1; k = jumpbacknum-1;
	for(i=0;i<funcnt;i++)
	{
		j = gevalextnum-i; funptr = (long)gevalext[j].ptr; lptr = (long *)(funptr-FUNCBYTEOFFS); if (!lptr[1]) continue;

			//Relocate code
		memcpy(cptr,(void *)funptr,lptr[1]);
		lptr[0] = ((long)cptr); //lptr[0] now holds code pointer so data pointer can be plugged in later

			//Relocate patch pointers
		while ((l >= 0) && (((unsigned long)patch[l].lptr)-((unsigned long)funptr) < (unsigned long)lptr[1]))
			{ patch[l].lptr = (long *)(((long)patch[l].lptr) + ((long)cptr) - funptr); l--; }

			//Relocate jumpback list
		while ((k >= 0) && (((unsigned long)jumpback[k].addr)-((unsigned long)funptr) < (unsigned long)lptr[1]))
			{ jumpback[k].addr += ((long)cptr) - funptr; k--; }

		cptr += lptr[1];
	}
	if (databytes) { memset(cptr,0x90,CODEDATADIST); cptr += CODEDATADIST; }
	for(i=0;i<funcnt;i++)
	{
		j = gevalextnum-i; funptr = (long)gevalext[j].ptr; lptr = (long *)(funptr-FUNCBYTEOFFS); if (!lptr[3]) continue;

			//Relocate data
		memcpy(cptr,(void *)(funptr+lptr[2]),lptr[3]);

			//Relocate consts&array (KEDX)
		if (*(unsigned char *)(lptr[0]) == 0xba) *(long *)(lptr[0]+1) = ((long)cptr); //Note: condition should always be true

		cptr += lptr[3];
	}

	for(i=0;i<funcnt;i++)
	{
		j = gevalextnum-i; funptr = (long)gevalext[j].ptr; lptr = (long *)(funptr-FUNCBYTEOFFS);
		gevalext[j].ptr = (long *)lptr[0]; //Put the new function pointer on global list
		free((void *)lptr);                //..and free the original function which was just copied
	}

	for(j=patchnum-1;j>=0;j--)
	{
		if ((patch[j].ind&0xf0000000) == KIMM) patch[j].lptr[0] += ((long)gevalext[patch[j].ind&0x0fffffff].ptr);
													 else patch[j].lptr[0] += gstatmem + (patch[j].ind&0x0fffffff);
	}

	for(j=jumpbacknum-1;j>=0;j--) jumpback[j].val = *(long *)(jumpback[j].addr); //backup original jumpback values
	compcode = (unsigned char *)v; //make sure kasm87_showdebug has a valid pointer

		//Copy jumpback table to script's malloced array so kasm87jumpback() can support multiple scripts
	*(long *)(((long)v)-FUNCBYTEOFFS) = jumpbacknum;
	*(long *)(((long)v)-FUNCBYTEOFFS+4) = kasm87leng;
	*(long *)(((long)v)-FUNCBYTEOFFS+8) = gstatmem;
	memcpy((void *)(((long)v)+kasm87leng),jumpback,jumpbacknum*sizeof(jumpback_t));
#else

		//—— 这里是本机（arm64/osx）补上的那一格 ——
		//原文这条分支只有一句 `??? not implemented .. need to fix .. sorry :/`：
		//COMPILE==0 时 kasm87comp 回来的不是一块机器码，而是 kasm87c/kasm87cp 的地址
		//（kasm_interp.c 末尾那个"一次只跑一份脚本"的 hack：它把 kcd 放进全局
		//gkasm87cptr，然后把解释器入口当函数指针交回来）。所以：
		//  * 没有码块要合并 —— v 已经是可调用的东西；
		//  * **绝不能**写 v-FUNCBYTEOFFS 那三个头字：FUNCBYTEOFFS 是 0，v 指着
		//    kasm87c 的代码段，写下去直接 SIGBUS。上面那四句因此挪进了 #if 里。
		//v 是 port/a64/pd_a64_jit.c 造的那一格 thunk（见那份文件的头注）。gstatmem 原文
		//是写进头字 +8 的，这儿记进 thunk 的账里 —— kasm87free 要靠它把那块也放掉。
	pd_a64_set_statmem(v,gstatmem);

		//再把每一格 kcd 里那份 gevalext 抄本按现在的 gevalext[] 刷一遍 —— 递归与向后
		//引用要靠它（抄的时候自己那一格的 .ptr 还是空的）。x86 那条路是上面 patch[]
		//那个循环干的同一件事，解释器没有码字要补，要补的是那份抄本。
	for(i=0;i<funcnt;i++) pd_a64_refresh_ext((void *)gevalext[gevalextnum-i].ptr,gevalext,gevalextnum);
	kasm87leng = 0;
#endif

	for(j=gevalextnum-1;j>=ogevalextnum;j--) free(gevalext[j].nam);
	free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum;
	free(tbufmal);

#if _WIN32
	VirtualProtect((void *)(((long)v)-FUNCBYTEOFFS),kasm87leng+FUNCBYTEOFFS,0x40/*PAGE_EXECUTE_READWRITE*/,(unsigned long *)&i);
	//FlushInstructionCache(GetCurrentProcess(),((long)v)-FUNCBYTEOFFS,kasm87leng+FUNCBYTEOFFS);
#endif

	return(v);
}