/* port/a64/pd_a64_api.c —— `kasm87free` 与 `kasm87jumpback` 的 arm64 版。
 *
 * 原文那两个读的是 `f-FUNCBYTEOFFS` 那三个头字（jumpbacknum / kasm87leng / gstatmem）。
 * 那张头只有 COMPILE!=0 那条路才会写；COMPILE==0 时 `FUNCBYTEOFFS` 是 0，于是它们
 * 把**指令**当头字读 —— `kasm87free` 会 `free()` 一个不是 malloc 来的地址，当场
 * `abort()`（真踩过：`./eval_a64 "1+2*3"` 值算对了，收尾 Abort trap: 6）。
 *
 * 所以这一份接管这两个名字：缝合文件在包含 `kasm_comp.c` 之前把原文那两个改名成
 * `kasm87free_x86` / `kasm87jumpback_x86`（那两份在 arm64 上就是死代码），
 * 真正对外的 `kasm87free` / `kasm87jumpback` 是下面这两个。
 * 原文一个字节都没动 —— 改名是缝合文件里的 `#define`，不在原文里。
 */
#if (COMPILE == 0)

void kasm87free (void *f)
{
	long statmem;
	void *kcd;

	if (!f) return;
	if (!pd_a64_owns(f)) return;          /* 不是我们造的就不碰（原文那条路不会走到这儿） */

	statmem = *(long *)&((char *)f)[64];
	kcd     = *(void **)&((char *)f)[56];
	if (statmem) free((void *)statmem);   /* 原文是从头字 +8 拿的 */
	if (kcd) free(kcd);
	pd_a64_release(f);
}

	/* mode=0: 把向后跳清成 0；mode=1: 还原。
	   解释器那条路压根不生成跳转指令（跳转是 gasm[] 上的下标），所以这儿什么都不用做。
	   polydraw.c:511/545 会成对调它 —— 它是"脚本跑太久就掐断"的机关，
	   在解释器上等价于不管。 */
void kasm87jumpback (void *f, long mode)
{
	(void)f; (void)mode;
}

#endif
