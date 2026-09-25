
void ksrand (long val) { kholdrand = val; snormstat = 0; }
#ifndef _MSC_VER
static long krand () { kholdrand = (unsigned long)((kholdrand*(214013*2)+2531011*2)>>1); return(kholdrand); }
#else
__declspec(naked) static long krand ()
{
	_asm
	{
		mov eax, kholdrand
		imul eax, 214013*2
		add eax, 2531011*2
		shr eax, 1
		mov kholdrand, eax
		ret
	}
}
#endif

static double nrnd ()
{
	static double srand2;
	double x, y, r;

		//Box-Muller method (Good & fast)
	if (snormstat) { snormstat = 0; return(srand2); }
	do
	{
		x = ((double)(krand()-1073741824))*(oneover2_31*2.0); //-1 to 1
		y = ((double)(krand()-1073741824))*(oneover2_31*2.0); //-1 to 1
		r = x*x + y*y;
	} while (r >= 1);
	snormstat = 1; r = sqrt(-2.0*log(r)/r); srand2 = x*r;
	return(y*r);
}

#ifndef _MSC_VER
static double fact (double num)
{
	if ((num <= -.99999999999999996) || (num >= 170.6243769562767)) return(*(float *)&pinf);
	num++; //2^, 14*, 1/, 15+  (Ken optimized out most divides - wasn`t easy!)
	return(pow(num+5.5,num+0.5)*exp(-5.5-num)*
		(((((((num*2.506628275107298 + 83.8676043423952)*num + 1168.926494792211)*num +
		 8687.245297053594)*num + 36308.29514770109)*num + 80916.62789524846)*num + 75122.63315304522) /
		 (((((((num + 21)*num + 175)*num + 735)*num + 1624)*num + 1764)*num + 720)*num)));
}
#else
__declspec(naked) static double __cdecl fact (double num)
{
	static const double maxval = 170.6243769562767;
	static const double factconsts[15] = {2.506628275107298,83.8676043423952,1168.926494792211,8687.245297053594,
		36308.29514770109,80916.62789524846,75122.63315304522,21,175,735,1624,1764,720,5.5,0.5};

	//st(0) >  src: if !(ah&0x45)
	//st(0) >= src: if (!(ah&0x45)) || (ah&0x40)
	//st(0) == src: if (ah&0x40)
	//st(0) <= src: if (ah&0x41)
	//st(0) <  src: if (ah&0x01)
	//unordered     if (ah&0x04)

	_asm //WARNING: CAN MODIFY ONLY EAX!
	{
		fld qword ptr [esp+4]
		fcom dword ptr [negone]
		fnstsw ax
		and ah, 0x41
		jnz short factinf  ;<= -1
		;fld dword ptr [negone]
		;fcomip st, st(1) ;Requires >=PPRO
		;jae short factinf

		fcom qword ptr [maxval]
		fnstsw ax
		and ah, 0x45
		jz short factinf   ;> maxval
		;fld qword ptr [maxval]
		;fcomip st, st(1) ;Requires >=PPRO
		;jb short factinf

		fadd dword ptr [posone]
		fld st(0)
		fld st(0)
		fld st(0)

		mov eax, offset factconsts

		fmul qword ptr [eax]    ;a *= konst[0]
		fadd qword ptr [eax+8]  ;a += konst[8]
		fmul st, st(2)          ;a *= num
		fadd qword ptr [eax+16] ;a += konst[16]
		fmul st, st(2)          ;a *= num
		fadd qword ptr [eax+24] ;a += konst[24]
		fmul st, st(2)          ;a *= num
		fadd qword ptr [eax+32] ;a += konst[32]
		fmul st, st(2)          ;a *= num
		fadd qword ptr [eax+40] ;a += konst[40]
		fmul st, st(2)          ;a *= num
		fadd qword ptr [eax+48] ;a += konst[48]
		fxch st(1)

		fadd qword ptr [eax+56] ;b += konst[56]
		fmul st, st(2)          ;b *= num
		fadd qword ptr [eax+64] ;b += konst[64]
		fmul st, st(2)          ;b *= num
		fadd qword ptr [eax+72] ;b += konst[72]
		fmul st, st(2)          ;b *= num
		fadd qword ptr [eax+80] ;b += konst[80]
		fmul st, st(2)          ;b *= num
		fadd qword ptr [eax+88] ;b += konst[88]
		fmul st, st(2)          ;b *= num
		fadd qword ptr [eax+96] ;b += konst[96]
		fmul st, st(2)          ;b *= num
		fdivp st(1), st         ;a /= b

		fxch st(2)

		fadd qword ptr [eax+104] ;c = num+konst[104] ;c ?
		fxch st(1)                                   ;? c
		fadd qword ptr [eax+112] ;d = num+konst[112] ;d c
		fld st(1)                                    ;c d c

			;c = 2^(log2(c)*d - c*l2e)
		fyl2x ;(st1 *= log2(st0), pop st)            ;log2(c)*d c
		fxch st(1)                                   ;c log2(c)*d
		fldl2e
		fmulp st(1), st
		fsubp st(1), st
#if 0
			;Multi-thread unsafe exp (faster than safe)
		mov eax, offset kexptval[8]
		fist dword ptr [eax]
		fisub dword ptr [eax]
		add dword ptr [eax], 0x3fff
		f2xm1
		fadd dword ptr [posone]
		fld tbyte ptr [eax-8]
		fmulp st(1), st(0)
#else
			;Multi-thread safe exp: (~80cc slower than unsafe)
		sub esp, 8
		fist dword ptr [esp]
		fisub dword ptr [esp]
		add dword ptr [esp], 0x3fff
		f2xm1
		fadd dword ptr [posone]
		push 0x80000000
		push 0
		fld tbyte ptr [esp]
		fmulp st(1), st(0)
		add esp, 16
#endif
		fmulp st(1), st                               ;a *= c
		ret

factinf:
		fstp st(0)
		fld dword ptr [pinf]
		ret
	}
}
#endif

	//Ken`s replacement for pow...
