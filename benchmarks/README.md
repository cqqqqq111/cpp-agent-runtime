# 基准测试报告（Day 20）

> 三个 benchmark 覆盖三块核心：协程调度器、工具纯执行、ReAct 循环端到端。
> 所有测试**不经过真实 LLM**（用 `Scheduler::benchmark()` 和 `MockLLMAdapter`），
> 因此测量的是引擎本身的成本，而不是网络 / API 延迟。

---

## 运行方式

```bash
# 构建（首次会自动编译 benchmarks）
cmake -S . -B build
cmake --build build --target bench-scheduler bench-tools bench-react

# 逐个运行
./build/benchmarks/bench-scheduler.exe
./build/benchmarks/bench-tools.exe
./build/benchmarks/bench-react.exe

# 或用 ctest 一次性跑全部
cd build && ctest -R bench --output-on-failure
```

> **环境**：Windows 10，MinGW-W64 g++ 16.1.0，CMake 4.4，Release 构建。
> 绝对数值随机器不同而变，**相对趋势**（线性/持平/量级）才是结论。
>
> **运行依赖**：benchmark 可执行文件需要 MinGW 运行时 DLL（`libwinpthread-1.dll`
> 等）在 PATH 上。用 `g++` 的同一套 shell 运行即可。

---

## 结果

### 1. 协程调度器（`bench_scheduler`）

单批 = 创建 N 个协程 → schedule 全部 → `run_all()` 跑完的墙钟时间。

| Benchmark | Mean | P50 | P99 | Unit |
|-----------|------|-----|-----|------|
| 1K  tasks ×1 iter | 0.071 | 0.064 | 0.114 | ms |
| 10K tasks ×1 iter | 0.935 | 0.922 | 1.105 | ms |
| 100K tasks ×1 iter | 12.038 | 12.170 | 12.273 | ms |
| 100 tasks ×100 iters | 0.046 | 0.045 | 0.054 | ms |
| 100 tasks ×1K iters | 0.357 | 0.352 | 0.380 | ms |
| 100 tasks ×10K iters | 3.520 | 3.510 | 3.548 | ms |
| N=1  concurrent tools | 0.000 | 0.000 | 0.001 | ms |
| N=2  concurrent tools | 0.000 | 0.000 | 0.001 | ms |
| N=8  concurrent tools | 0.001 | 0.001 | 0.001 | ms |
| N=32 concurrent tools | 0.004 | 0.004 | 0.004 | ms |

**结论**
- 调度 + 完成一个空任务约 **~0.1 µs**（100K × 1 → 12 ms / 100K）。
- 单次 yield 约 **~3.5 ns**（100 × 10K = 100 万次 yield 只花 3.5 ms）。
- 并行模拟 N=1→32 近乎线性且绝对值极小 —— 协程切换是纯 `queue.push/pop`，无栈切换、无系统调用。

### 2. 工具吞吐量 / 延迟（`bench_tools`）

单次操作墙钟时间，不经 LLM。

| Benchmark | Mean | P50 | P99 | Unit |
|-----------|------|-----|-----|------|
| read_file 1KB | 0.242 | 0.223 | 0.485 | ms |
| read_file 100KB | 0.699 | 0.685 | 0.825 | ms |
| read_file 1MB | 5.049 | 5.081 | 5.306 | ms |
| read_file 10MB | 50.462 | 47.318 | 57.031 | ms |
| grep 10 files | 4.249 | 4.207 | 4.667 | ms |
| grep 100 files | 39.928 | 39.566 | 41.453 | ms |
| grep 500 files | 89.063 | 88.441 | 92.414 | ms |
| code_exec: shell echo | 6.142 | 5.440 | 12.665 | ms |
| code_exec: python pass | 29.639 | 28.376 | 35.205 | ms |
| vector search 1K (d=128) | 0.126 | 0.125 | 0.152 | ms |
| vector search 10K (d=128) | 1.333 | 1.263 | 1.783 | ms |
| vector search 100K (d=128) | 17.376 | 17.305 | 17.779 | ms |

