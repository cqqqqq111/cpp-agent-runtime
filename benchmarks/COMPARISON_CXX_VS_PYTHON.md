# C++ vs Python 同机对比印证报告

> 目的：把「C++ 比 Python 快」这句话从「引用常识」变成「同机实测」。
> 日期：2026-09-06
> 环境：Windows 10 Pro · MinGW-W64 g++ 16.1.0 · Release 构建 · Python 3.12 · CMake 4.4

---

## 0. 结论先行（TL;DR）

| 维度 | C++ 实测 | Python 实测 | 差距 |
|------|----------|------------|------|
| 协程 / 任务切换 | **3.38 ns** | asyncio ~1065 ns | **~315×** |
| 线程往返 | — | ~14.45 µs | ~4270× 慢于 C++ yield |
| 并行 I/O 工具（sleep×8） | **8.10×** | threading 5.16× | C++ 略优 |
| 并行 CPU 密集（N=4） | 无 GIL，理论近 4× | threading **1.00×**（GIL） | **决定性差距** |
| 向量检索 100K（dim=128） | **16.6 ms** | ~458 ms（外推） | **~28×** |

**三个核心宣传数字全部印证成立**，且并行加速实测（8.10×）高于最初宣称的 6.1×。

---

## 1. 印证目标与方法

### 目标
项目早期宣称的三个最「扎眼」的数字：
1. 协程切换 ~3.5 ns
2. 并行工具执行 6.1×
3. 向量检索 100K / 17 ms

### 方法
- **同机**：C++ 与 Python 在同一台机器、同一时刻段跑。
- **同测量**：都报告 mean / P50 / P99（nearest-rank），对齐 `bench_common.h`。
- **同负载**：协程切换 = N 协程 × K 次让出；并行工具 = sleep(1ms)×N；向量检索 = 暴力余弦 top-10。
- **C++ 侧**：重跑 `bench-scheduler` / `bench-tools` / `bench-react`。
- **Python 侧**：`benchmarks/bench_python_comparison.py`（纯标准库，无 LangChain / FAISS / numpy）。

---

## 2. C++ 同机实测（2026-09-06）

### 2.1 协程调度器（`bench-scheduler`）

| 项 | Mean | 换算 |
|----|------|------|
| 1K tasks ×1 | 0.070 ms | — |
| 10K tasks ×1 | 0.822 ms | — |
| 100K tasks ×1 | 10.964 ms | **~0.11 µs / 任务** |
| 100 tasks ×10K | 3.378 ms | **3.38 ns / yield**（100 万次 yield） |

> README 参考值：单任务 ~0.1 µs、yield ~3.5 ns —— **实测吻合**。

### 2.2 工具吞吐（`bench-tools`）

| 项 | Mean | 换算 |
|----|------|------|
| read_file 10MB | 47.03 ms | ~4.7 ms/MB |
| grep 500 files | 87.46 ms | ~0.175 ms/文件（线性） |
| code_exec shell | 5.85 ms | — |
| code_exec python | 29.53 ms | — |
| vector search 1K | 0.129 ms | — |
| vector search 10K | 1.322 ms | — |
| **vector search 100K** | **16.605 ms** | **~0.166 µs / 向量** |

> README 参考值：100K / 17 ms —— **实测吻合（16.6 ms）**。

### 2.3 ReAct 端到端（`bench-react`）

| 项 | Mean | 换算 |
|----|------|------|
| react 10 iterations | 0.105 ms | ~10 µs / 迭代 |
| N=8 serial | 125.86 ms | — |
| **N=8 parallel** | **15.53 ms** | **8.10× 加速** |

> README 参考值：N=8 并行 6.1× —— **实测更高（8.10×）**。

---

## 3. Python 同机实测

| 项 | Mean | 换算 |
|----|------|------|
| asyncio task switch（100×2000） | 212.94 ms | **~1065 ns / switch** |
| 线程 ping-pong（100K 往返） | — | **~14.45 µs / 往返** |
| 并行 I/O threading N=8 | 2.70 ms | 5.16×（串行 13.93 ms） |
| 并行 CPU threading N=4 | 266 ms | **1.00×（GIL，退化为串行）** |
| 纯 Python 向量 10K | 45.84 ms | 外推 100K ≈ 458 ms |

---

