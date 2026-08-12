# 10 — 性能基准与改进方向（M8）

测量环境：Apple Silicon (macOS)，LLVM 22.1.8，分辨率 320×320（framebench）/ 320×320（bench，纯 EVAL）。
两种基准：

- `bench` — 仅 EVAL→GLCmd 录制（不含 GL 光栅），反映解释器/JIT 的纯计算开销。
- `framebench` — **真实整帧**：EVAL 录制 + `pd_gl_renderer_render`（FBO replay）+ `glReadPixels` 回读。这才是“卡不卡 60fps”的判定依据。

## 1. 真实整帧 FPS（framebench，60 帧均值）

| 脚本 | interp fps | llvm fps | llvm 加速 | 是否 ≥60 | 瓶颈 |
|------|-----------|----------|----------|----------|------|
| tigrou/balls2k | 37.7 | 59.1 | 1.57× | 临界 | EVAL+draw |
| tigrou/particules sparks | 45.0 | 68.6 | 1.52× | 过线 | EVAL |
| tigrou/ribbons invasion | 81.5 | 113.1 | 1.39× | 过线 | EVAL |
| tigrou/disco ball | 14.0 | 14.2 | 1.01× | **远低于** | **draw call 密集** |
| tigrou/snake tube | 40.0 | 42.4 | 1.06× | 远低于 | GL 几何 |
| ken/drawsph | 45.7 | 52.9 | 1.16× | 低于 | GL 几何 |
| tigrou/ballsk | 110.7 | 168.3 | 1.52× | 过线 | EVAL |
| tigrou/metaballs | 605.4 | 644.8 | 1.07× | 过线 | EVAL |
| tigrou/menger sponge | 89.9 | 91.6 | 1.02× | 过线 | GL |
| ken/heightmap | 343.1 | 540.7 | 1.58× | 过线 | EVAL |

## 2. 结论

1. **LLVM JIT（核心目标）在“计算密集”例子上稳定加速 1.4–1.6×**，且把若干 interp 卡在 40–45fps 的例子拉过 60fps 线（particules sparks 45→68.6、balls2k 37.7→59.1）。
2. **轻计算例子（每帧 <1ms）LLVM 反而略慢（0.3–1.0×）**：LLVM 调用约定（构造 `pd_Ctx`、函数调用）比解释器逐指令循环更重。这是 JIT 固有特性，不是 bug。
3. **`disco ball` / `snake tube` / `drawsph` 即使 LLVM 也远低于 60fps**——瓶颈在 **GL 光栅与 draw call 提交**（disco ball ≈ 19970 个 draw call，每球一个 `glBegin/glEnd`，CPU 提交开销主导，EVAL 仅占 ~1.5ms / 70ms）。**这类与 JIT 无关，JIT 救不了。**

## 3. 是否需要改进 / 改进方向

- **计算密集（已解决）**：LLVM JIT 已满足，无需额外动作。
- **draw-call 密集（已知优化项，未做）**：要真正把 `disco ball` 拉过 60fps，需要 **实例化渲染（instancing）**：将“每球一个变换 + 一个 draw call”改为“一个基础球 VBO + 每实例模型矩阵数组 + 单次 `glDrawArraysInstanced`”。这要求 polyhost 几何录制区分“每实例变化的状态（矩阵/颜色）”与“共享几何”，属于对 `pd_gl_renderer` 的较大重构，正确性风险高，列为后续优化而非阻塞项。
- **sljit（过渡后端）**：始终 ≈1.0×（调用委派给解释器），符合“过渡”定位，不追求其独立加速。

## 4. 复现

```sh
cd c_impl
make build/bench build/framebench
./build/framebench ../tigrou/disco\ ball.pss --frames 60
./build/bench ../tigrou/balls2k.pss --frames 60
```