**结论**
- `read_file` ~**5 ms / MB**（10 MB ≈ 50 ms），瓶颈是逐行 `fmt::format` 加行号（10 MB ≈ 15 万行）。
- `grep` ~**0.18 ms / 文件**，线性。
- 子进程启动延迟：shell ~6 ms，Python ~30 ms —— 纯 `_popen` + 解释器冷启动。
- `VectorIndex::search` 线性于向量数，~**0.17 µs / 向量**（dim=128）；100K 向量 17 ms，仍低于注释里「10K 内 < 50ms」的暴力搜索预期。真到 1M 向量才需要 HNSW。

### 3. ReAct 循环端到端（`bench_react`，MockLLM）

| Benchmark | Mean | P50 | P99 | Unit |
|-----------|------|-----|-----|------|
| react 1 iteration | 0.010 | 0.008 | 0.056 | ms |
| react 5 iterations | 0.044 | 0.042 | 0.077 | ms |
| react 10 iterations | 0.087 | 0.086 | 0.097 | ms |
| N=1 serial / parallel | 15.812 / 16.189 | 15.497 / 15.692 | 25.020 / 28.934 | ms |
| N=2 serial / parallel | 30.260 / 15.747 | 27.963 / 16.592 | 40.231 / 19.896 | ms |
| N=4 serial / parallel | 67.994 / 21.140 | 63.002 / 20.136 | 80.251 / 30.770 | ms |
| N=8 serial / parallel | 129.969 / 21.442 | 125.369 / 19.702 | 155.408 / 39.988 | ms |
| truncate to 100% budget | 0.000 | 0.000 | 0.001 | ms |
| truncate to 90% budget | 0.012 | 0.011 | 0.013 | ms |
| truncate to 80% budget | 0.031 | 0.030 | 0.036 | ms |

**结论**
- 循环机械开销 ~**8–10 µs / 迭代**（MockLLM 瞬时返回）。真实场景里 LLM 网络延迟（秒级）压倒性主导，循环自身可忽略。
- **并行显著快于串行**（N=2 时 15.7 vs 30.3 ms ≈ 1.9×，N=8 时 21.4 vs 130.0 ms ≈ 6.1×）。并行路径用 `std::async` 给每个工具调用派发独立工作线程，N 个阻塞调用墙钟重叠；N=1 时 `calls.size() > 1` 守卫回退串行，避免线程创建开销。前提是工具 `execute()` 线程安全。
- token 截断 ~**40 ns / 条消息**，O(被淘汰消息数)，可忽略。

---

## 已知边界 / 测量说明

| 项 | 说明 |
|----|------|
| `SleepTool` 用 `sleep_for(1ms)` | Windows 默认定时器粒度 ~15.6 ms，每个「1ms 睡眠」实际 ~15 ms，绝对数值被放大。串行与并行用同一 sleep，**相对加速比不受影响**；并行下 N 个 sleep 由工作线程重叠，墙钟 ≈ 一次睡眠。 |
| `MockLLM` 瞬时返回 | 隔离了 LLM 延迟，测的是循环机械成本，不是端到端真实时延。 |
| 向量维度 d=128 | 为控制内存（100K × 128 × 4B ≈ 51 MB）。真实 embedding（1536d）延迟约 ×12。 |
| `read_file` 10MB 是上限 | 工具硬编码 `kMaxSize = 10MB`，基准用二进制模式写恰好 10MB，踩在边界内。 |
| P50/P99 用最近秩 | 样本量小（3–20 次），P99 偏保守，看量级即可。 |

---

## 下一步优化方向（从数据推导）

1. **并行工具执行的线程池化**：当前并行用 `std::async` 每次新建线程，N 大时线程创建/销毁开销可被长任务摊薄；若工具是高频短调用，换成常驻线程池更优。真实工具还需保证 `execute()` 线程安全。
2. **向量检索**：100K 已到 17 ms，接近交互可接受上限；上 HNSW 可降到亚毫秒。
3. **read_file**：逐行 `fmt::format` 是 10MB 慢的主因，可改为批量 `stringstream` 拼接或流式返回。
