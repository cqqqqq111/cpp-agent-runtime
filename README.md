# cpp-agent-runtime —— 高性能 AI Agent 运行时（C++20 从零实现）

> 一个**不依赖 LangChain / LlamaIndex / FAISS** 的 AI Agent 运行时引擎。
> 核心逻辑（协程调度、ReAct 循环、工具派发、向量检索、记忆）全部用 **C++20 手写**，
> LLM / embedding 通过 **pybind11** 桥接 Python 的 OpenAI SDK 生态。
>
> **关键词**：C++20 协程 · ReAct Agent · Function Calling · 向量检索 / RAG · 异步运行时 · 多线程并行 · pybind11 · 性能基准

---

## 一、它解决什么问题

主流做法是用 LangChain 这类框架把「LLM + 工具 + 记忆」拼起来。这样做能跑通，但有三层代价：

| 痛点 | 具体表现 | 本项目的做法 |
|------|---------|-------------|
| **黑盒** | 只看到 `agent.run()`，不知道 ReAct 每步状态怎么流转、检索怎么算 | 自己实现整条链路，每一层可替换、可测量 |
| **异步开销大** | Python `asyncio` 受 GIL 限制，任务切换微秒级，CPU 密集任务无法真并行 | 自研 C++20 无栈协程调度器，切换纳秒级；工具并行用真线程，无 GIL |
| **不可观测** | 出问题只能加 print，没有结构化的推理链记录 | 每步 THINK/ACT/OBSERVE 落成 JSON trace，可在 web 面板回放 |

**一句话**：别人在「组装」框架，这个项目在「实现」框架——并且在每一层都给出可复现的量化数据。

---

## 二、架构

```
┌──────────────────────────────────────────────┐
│          Terminal TUI（ANSI 三面板 + 流式打字机）  │  ← 交互层
├──────────────────────────────────────────────┤
│         Agent Orchestrator（ReAct Loop）       │  ← 控制流 THINK→ACT→OBSERVE
├──────────────┬───────────────┬───────────────┤
│  Tool System │ Memory Manager│  LLM Adapter  │  ← 核心子系统
│   (插件式)    │   (三层记忆)   │  (pybind11)   │
├──────────────┴───────────────┴───────────────┤
│      C++20 Coroutine Scheduler（异步运行时）    │  ← 并发基座
├──────────────────────────────────────────────┤
│        pybind11 → Python（OpenAI SDK）        │  ← LLM / embedding 生态
└──────────────────────────────────────────────┘
```

**分层原则：每一层可独立替换。**
换 LLM 只改 `LLMAdapter` 实现；换检索算法只改 `VectorIndex`；换调度策略只改 `Scheduler`。

**ReAct 单轮数据流**：

```
messages(历史) + tools(JSON Schema)
        │
        ▼
   ┌─ THINK ──► LLM ──► 有 tool_calls？
   │                        ├─ 无 ──► 最终答案，结束
   │                        └─ 有 ──► ACT：ToolRegistry 派发执行
   │                                       │
   │                                   OBSERVE：结果作为 role=tool 消息回填
   │                                       │
   └────────── 循环（受 max_iterations / 循环检测 / token 预算约束）
```

---

## 三、目录结构

```
cpp-agent-runtime/
├── CMakeLists.txt              # C++20；fmt/spdlog 走 FetchContent，nlohmann/json vendored
├── src/
│   ├── core/
│   │   ├── task.h              # Task<T>：协程包装器，move-only，RAII 管理帧
│   │   └── scheduler.{h,cpp}   # Scheduler：FIFO 协作式调度 + yield_awaiter
│   ├── agent/
│   │   ├── message.{h,cpp}     # Message / MessageHistory / token 截断
│   │   ├── react_loop.{h,cpp}  # ReAct 主循环 + 循环检测 + 并行工具派发
│   │   └── agent_config.h      # max_iterations / token 预算 / 迭代超时
│   ├── tool/
│   │   ├── tool_base.{h,cpp}   # Tool 抽象 + ToolRegistry + JSON Schema→OpenAI 格式
│   │   ├── fs_tool.cpp         # read_file / list_dir / search_content（沙箱根目录）
│   │   ├── grep_tool.cpp       # 正则搜索，支持 glob 过滤
│   │   ├── git_tool.cpp        # status / diff / log（子进程）
│   │   └── code_exec_tool.cpp  # python / shell 子进程执行（超时 + 输出上限）
│   ├── memory/
│   │   ├── working_memory.{h,cpp}  # 会话级 TTL KV，注入 system prompt
│   │   ├── vector_index.h          # 暴力余弦检索 + partial_sort top-k
│   │   └── long_term_memory.h      # remember / recall，embedding 函数注入
│   ├── llm/
│   │   ├── llm_adapter.{h,cpp} # LLMAdapter 抽象 + 流式回调接口
│   │   └── py_bridge.{h,cpp}   # pybind11 嵌入式解释器（唯一 C++↔Python 接缝）
│   ├── tracing/trace_recorder.{h,cpp}  # TraceStep → JSON（可回放）
│   ├── tui/terminal_ui.{h,cpp} # ANSI 终端 UI，无外部依赖
│   └── main.cpp                # 组装 + CLI（--tui / --trace <file>）
├── python/llm_client.py        # OpenAI SDK 薄封装：chat / streaming / embedding
├── tests/                      # 8 个单元测试（CTest）
├── benchmarks/                 # 3 个 benchmark + Python 对照脚本
├── demos/                      # 4 个可运行场景（离线段式 LLM，输出 trace）
└── web/index.html              # trace 可视化面板
```

