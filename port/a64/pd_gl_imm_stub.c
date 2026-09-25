/* port/a64/pd_gl_imm_stub.c —— 立即模式攒批的空壳。
 *
 * `eval_a64` 与 `eval_bench` 那两份是**纯 eval**（一个 GL 都不连），而 JIT 里那三句
 * 钩子是无条件发的。所以这两份链上这个空壳：三格都不做事，行为与 `PD_IMM=0` 相同。
 * （不用 `__attribute__((weak))` —— 那种"链上谁就是谁"的把戏出了错很难查。） */

void *pd_imm_hook (void *fn) { (void)fn; return(0); }
void *pd_imm_call (void *fn) { return(fn); }
int pd_imm_isgl (const char *nm) { (void)nm; return(0); }
void pd_imm_break (void) { }
void pd_imm_flush (void) { }
void pd_imm_frame_end (void) { }
