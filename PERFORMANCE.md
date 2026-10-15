# 性能观测与基线

当前 trace 格式为 `1.1.0`。一次运行记录整体耗时、逐次 LLM 请求、逐个工具的等待和执行时间；`web/index.html` 可以打开 trace 查看详情。旧 trace 仍可查看，但汇总脚本会跳过缺少性能指标的文件。

## 2026-10-15：LLM 连接复用

**问题**：原来的 `_request()` 每次都创建 `AsyncOpenAI`，在一次请求后关闭客户端，并通过 `asyncio.run()` 销毁该次事件循环。同一个 ReAct 任务的下一轮请求仍需重新建连。仅缓存客户端而继续销毁它所属的事件循环，可能导致跨循环使用连接的问题。

**实现**：`python/llm_transport.py` 将事件循环和客户端成对放入有界会话池。每次调用独占一个会话，完成后归还；普通与流式请求可复用同一条空闲连接。没有新增常驻工作线程，回调仍在发起请求的线程执行。空闲会话可由另一线程独占使用，但同一个事件循环不会被两个线程同时驱动；每次任务使用本次调用方的 contextvars。

- `LLM_MAX_SESSIONS` 默认 4，允许 1–32，限制每个 `LLMClient` 的并发传输会话数。不是整个进程的 Agent 数量上限。
- 请求的总期限包括等待空闲会话、请求建立和读取响应；到期后先完成取消和流式响应清理，再归还会话，不留下仍会触发 C++ 回调的后台请求。
- 同步流式消费者若长时间阻塞，仍会阻塞当前事件循环，无法用异步计时器强行中断该消费者；网络等待与异步请求取消已覆盖。
- 模型名称改为每次请求的独立参数，避免并发调用通过修改共享 `_model` 相互影响。
- 继续保持 `max_retries=0`，401、503 等错误不会自动增加模型调用次数。连接复用不是响应缓存，不跳过模型推理，也不保证降低 token 账单。
- Python 单独使用 `LLMClient` 时可调用 `close()`；默认桥接客户端由 `shutdown_llm_clients()` 在解释器销毁之前关闭。关闭会等待正在执行的调用，调用方应先停止/加入 Agent 工作线程。
- C++ 的嵌入式解释器按进程生命周期管理。第三方原生扩展不支持可靠的销毁后重建，因此再次初始化会返回错误而不是继续触发崩溃；客户端池可以在同一解释器内关闭并重建。
- 实际服务/代理若返回 `Connection: close`、主动断开或使连接过期，仍需新连接。生产环境代理设置保持原样，只有本地测试子进程清除了代理变量以测量直接连接。

### 本地连接对照（不消耗 API 额度）

```bash
.venv/bin/python scripts/bench_transport.py --legacy
.venv/bin/python scripts/bench_transport.py
.venv/bin/ctest --test-dir build/release -R 'transport|python-bridge' --output-on-failure
```

基准使用相同 SDK、localhost HTTP/1.1 服务、合成文本和固定测试凭据；普通 JSON / SSE 交替，每组 20 次请求。`--legacy` 显式复现旧版“每次新客户端 + 新事件循环”的生命周期作为对照。以下为复用同一个外层 `LLMClient` 的结果，首次请求仍包含初始化：

| 场景 | 旧版连接数 → 新版连接数 | 旧版 P50 → 新版 P50 |
|---|---|---|
| localhost，无额外建连等待 | 20 → 1 | 3.280 → 1.228 ms |
| 每个新连接模拟 10ms 建立成本 | 20 → 1 | 13.672 → 1.214 ms |

新实现分别减少约 62.5% 和 91.1% 的该本地基准中位延迟。第二组的 10ms 是人为模拟的连接建立成本，不是测得的 TLS 或真实模型时延。没有任何线上提速比例结论，模型推理和服务端排队仍可能占主要时间。原始本地结果：`build/transport-before.json`、`build/transport-after.json`；测试中新建外层客户端的冷启动对照仍使用 20 条连接。

验证覆盖：普通/流式连接复用、跨调用线程复用、并发会话上限、排队超时不发送请求、网络超时后恢复、回调异常后的恢复、回调线程/contextvars 保持、并发模型隔离、401/503 不重试、关闭等待在途请求及释放服务端连接、同一解释器内重建默认客户端。C++ 集成测试也检查工作线程超时后的恢复与幂等关闭。本轮没有调用真实模型 API。

