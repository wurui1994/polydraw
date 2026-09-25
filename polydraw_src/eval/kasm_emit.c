
static void put1byte (long a) { if (putwrite) compcode[kasm87leng] = (char)a; kasm87leng++; }
static void put2byte (long a) { if (putwrite) *(short *)&compcode[kasm87leng] = (short)a; kasm87leng += 2; }
static void put4byte (long a) { if (putwrite) *(long *)&compcode[kasm87leng] = a; kasm87leng += 4; }

static void putsib (long opcode, long a, rtyp b)
{
	long hbr, lbr;

	if ((b.r&0xf0000000) == KPTR)
	{
		b.r &= 0x0fffffff;
		if ((b.r >= -128) && (b.r < 128)) put4byte((b.r<<24)+0x244c8b); //mov ecx, [esp+imm8]
		else { put2byte(0x8c8b); put1byte(0x24); put4byte(b.r); }   //mov ecx, [esp+imm32]
		b.r = KECX+b.q*8;
	}
	else if ((b.r&0xf0000000) == KEDX) b.r += b.q*8;

	if (!(opcode&0xffffff00)) put1byte(opcode);
								else put2byte(opcode);

	hbr = (((unsigned long)b.r)>>28); lbr = (b.r&0x0fffffff);
	if (hbr == (KFST>>28))
	{
			//d9 c0   fld   st(0)
			//d9 c8   fxch  st(0)
			//dd c0   ffree st(0)
			//dd d0   fst   st(0)
			//dd d8   fstp  st(0)
			// ...
		if (putwrite)
		{
			if ((compcode[kasm87leng-1] != 0xdd) || ((a&0x30) != 0x10)) //Don't do hack for FST or FSTP...
				compcode[kasm87leng-1] -= 0x04; //Nasty hack!!!
		}
		fpustat |= (1<<(lbr>>3));
		put1byte(a+0xc0+(lbr>>3)); return;
	}
	a &= 0x38; //Throw away stack pointer position when using memory access!
	if ((hbr == (KIMM>>28)) || (hbr == (KGLB>>28)))
	{
		put1byte(a+0x05);
#if (COMPILE == 0)
		if (hbr == (KIMM>>28)) put4byte((long)(gevalext[lbr].ptr)+b.q*8); //access static variable (imm32)
								else put4byte(gstatmem + lbr+b.q*8); //access static variable (imm32)
#else
		if (putwrite)
		{
			checkpatch(patchnum+1);
			patch[patchnum].lptr = (long *)&compcode[kasm87leng];
			patch[patchnum].ind = b.r;
			patchnum++;
		}
		put4byte(b.q*8); //access static variable (imm32)
#endif
		return;
	}
	if ((lbr < -128) || (lbr >= 128)) { put1byte(a+0x80+hbr); if (hbr == 4) put1byte(0x24); put4byte(lbr); }
	else if ((lbr) && (hbr != 5))     { put1byte(a+0x40+hbr); if (hbr == 4) put1byte(0x24); put1byte(lbr); }
	else                              { put1byte(a     +hbr); if (hbr == 4) put1byte(0x24);                }
}

static long putlen (rtyp b)
{
	long r, lng;

	lng = 0;
	if ((b.r&0xf0000000) == KPTR)
	{
		b.r &= 0x0fffffff;
		if ((b.r >= -128) && (b.r < 128)) lng = 4; else lng = 7;
		b.r = KECX+b.q*8;
	}
	else if ((b.r&0xf0000000) == KEDX) b.r += b.q*8;

	r = (((unsigned long)b.r)>>28); b.r &= 0x0fffffff;
	if (r == (KFST>>28)) return(lng+1);
	if ((r == (KIMM>>28)) || (r == (KGLB>>28))) return(lng+5);
	if ((b.r < -128) || (b.r >= 128)) return(lng+(r==4)+5);
	else if ((b.r) && (r != 5))       return(lng+(r==4)+2);
	else                              return(lng+(r==4)+1);
}

	//This function helps find sequences like this which can be safely removed:
	// fld qword ptr [esp+0x28]
	// fstp qword ptr [esp+0x28]
	// ... (qword ptr [esp+0x28] not used again)
	//
	// r0 = ? op ?;
	//  ? = r0 + ?;
	// (r0 written before read)
static long anyreads1stop, anyreads1stinst;