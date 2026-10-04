# 移植的 25 个洞：清单与找法

> "洞" = **原文自己写着**的 32 位/x86 口径，在 LP64 或别的 CPU 上走不通 ——
> 不是我们改坏的东西。每条的完整证据（量到的值、探针输出、修法 diff）在
> [port/README.md](../port/README.md)。

## 先说找洞的方法论（比清单更值钱）

1. **归因前先探针，猜过的每条都记下来**（`orthoglobe` 那一红四步探针全指错方向，
   因为量的是"默认贴图上的白"，不是真实纹理）；
2. **同一个二进制 A/B**（`PD_JIT=0/1`、`PD_IMM=0/1`）—— 先把"真退步"与"判据自己
   不稳"分开；`klock()` 驱动的脚本连跑两趟能差 2~4 倍，判退步要**交错多趟看分布**；
3. **断一次栈只要几秒** —— "符号地址相邻"不构成证据（`curvybuild` 那次差点把
   `mysleep` 的账赖到刚写的派发上）；
4. **`/usr/bin/sample` 一发再归类** —— "超时"那一栏不要当"算得久"放过：
   4004/4004 个样本全在一个函数里的形状是死循环，不是慢；
5. abort（rc=134）那一族走 `lldb -o "b malloc_error_break"`，它把"堆被写坏"与
   "free 了不是 malloc 来的东西"分开，后者直指清理路径。

诊断开关（env，全部收在移植层，原文零感知）：

| 开关 | 量什么 |
| --- | --- |
| `PD_RUNDBG=1` | 解释器操作数：填毒值查"谁没写"、`p[j]` 落在哪块地盘、执行前查头一页野指针并印整张参数表 |
| `PD_TEXDBG=1` | 贴图一路的 `glBindTexture/glTexImage2D/…` 参数 + 紧跟的 `glGetError`（名字 0 那格、`glsettex` 没被调，都是它逼出来的） |
| `PD_GLDBG=1` | GL 调用流与错误码 |
| `PD_MOUSE=x,y` | 固定鼠标位置复现某一帧 |
| `PD_JIT=0/1/2`、`PD_JITMAX/PD_JITNO/PD_JITFB`、`PD_IMM=0/1` | 见 [jit.md](jit.md) |

## 洞账

| # | 洞 | 落点 |
| --- | --- | --- |
| 1 | `kasm87` 收尾在非 x86 上 `??? not implemented`，往代码段写头字直接 SIGBUS | `port/a64/kasm_main_a64.c` |
| 2 | 解释器把入口地址当函数指针交回，多函数必崩 | `pd_a64_jit.c` 的真 thunk |
| 3 | `kasm87free/jumpback` free 不是 malloc 来的地址，当场 abort | thunk 尾巴上记账 |
| 4 | kcd 里 `gevalext` 抄本没人回填，递归/向后引用段错误 | `pd_a64_refresh_ext` |
| 5 | `parmdat` 指针按 4 字节写、按 8 字节读，两个指针参数就错位 | `pd_a64_parm.c` 事后重映射 |
| 6 | `KIMM` 是 unsigned int 常量，LP64 不回绕、`gevalext[]` 读飞 | 缝合文件 `#define KIMM …L` |
| 7 | 宿主函数按变参强转调用，arm64 变参走栈、只有第一个实参对 | `tools/mkrun.mjs` → `pd_a64_run.c` 精确原型 |
| 8 | 控制台写进空壳窗口，一切诊断看不见 | `kputs` → stderr |
| 9 | FBO 按 viewport 重建丢第 0 帧 | 一次建 2048x1536，存图取子矩形 |
| 12 | `plst[KGLB]` 把 gstatmem 算了两遍（十份崩的大半） | `pd_a64_jit.c` |
| 14 | `gnumarg` 不是参数个数（前头是全局 STATIC），参数基址排错、MOV 上段错误 | `pd_a64_parms()` 按家族筛 |
| 15 | `texttrans` 位图按"long 4 字节"算，越界压坏 `tbufmal` | `tools/mkmain.mjs` 按 `sizeof(long)` |
| 16 | 入口选 `kasm87c/cp` 看的是 `newvar[0]`，带全局时那是个全局 | `pd_a64_copyglob2struct` 自己判 |
| 17 | 维度表一格 4 字节、两头按 long 读写，`static planes[6][4]` 读出天文数字 | `tools/mkparse.mjs` |
| 18 | 带字符串（`C`）的宿主函数**一个都没被调** —— `glsettex` 从未上传，整幅采样默认白图 | `mkrun.mjs` 补五格；原型串不 NUL 结尾、找 `C` 只能 `memchr` 前 n 格 |
| 19 | USERFUNC"函数指针形参"那支读全局 `gasm[i].g` | `mkrun.mjs` `fixGlobGasm` |
| 20 | 原型 `dDddd`（指针不在末尾）解释器一声不响跳过，与 JIT 行为不一致 | JIT 只发解释器也会发的形状（口径：两条路答案相同） |
| 21 | `krand` 的 32 位回绕在 LP64 不发生：`rnd` 全错、`nrnd` 几乎死循环（两份"超时"的根） | `tools/mkmath.mjs`：截断挪到移位之前 |
| 22 | `glsettex(号,数组,…)` 一族（`dDdd/dDddd/dDdddd`）从来没真上传过 | 两头都补：`mkrun.mjs` 三格 + JIT 去掉形状门 |
| 23 | 名字 0 的贴图在 Apple 上装不进去（polydraw 不走 `glGenTextures`） | `pd_gl_texdbg.c` 换真名字 |
| 24 | 上传前的范围检查把地址截成低 32 位且比错了地块，永远过不去 | `pd_gl_settexarr_a64.c` 接管 |
| 25 | GLSL"上限"其实是我们没开扩展（`EXT_gpu_shader4` / `shader_texture_lod`） | `wglGetProcAddress` 处前缀注入 |

