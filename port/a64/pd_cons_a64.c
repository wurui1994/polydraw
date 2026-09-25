/* port/a64/pd_cons_a64.c —— polydraw 的控制台转到 stderr。
 *
 * 原文的 `kputs`（`pd/pd_cons.c:4`）往编辑器那个控制台窗口写；我们的窗口是空壳，
 * 于是 polydraw 的全部诊断（着色器编译错误、脚本编译错误、`compile frag#0`
 * 这种进度）一个字都看不见 —— 查问题时等于闭着眼睛。
 *
 * 缝合文件把原文那一份改名成 `kputs_win32`（在 arm64 上是死代码），真名归这儿。
 */
static void kputs (const char *st, int addcr)
{
	if (!st) return;
	fprintf(stderr,"[pd] %s%s",st,addcr ? "\n" : "");
}