---

## 四、构建 & 运行

**依赖**

- C++20 编译器（GCC ≥ 10 / Clang ≥ 14 / MSVC 2022+；GCC < 14 需 `-fcoroutines`，CMake 已自动处理）
- CMake ≥ 3.20
- Python 3.10+ 与 `openai`、`pybind11`

```bash
pip install openai pybind11

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**运行**

```bash
export OPENAI_API_KEY="sk-..."       # 或兼容端点

./build/cpp-agent                    # 默认交互模式
./build/cpp-agent --tui              # TUI 模式（流式打字机效果）
./build/cpp-agent --trace out.json   # 带 trace 记录
```

**跑测试与基准**

```bash
cd build && ctest --output-on-failure     # 8 个单元测试 + 3 个 benchmark
./build/benchmarks/bench-scheduler
./build/benchmarks/bench-tools
./build/benchmarks/bench-react
```

> ⚠️ **Windows / MinGW 注意**：benchmark 可执行文件依赖 MinGW 运行时 DLL。若 `PATH` 中同时存在多个 MinGW
> （例如 MSYS2 的 `/mingw64/bin` 与独立安装的 MinGW），**必须让编译时所用的那一套排在前面**，
> 否则会因 `libstdc++-6.dll` 版本不匹配而启动失败（退出码 `0xc0000139`）。

---

## 五、端到端验证（真实运行结果）

### 5.1 单元测试：11/11 通过

```
$ cd build && ctest
      Start  1: scheduler ........   Passed    0.03 sec
      Start  2: message ..........   Passed    0.06 sec
      Start  3: react-loop .......   Passed    0.07 sec
      Start  4: fs-tool ..........   Passed    0.09 sec
      Start  5: memory ...........   Passed    8.09 sec
      Start  6: tools ............   Passed   10.29 sec
      Start  7: parallel .........   Passed    0.07 sec
      Start  8: day22 ............   Passed    0.60 sec
      Start  9: bench-scheduler ..   Passed    0.08 sec
      Start 10: bench-tools ......   Passed    2.14 sec
      Start 11: bench-react ......   Passed    3.13 sec

100% tests passed, 0 tests failed out of 11
```

### 5.2 真实 Agent 运行（离线 demo，含真实工具链）

`demos/code_review_demo` 用脚本化 LLM 驱动**真实的** `ReActLoop` + `filesystem`/`grep` 工具，
全程离线、可复现：

```
$ ./build/demos/code_review_demo
[info] ReAct: executing tool 'filesystem' (id=call_1)
[info] ReAct: executing tool 'grep' (id=call_2)
[info] ReAct: finished after 3 iterations (300 tokens)
[info] TraceRecorder: finalized — 7 steps, 3 LLM calls, 300 tokens, 10.3ms

Query: Review src/tool/fs_tool.h for security and style issues.