## 2026-10-15：第一轮上下文与预算优化

此前失败样本的工具执行仅约 0.1ms，不能据此得出稳定延迟结论，但暂时没有证据需要优先重写线程调度。因此这一轮先缩小模型输入，并保证小额预算下的用量和完成状态可信；验证全部在本地进行，没有新增付费 API 请求。

- **按任务选择工具**：非 TUI 真实模式支持 `--tools filesystem`、`--tools echo` 或逗号分隔的组合。只注册并发送选中的工具定义，未知名称会在发起请求前报错。不传参数时保持原来的三种只读工具；TUI 和离线模式不接受此参数，避免静默忽略。采样脚本已经按 code / echo 场景选择必要工具。
- **按行读取**：filesystem 的 `read_file` 支持 `start_line`、`end_line`，均从 1 开始且包含端点；不指定仍读取全文，省略 end_line 读到 EOF。输出保留原始行号并标注选取范围。非法/倒置范围、起点超出 EOF、越界路径会报错。它减少返回到模型的文本和格式化工作；读取靠后的行仍需顺序扫描前面的行。
- **流式真实用量**：请求最终 usage 分片，即使该分片没有 choices 也会记录 token 数。不再把流式分片数量当作输出 tokens。服务商未返回 usage 时，`llm_response.usage_available=false`，详情页显示未报告用量；不能把此时的汇总 0 当作免费。
- **输出上限处理**：保留模型返回的 finish_reason。`length` 对应运行状态 `output_limit`，`content_filter` 对应同名状态；不执行截断的工具参数，不将不完整结果标记为完成，CLI 返回非零，采样脚本停止后续调用。部分文本仍可保留在 trace 中供诊断。

按行读取的工具参数示例：

```json
{"action":"read_file","path":"agent/react_loop.cpp","start_line":150,"end_line":180}
```

无 API 消耗的本地对照：

```bash
./build/release/demos/context_size_demo
```

| 对照 | 全量 | 按需 | 字节减少 |
|---|---:|---:|---:|
| 当前版本全部工具定义 / 仅 filesystem | 1842 B | 961 B | 47.8% |
| 当前 react_loop.cpp 全文 / 第 150–180 行 | 22446 B | 1778 B | 92.1% |

这是同一版本下的 UTF-8 字节比较，不是 token 或延迟测试；裁剪范围必须由任务决定，不能省略任务所需代码。相比上次真实 trace 的 1627 B 工具定义，当前采样场景仅发送 filesystem 为 961 B；新增行范围参数本身也占 schema 空间，所以不选工具时 schema 会略增。

新增回归覆盖按行读取的边界/路径保护、工具选择、流式用量尾包/缺失、输出截断的非流式和流式路径。真实成功率与 P95 仍需后续有预算时采样，当前不宣称线上提速。

## 快速使用

后续轻量优化（2026-10-15）：在已知数量时预留消息、JSON 数组和循环检测字符串的容量；移动解析后的工具参数，避免 JSON 深拷贝；无 trace 订阅者时将工具输出字符串直接移入会话消息，有 trace 时仍保留完整输出。循环检测保持原有指纹格式，只减少字符串拼接临时对象。这些改动降低本地分配/拷贝开销，不改变工具选择、并发策略或 API 请求次数，也不能据此推导真实模型提速比例。

在项目根目录的 Ubuntu / WSL shell 中运行（使用现有 Release 构建）：

```bash
./build/release/cpp-agent --offline --trace logs/perf.json
.venv/bin/python scripts/trace_stats.py logs/perf.json
```

使用已配置的真实模型时去掉 `--offline`，并通过 `--query "你的任务"` 指定任务。离线模式只用于验证流程，不能代表真实模型延迟。

真实模型配置放在项目根目录 `.env`，`.env.example` 只保留占位模板。非 TUI 模式现在执行真实 ReAct 任务，支持 `--query` / `--trace` / `--stream` / `--tool-root`；工具包括只读 filesystem、grep 和 echo，每次最多 4 轮、每个 LLM 请求最长 60 秒。例如：

```bash
./build/release/cpp-agent --stream --tool-root src --query "读取 agent/agent_config.h 并简要说明默认配置" --trace logs/live.json
```

