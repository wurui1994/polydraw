/* bench_ed — clean single-frame timing for evaldraw .kc per-pixel scripts.
 * Links ed_runlib, times only ed_run_frame (excludes PNG write + startup).
 * Usage: bench_ed file.kc W H [frames] */
#include "eval_impl/ed_runlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
static double now_sec(void){static mach_timebase_info_data_t tb;if(!tb.denom)mach_timebase_info(&tb);
    return (double)mach_absolute_time()*tb.numer/tb.denom/1e9;}
#else
static double now_sec(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec+ts.tv_nsec/1e9;}
#endif

static char *read_file(const char*p){FILE*f=fopen(p,"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);char*b=malloc(sz+1);
    size_t rd=fread(b,1,sz,f);fclose(f);b[rd]=0;return b;}

int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: bench_ed file.kc W H [frames]\n");return 1;}
    const char*script=argv[1];int W=atoi(argv[2]),H=atoi(argv[3]);
    int frames=argc>4?atoi(argv[4]):1;
    char*src=read_file(script);if(!src){fprintf(stderr,"cannot read\n");return 2;}
    ed_Ctx*ctx=ed_compile(src,W,H);if(!ctx){free(src);return 1;}free(src);
    ed_set_clock_scale(ctx,1.0/60.0);
    fprintf(stderr,"nInstr=%zu nLocals=%zu nParams=%zu nGlobals=%zu\n",
        ctx->prog.nInstr,ctx->prog.nLocals,ctx->prog.nParams,ctx->prog.nGlobals);
    if (getenv("DUMP")) pd_dump_program(&ctx->prog, stderr);
    /* warmup 1 frame */
    ed_run_frame(ctx,0);
    double t0=now_sec();
    for(int f=0;f<frames;f++) ed_run_frame(ctx,(double)f);
    double dt=now_sec()-t0;
    printf("res=%dx%d frames=%d total=%.3fms per_frame=%.3fms Mpix_per_s=%.2f\n",
        W,H,frames,dt*1e3,dt*1e3/frames,
        (double)W*H*frames/(dt*1e6));
    ed_free(ctx);
    return 0;
}
