/* port/a64/eval_bench.c —— 代替 `eval/eval_test.c` 的量尺 main（arm64/osx）。
 *
 * 为什么不用 Ken 自带的那个：它的 `rdtsc64()` 在非 x86 上是
 * `return(LL(0))`（`eval_test.c:35`），所以它印的永远是 `0 cc` —— 量不出东西。
 * 这一份用 `clock_gettime(CLOCK_MONOTONIC)`，按"同一份脚本跑 N 趟取最小"报时间。
 *
 * 判据口径与 Omni 那边一致（memory: 判据不许在慢路上铺帧数）：
 * 一份例子 ≤ 10s，趟数按时间给 —— 先探 1000 趟，再按探到的单趟时间凑够 200ms。
 *
 * 用法：
 *   eval_bench "脚本" [实参...]           印一次值与每趟耗时
 *   eval_bench --compile "脚本"           只量 kasm87() 的编译时间
 */
#include <time.h>

static double pd_now (void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC,&ts);
	return((double)ts.tv_sec + (double)ts.tv_nsec*1e-9);
}

/* 一个**批**里跑 n 趟，整批计一次时；外面重复 r 批取最小的那一批。
 *
 * 为什么不逐趟计时：`clock_gettime` 这台机器上一跳 41.67ns，而一趟简单脚本比这还快
 * —— 逐趟量出来就是 `0.0000 us`（踩过）。批量摊薄之后才有分辨率。
 * 取最小而不是平均：我们量的是"这台机器能多快"，调度噪声只会让某一批变慢。 */
static double pd_timeit (EVALFUNC f, double *v, long nv, long n, long r, double *out)
{
	double best = 1e30, t0, t1, d = 0;
	long i, k;
	for(k=0;k<r;k++)
	{
		t0 = pd_now();
		for(i=0;i<n;i++)
		{
			     if (nv >= 3) d = f(v[0],v[1],v[2]);
			else if (nv == 2) d = f(v[0],v[1]);
			else if (nv == 1) d = f(v[0]);
			else              d = ((double (*)(void))f)();
		}
		t1 = pd_now();
		if ((t1-t0)/(double)n < best) best = (t1-t0)/(double)n;
	}
	*out = d;
	return(best);
}

int main (int argc, char **argv)
{
	double v[256], val, per, t0, t1;
	long i, nv, reps;
	EVALFUNC f;

	if (argc < 2)
	{
		printf("用法: eval_bench \"脚本\" [实参...]\n");
		printf("      eval_bench --compile \"脚本\"\n");
		return(1);
	}

	if (!strcmp(argv[1],"--compile"))
	{
		if (argc < 3) return(1);
		t0 = pd_now();
		for(i=0;i<100;i++)
		{
			f = (EVALFUNC)kasm87(argv[2]);
			if (!f) { printf("%s\n",kasm87err); return(1); }
			kasm87free((void *)f);
		}
		t1 = pd_now();
		printf("compile %.4f ms/趟（100 趟平均）\n",(t1-t0)*1000.0/100.0);
		kasm87freeall();
		return(0);
	}

	memset(v,0,sizeof(v));
	nv = argc-2;
	for(i=0;i<nv;i++) v[i] = atof(argv[i+2]);

	f = (EVALFUNC)kasm87(argv[1]);
	if (!f) { printf("ERROR %s\n",kasm87err); return(1); }

	/* 先探一小批看单趟多快，再按它凑够每批 20ms、共 9 批（≈200ms，远在 10s 预算里）。 */
	per = pd_timeit(f,v,nv,200,3,&val);
	reps = (long)(0.02/(per > 1e-9 ? per : 1e-9));
	if (reps < 200) reps = 200;
	if (reps > 5000000) reps = 5000000;
	per = pd_timeit(f,v,nv,reps,9,&val);

	printf("value: %.17g\n",val);
	printf("per:   %.4f us（每批 %ld 趟 x 9 批取最小）\n",per*1e6,reps);

	kasm87free((void *)f);
	kasm87freeall();
	return(0);
}