Final answer:
Review of src/tool/fs_tool.h:
  • Path traversal is correctly blocked via canonicalization against allowed_root_ (validate_path). Good.
  • Three actions (read_file / list_dir / search_content) are cleanly split into private helpers.
  • No TODO/FIXME markers remain in src/*.cpp.
Verdict: APPROVE — no blocking issues.

Iterations: 3 | Tools: 2 | Elapsed: 10.33 ms
Trace saved to: logs/code_review.json
```

每条推理链落成 JSON（本例 7 步 / 10 KB），可在 `web/index.html` 里回放 THINK/ACT/OBSERVE 全过程。

### 5.3 Benchmark（Release / MinGW，实测）

**协程调度器**

| 项 | Mean | 换算 |
|----|------|------|
| 1K tasks ×1 | 0.070 ms | — |
| 100K tasks ×1 | 10.964 ms | ~0.11 µs / 任务 |
| 100 tasks ×10K | 3.378 ms | **3.38 ns / 次 yield** |

**工具与检索**

| 项 | Mean | 换算 |
|----|------|------|
| read_file 10MB | 47.03 ms | ~4.7 ms/MB |
| grep 500 files | 87.46 ms | ~0.175 ms/文件（线性） |
| code_exec shell | 5.85 ms | 含子进程冷启动 |
| code_exec python | 29.53 ms | 含解释器冷启动 |
| vector search 100K (d=128) | 16.605 ms | ~0.166 µs / 向量 |

**ReAct 循环与并行**

| 项 | Mean | 说明 |
|----|------|------|
| react 10 iterations | 0.105 ms | ~10 µs/迭代机械开销 |
| N=8 serial | 125.86 ms | — |
| **N=8 parallel** | **15.53 ms** | **8.10× 墙钟加速** |

### 5.4 与 Python 生态的同机对比

同一台机器、同一时刻段、等价负载（`benchmarks/bench_python_comparison.py`，纯标准库）：

| 维度 | C++（实测） | Python（实测） | 差距 |
|------|------------|---------------|------|
| 协程 / 任务切换 | 3.38 ns | asyncio ~1065 ns | **~315×** |
| 并行 I/O（sleep×8） | 8.10× | threading 5.16× | 量级一致 |
| 并行 CPU（N=4） | 无 GIL，可近 4× | threading **1.00×** | **GIL 是分水岭** |
| 向量检索 10K | 1.32 ms | 纯 Python ~45.8 ms | ~35× |

> **结论**：协程的 315× 是「锦上添花」；**真正不可替代的是 GIL**——Python 多线程跑 CPU 密集任务
> 实测加速比 1.00×（完全退化为串行），asyncio 也救不了。这才是「为什么用 C++ 写引擎」最硬的论据。

---

## 六、关键设计决策

### 1. 无栈协程调度器：为什么快，以及为什么故意做得简单

`Scheduler` 只有一个 `std::queue<coroutine_handle<>>`，`co_await scheduler.yield()`
在 `await_suspend` 里把自己推回队尾。**不到 10 行，是整个并发基座。**

- **为什么快**：无栈协程的帧在堆上，`resume()` 直接跳回保存的 PC——**无栈切换、无系统调用、无内核参与**，
  一次切换 ≈ 两次队列操作 + 一次间接跳转。
- **为什么不做抢占/优先级**：Agent 的执行模型天然是「轮流发言」——一个 turn 跑到 I/O（LLM 调用或工具执行）
  就主动让出。协作式足够，且避免了锁与竞态。

### 2. ReAct 循环：终止条件比循环本身更难写

朴素循环的两个真实失败模式，都做了处理：

- **精确重复**：模型反复请求同一个工具调用 → 指纹比对，命中即停。
- **ABAB 振荡**：在 A、B 两个调用之间来回抖动 → 检测 `A→B→A` 且当前又是 `B`，即停。
- 指纹**刻意排除 call_id**——模型卡住时会生成全新的 id 却重复同样的请求。
- 另有 `max_iterations` 封顶、`max_context_tokens` 预算截断、可选的单次 LLM 调用超时。

### 3. 并行工具执行：一次由「负结果」驱动的优化

这是最能体现「定位问题 → 根因 → 取舍 → 验证」闭环的一段：

- **发现**：benchmark 测出「并行 ≈ 串行」（N=8 时 125 ms vs 127 ms）。
- **根因**：单线程协作式调度器 + 同步阻塞的工具 `execute()`——所谓「并行」只是把 N 个调用排队串行跑。
  协程只能在**主动让出**的地方切换，而工具从不 yield。
- **取舍**：异步 I/O / 常驻线程池 / `std::async` 三选一，选了 `std::async`（改动最小、语义最直白）。
- **结果**：N=8 从 125.9 ms → 15.5 ms（**8.10×**）。
- **代价**：每个调用新建线程（高频短调用应换线程池）、要求工具 `execute()` 线程安全、N=1 时回退串行避免线程创建开销。

### 4. 三层记忆与向量检索

```
WorkingMemory   ：会话内 TTL KV，to_context_string() 注入 system prompt
LongTermMemory  ：remember(text) → embed → VectorIndex
                  recall(query,k) → embed → 余弦 top-k → 注入上下文
VectorIndex     ：暴力余弦 + std::partial_sort 取 top-k
```

- **embedding 函数注入**（`EmbeddingFn`），使记忆层**provider 无关**——不绑定任何一家 API。
- **归一化优化**：embedding 本身已 L2 归一化，余弦相似度可退化为点积。
- **检索复杂度**：O(n·d) 扫描 + O(n·log k) 排序。10 万向量（d=128）实测 16.6 ms，交互可接受。

### 5. 工具系统：插件式 + 异常隔离

- `Tool` 抽象 + `ToolRegistry` 按名派发，`ToolDefinition::to_openai_function()` 输出 OpenAI 格式。
- **异常隔离**：工具抛出的异常被 Registry 捕获，转成 `success=false` 的 ToolResult 喂回 LLM，
  **而不是让整个 agent 崩溃**——模型会看到失败信息并尝试换思路。
- 文件工具做**沙箱**：路径 canonicalize 后校验是否越出 `allowed_root_`，阻断 `../../etc/passwd`。

### 6. C++ 引擎 + Python 生态：把接缝收在一处

`py_bridge.cpp` 是**唯一**的 C++↔Python 接缝：进程级持有一个嵌入式解释器，
缓存 `call_llm` / `call_embedding` 等函数句柄，避免每次调用重新 import。

- **C++ 侧**：调度、工具派发、向量检索、记忆——要快、要可控。
- **Python 侧**：LLM、embedding——要接 SDK、要生态。
- 分层结果：性能敏感层不付 Python 的代价，生态敏感层不重造轮子。

### 7. 可观测：trace 不是日志，是结构化数据

每个 `TraceStep` 记录 phase（THINK/ACT/OBSERVE）、微秒级时间戳、完整的 LLM 响应与工具结果，
序列化成 JSON 后在 `web/index.html` 回放。`TraceRecorder::wrap()` 的设计使它可以**包裹**已有的
TUI 回调而不侵入——TUI 照常工作，trace 同时落盘。

---

## 七、诚实的边界

知道自己的系统在什么规模、什么条件下会失效，比声称它「完美」更有价值。

| 简化点 | 现状 | 升级方向 |
|--------|------|---------|
| 向量索引 | 暴力搜索 O(n·d)，10 万向量 16.6 ms | HNSW / faiss（百万级亚毫秒） |
| 并行调度 | `std::async` 每次调用新建线程 | 常驻线程池 + `max_parallel` 上限 |
| 工具并发 | 要求工具 `execute()` 线程安全 | 无状态化 / 加锁 / 隔离 |
| 文本切块 | 整段文本存一个向量 | 滑动窗口 + overlap + 元数据 |
| **Embedding provider** | **依赖提供商提供 embedding 端点；若配置的 provider 无此端点（如某些纯 chat 服务），长期记忆/RAG 会降级为禁用** | 接入本地 embedding 模型（bge / sentence-transformers） |
| 多智能体 | 单 Agent | 多 Agent 协作 |
| 规模 | 单进程、单用户 | 多会话并发 / 服务化 |

**已知的测量说明**（避免误读 §5.3）：

- 协程 3.38 ns 是**空协程空 yield** 的最优情况；真实带 I/O 的任务切换会更慢，但纳秒级量级成立。
- 8.10× 是在**纯 sleep 阻塞工具**下测得的上界；真实工具（网络 / 磁盘 / 限流）会打折。
- 100K 检索是 **d=128**；真实 embedding（1536 维）延迟约 ×12。
- benchmark 报 mean / P50 / P99，样本量 3–20 次，**看量级与相对趋势**，绝对数值随机器变化。

---

## 八、相关文档

| 文档 | 内容 |
|------|------|
| [benchmarks/README.md](benchmarks/README.md) | benchmark 完整记录与测量说明 |
| [benchmarks/COMPARISON_CXX_VS_PYTHON.md](benchmarks/COMPARISON_CXX_VS_PYTHON.md) | C++ vs Python 同机对比报告 |

## License

MIT