只有 `--stream` 才会采集首次文本回调时间。`--tool-root` 限制非 TUI 模式的文件工具范围，相对路径以该目录为起点。工具读取的内容会进入下一次模型请求。

## 真实模型的受限采样

确认可以向配置的模型服务发送指定源文件后：

```bash
.venv/bin/python scripts/measure_live.py --runs 3
# 试跑成功后再扩展采样，最多 20 次、10 分钟
.venv/bin/python scripts/measure_live.py --runs 20 --max-seconds 600
```

脚本只复制 `src/agent/agent_config.h` 到独立工具目录，让模型读取该文件并回答三个默认配置值；其他项目文件不在工具范围内。所有结果写入新的 `logs/live-performance/<时间>/`：`run-*.json` 为 trace，`manifest.json` 记录场景及进程耗时，`summary.json` 为汇总。进程耗时包含解释器初始化，trace 的 Agent 耗时不包含这部分。失败即停止，不自动重复请求。

不发送源码的连通性和延迟测试可用 `--scenario echo`，它只发送合成提示和 echo 文本，使用空的文件工具目录。两个场景的数据应分开比较。

```bash
.venv/bin/python scripts/measure_live.py --scenario echo --runs 3
```

当前已完成本地模拟 API 对真实命令行入口的验证；真实服务采样结果需以实际生成的 trace 为准。

### 小额预算测试

`.env` 可配置 `LLM_MAX_OUTPUT_TOKENS=256`，限制每个 ReAct 请求的最大输出；合法范围为 1–4096，未设置时保持原来的 4096。对 DeepSeek，可同时设置 `LLM_THINKING_MODE=disabled` 关闭思考；这个扩展参数仅适用于支持它的服务。输出上限不是人民币费用上限，输入和多轮调用也会计费。

```bash
.venv/bin/python scripts/measure_live.py --runs 1 --max-seconds 90 --no-stream
```

`--no-stream` 使用非流式返回的 provider token usage 核对用量，不采集首段文本时间。2026-10-15 的低额试跑（`deepseek-flash`，输出上限 256，关闭思考）记录在 `logs/live-performance/20261015T021016407849Z/`：首次请求成功，725 输入 / 58 输出 tokens，耗时 2313.83ms；读取文件工具成功，执行耗时 0.107ms；第二次请求返回服务繁忙 503，立即停止，没有重试。整次运行 2639.82ms。测试前后余额接口均显示 0.99 CNY，这一精度不能说明完全免费。本次为失败样本，不能用作成功任务的 P95 或完整性能基线。