## 4. 完整对比表（实测 vs 实测）

| 维度 | C++（实测） | Python（实测） | 差距 | 说明 |
|------|------------|---------------|------|------|
| 协程切换 | 3.38 ns | asyncio 1065 ns | 315× | 空协程空 yield vs asyncio sleep(0) |
| 线程切换 | — | 14.45 µs | ~4270× | 相对 C++ yield |
| 并行 I/O（sleep×8） | 8.10× | 5.16× | C++ 略优 | 两者都受 Windows 定时器粒度影响 |
| 并行 CPU（N=4） | 无 GIL（理论近 4×） | 1.00× | 决定性 | GIL 是分水岭 |
| 向量检索 10K | 1.32 ms | 45.84 ms | ~35× | 暴力余弦，同样 O(n·d) |
| 向量检索 100K | 16.6 ms | ~458 ms（外推） | ~28× | 纯 Python 无 numpy |

---

## 5. 关键结论

### 5.1 三个宣传数字全部真实（可复现）
- 3.38 ns（宣称 3.5 ns）、16.6 ms（宣称 17 ms）、8.10×（宣称 6.1×）——全部在同一量级，**没有夸大**。
- 并行加速实测甚至更好，说明 6.1× 是保守值。

### 5.2 纠正了一处早期的错误结论
项目早期的设计笔记里写着「asyncio task switch ~100 ns，比协程快 2 倍」——实测 **~1065 ns，比 C++ 协程慢 315 倍**。
量级上都反了。这条错误由本次同机实测钉死。

### 5.3 「为什么用 C++」最硬的论据是 GIL，不是协程
- 协程 315× 是「锦上添花」；GIL 的「CPU 密集 4× vs 1×」才是「分水岭」。
- asyncio 救不了 CPU 密集任务（单线程 + GIL），threading 也救不了（GIL 串行化），只有 C++ 无 GIL 能真并行。

### 5.4 Python threading 的 5.16× 反而交叉验证了 C++ 的 8.10×
同一负载、同一台机器，Python 多线程也能把 sleep 重叠成 ~5×，证明「多线程让阻塞调用重叠」这个机制在两套代码里都成立——**C++ 的并行数字可信**。

---

## 6. 踩坑记录：DLL 版本不匹配（0xc0000139）

印证过程中 `bench-tools.exe` 报 `exit 127`，gdb 显示退出码 `0xc0000139`（`STATUS_ENTRYPOINT_NOT_FOUND`）。

- **根因**：编译用 MinGW 在 `C:/Users/chenqi/Desktop/study/mingw64/mingw64/bin`（GCC 16.1.0，`libgcc_s_seh-1.dll` 933KB），但运行时 `PATH` 里 MSYS2 的 `/mingw64/bin`（旧 GCC，同名 DLL 仅 151KB）排在前面，导致加载了旧版 `libstdc++-6.dll`。`bench-tools` 用到了新版 GCC 的符号 → 启动即失败。
- **为什么只有 bench-tools 挂**：`bench-scheduler` / `bench-react` 恰好没触发版本不匹配的符号。
- **解决**：运行前 `export PATH="/c/Users/chenqi/Desktop/study/mingw64/mingw64/bin:$PATH"`。
- **教训**：C++ benchmark 数据受「编译时 GCC 版本」和「运行时 DLL 版本」双重影响，出异常数字先查 DLL 一致性。

---

## 7. 诚实边界（别过度解读）

| 数字 | 前提 | 别误用为 |
|------|------|---------|
| 3.38 ns | 空协程空 yield 的最优情况 | 真实 I/O 任务的切换成本 |
| 8.10× | 纯 sleep(1ms) 阻塞工具 | 真实工具（LLM / 磁盘 / 限流）的加速 |
| 16.6 ms | 暴力余弦，dim=128 | 真实 embedding（1536d 约 ×12）|
| 458 ms（Python 100K） | 纯 Python 无 numpy | numpy 会快一个量级（但那是 C 扩展）|
| 「CPU 密集 C++ 近 4×」 | 理论推断（无 GIL） | 本轮未实测，需补 benchmark |

**下一步可做**：补一个 C++ 的 CPU 密集并行 benchmark，把「无 GIL 真并行」从理论变成实测，这是对比 Python 的最后一块短板。