（#10、#11、#13 没用过就跳号了；#22~24 修完 `curvybuild` 2 → 19579 色。）

## 洞外的账 —— 不是移植欠的，别顺手"修"

| 份 | 事实 | 依据 |
| --- | --- | --- |
| `geo_duptris`（空画面） | 脚本发 `glBegin(GL_QUADS)`，几何着色器入口声明 `GL_TRIANGLES`；NVIDIA 拆 QUADS，Apple 不拆 —— **驱动语义的差**。收益只有 214 个非黑像素的细白线 | 探针量过：`EXT_geometry_shader4` 存在、着色器编过、错在 `err=0500` |
| `gspiral`（着色器错） | `float * int` 隐式提升是 GLSL 1.30 的东西，`EXT_gpu_shader4` 不给；NVIDIA 宽松所以原版能过 —— **参考脚本依赖非标准行为** | 按 1.20 规矩那一句非法；脚本是语料不许动 |
| `gears`（linux 黑屏） | **脚本自己的未定义行为**：`@f2` 里 `vec4 cc;` 未初始化就累加；Apple 给 0、mesa 给垃圾 → NaN 整屏黑。改一行初始化即出图（在 /tmp 试的，没动原文） | 抓屏直方图两条腿逐格相同，排掉了"抓屏坏" |
| `ribbons invasion`（linux 裁掉） | 几何正好贴在 `znear`（`z=-0.1` 即近平面 0.1）；Apple 留下、mesa 裁 | 改 `-0.5` 即出图且颜色与 arm64 同族 |
| `heightmap`（空画面） | 贴图那三个洞修完已真上传（`sub2D 256x256`），剩下的是几何/相机那一半，**另一件事** | `PD_TEXDBG` 下 `err=-` |
| linux 的 `geo_test` | `gl_PositionIn` 是 `EXT_geometry_shader4` 那一代的名字，mesa 早已不给（只有 3.2 core 的 `gl_in[]`） | 要它得改脚本 |

## 教训（每条都真踩过）

* 探针必须带 `@f` 段，且先确认它非黑再当判据（两份全黑探针白拿了"逐字节相同"）；
* ms/帧那一栏曾把 fps 当 ms 印了整整一轮 —— **量毫秒的东西，列没对上就整栏失真**；
* 单趟扫描的颜色数不能当结论（`ballsk` 同一命令连跑三趟可以是 43 色也可以是 1 色）；
* "改了没效果"先查自己那刀真的生效了（`strcmp` 写在非 NUL 结尾的原型串上，五条全不中，
  现象与没补一模一样）；
* `printf` 一族不接（`myprintf` 是真变参，定参强转不对），记在案上而不是硬补。