按当日[官方价格](https://api-docs.deepseek.com/zh-cn/quick_start/pricing/)的高峰未命中输入 2 元/百万 tokens、输出 8 元/百万 tokens，成功请求的保守估算为 0.001914 元（忽略缓存折扣）；失败请求没有返回 usage，不据此推断其计费。实际以服务商账单为准。预算较小时先保留单次验证，等服务稳定后再决定是否扩展采样。

批量汇总同一任务、模型和配置的 trace：

```bash
.venv/bin/python scripts/trace_stats.py 'logs/my-scenario-*.json'
.venv/bin/python scripts/trace_stats.py 'logs/my-scenario-*.json' --json
```

输出包括样本数、均值、P50、P95、运行/请求状态和各工具失败数。P50 使用中位数；P95 使用 nearest-rank，即排序后第 `ceil(0.95 * N)` 项。无样本显示 N/A。统计包含成功、超时和失败样本，分析正常延迟时需另外选取成功 trace；不要仅凭单次运行判断尾延迟。

## 指标口径

所有耗时均为毫秒，使用单调时钟 `steady_clock`；ISO 时间戳仅用于定位实际发生时间。

| 所在位置 / 字段 | 含义 |
|---|---|
| `summary.performance.elapsed_ms` | 从进入 ReAct 到返回或抛出异常的总耗时；不含 Python 解释器的提前初始化，也不含最终 JSON 写盘 |
| `llm_ms` | 所有 LLM 适配器请求耗时之和，包含 Python 桥、SDK/网络/模型等待，以及流式消费者回调的耗时 |
| `tools_wall_ms` | 各工具批次从派发到收齐结果的墙钟时间之和 |
| `tool_execution_ms` | 所有工具执行时间相加；并行时可能大于批次墙钟时间，不代表 CPU 时间 |
| `tool_queue_ms` | 所有工具从本批次可执行时刻到开始执行的等待时间之和；串行包含等待前面的工具，并行包含线程启动/调度等待 |
| `llm_calls` / `tool_calls` | 实际记录的请求数 / 工具执行数 |
| `input_bytes` / `output_bytes` | 本次用户输入 / 最终返回文本的 UTF-8 字节数 |
| `steps[].elapsed_ms` | 步骤事件相对本次运行起点的时间偏移，不能作为该步骤自身耗时 |
| `steps[].duration_ms` | THINK 为请求耗时；OBSERVE 为工具批次耗时；ACT 是派发标记，记为 0 |
| `request_metrics.first_text_ms` | 从适配器请求开始到第一次非空文本回调的耗时；非流式请求或只有工具调用时为 null |
| `request_metrics.input_content_bytes` | 消息正文、历史工具调用函数名和参数的字节数之和 |
| `request_metrics.tool_schema_bytes` | 工具 schema 序列化后的字节数 |
| `request_metrics.output_content_bytes` | 返回正文及工具调用函数名和参数的字节数；异常时记录已收到的流式正文大小 |
| `tool_metrics[]` | 逐个工具的调用 ID、名称、等待/执行时间、参数/结果字节数、成功状态 |

这些字节数用于判断上下文和输出规模，不是 HTTP 传输大小，也不是 token 数。`first_text_ms` 不测工具调用参数的首个分片，不能当作服务端模型推理的纯 TTFT。

整次运行状态包括 `completed`、`timeout`、`error`、`context_budget`、`loop_detected`、`iteration_limit`。工具失败会单独标记，Agent 恢复后仍可能正常完成。LLM 异常会记录失败耗时，并尝试自动保存错误 trace，再继续抛出原异常。

代码中可通过 `agent.last_run_metrics()` 读取最近一次运行的汇总，无须注册 trace 回调。复用 recorder 时只保留最近一次运行；需要长期比较时，每次使用独立的输出文件。

## 可重复的串行 / 并行对照

```bash
./build/release/demos/performance_demo 20
# 用上面打印的目录替换 <本次目录>
.venv/bin/python scripts/trace_stats.py 'logs/performance/<本次目录>/serial-*.json'
.venv/bin/python scripts/trace_stats.py 'logs/performance/<本次目录>/parallel-*.json'
```

该场景不调用外部服务：两次模拟 LLM 请求各等待 5ms，流式结束另等 2ms；中间调用三个各等待 10ms 的工具。每种模式默认采样 20 次，trace 写入新的目录，避免混入历史结果。

2026-10-14 在本机 WSL Ubuntu、GCC 13.3、Release 构建下得到：

| 指标（ms） | 串行 P50 | 串行 P95 | 并行 P50 | 并行 P95 |
|---|---:|---:|---:|---:|
| 整次运行 | 42.543 | 42.606 | 22.554 | 22.653 |
| LLM 请求总耗时 | 12.236 | 12.264 | 12.236 | 12.264 |
| 工具批次墙钟耗时 | 30.240 | 30.279 | 10.269 | 10.354 |
| 工具执行耗时之和 | 30.237 | 30.275 | 30.242 | 30.284 |
| 首次非空文本 | 5.077 | 5.095 | 5.078 | 5.093 |

每种模式均为 20 次完成、40 次请求成功、60 次工具成功。原始数据在 `logs/performance/1791991405643352359/`（本地生成，未纳入版本控制）。

这个对照验证了观测能区分并行重叠时间与累计工作时间；它展示的是现有并行机制的行为，不能推导真实业务的加速比。20 个样本仅作为初始基线。

## 下一步如何决定优化方向

1. 选择固定的真实任务，固定模型、上下文与日志配置，采集至少 20 次，分别看成功率与延迟分布。
2. 若 LLM 请求占比最大，先看请求次数、上下文字节数和首段文本时间；当前统计不拆分网络、SDK、GIL 和模型端耗时。
3. 若工具时间占比最大，按工具名称看执行 P95；若短工具的等待/派发开销明显，再评估有界线程池。
4. 修改后用相同输入、配置与构建模式复测。不要把 Debug/Release、模拟/真实请求或不同模型混为一组。

计时和 trace 本身存在开销，尤其对微秒级工具；最终落盘在运行计时之外，步骤回调仍计入运行时间。本阶段未做 CPU、内存分配或函数级采样分析。