#ifndef _MSC_VER
static double kpow(double x, double y) { return pow(x,y); }
#else
__declspec(naked) static double __cdecl kpow (double x, double y)
{
	_asm //WARNING: CAN MODIFY ONLY EAX!
	{
		cmp dword ptr [esp+8], 0 ;if (x == 0) (1st half of test)
		je short dozer
backz:fld qword ptr [esp+12]
		fld qword ptr [esp+4]
		fabs
		fyl2x ;(st1 *= log2(st0), pop st)
		fist dword ptr [esp+4]
		fisub dword ptr [esp+4]
		mov eax, [esp+4]
		lea eax, [eax+16383]
#if 0
			;Multi-thread unsafe exp (faster than safe)
		mov dword ptr kexptval[8], eax
		f2xm1
		fadd dword ptr [posone]
		fld tbyte ptr kexptval[0]
#else
			;Multi-thread safe exp: (~80cc slower than unsafe)
		lea esp, [esp-8]
		mov dword ptr [esp], eax
		f2xm1
		fadd dword ptr [posone]
		push 0x80000000
		push 0
		fld tbyte ptr [esp]
		lea esp, [esp+16]
#endif
		fmulp st(1), st(0)
		jl short doneg          ;if (x < 0)
		ret

doneg:fld qword ptr [esp+12]  ;handle pow(-,*) cases
		fistp qword ptr [esp+4]
		fild qword ptr [esp+4]
		fcomp qword ptr [esp+12]
		fnstsw ax
		and ah, 0x40
		jz short bad1 ;power is not an integer

		test dword ptr [esp+4], 1
		jz short endit
		fchs
endit:ret
bad1: fstp st(0)
		fld dword ptr [nind]
		ret

dozer:cmp dword ptr [esp+4], 0 ;if (x == 0) (2nd half of test)
		jne short backz          ;oops! x wasn`t actually 0!
		fldz                     ;handle pow(0,*) cases
		fcomp qword ptr [esp+12]
		fnstsw ax
		test ax, 0x0100
		jnz short bad2
		test ax, 0x4000
		jz short skp2
		fld1
		ret
skp2: fld dword ptr [pinf]
		ret
bad2: fldz
		ret
	}
}
#endif
