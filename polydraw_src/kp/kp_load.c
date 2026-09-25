

//====================== ZIP decompression code ends =========================
//===================== HANDY PICTURE function begins ========================

void kpzload (const char *filnam, INT_PTR *pic, int *bpl, int *xsiz, int *ysiz)
{
	char *buf;
	int leng;

	(*pic) = 0;
	if (!kzopen(filnam)) return;
	leng = kzfilelength();
	buf = (char *)malloc(leng); if (!buf) { kzclose(); return; }
	kzread(buf,leng);
	kzclose();

	kpgetdim(buf,leng,xsiz,ysiz);
	(*bpl) = ((*xsiz)<<2);
	(*pic) = (INT_PTR)malloc((*ysiz)*(*bpl)); if (!(*pic)) { free(buf); return; }
	if (kprender(buf,leng,*pic,*bpl,*xsiz,*ysiz,0,0) < 0) { free(buf); free((void *)*pic); (*pic) = 0; return; }
	free(buf);
}
//====================== HANDY PICTURE function ends =========================
