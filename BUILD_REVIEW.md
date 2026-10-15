# 构建、运行与代码评估

## 修复后结果（当前版本）

2026-10-14 已按严重程度完成本轮修复。当前代码在 WSL Ubuntu 上可完整构建，离线 CLI、三个场景 demo、终端 TUI 及本地模拟模型接口均已验证。

| 验证 | 当前结果 |
|---|---|
| Debug 完整构建与 CTest | **14/14 通过**，约 15.18 秒 |
| Release 完整构建与 CTest | **14/14 通过**，约 14.07 秒；测试编译参数含 `-UNDEBUG` |
| ASan + UBSan 重点检查 | **3/3 通过**：day22、regressions、python-bridge |
| 三个离线 demo | 均退出 0，trace 中 2/2/3 个工具结果全部成功 |
| 离线 CLI | 可从任意工作目录启动，生成含成功工具结果的 trace |
| 离线 TUI | 伪终端内验证输入 → echo 工具 → 回答 → 正常退出 |
| 未配置 API Key | 明确提示使用 `--offline`，返回退出码 1 |
| Python/HTTP/SSE 集成 | 本地服务验证普通与流式工具调用、空 content、模型参数传递、慢请求取消、工作线程 GIL 获取 |

修复内容按优先级排列：

1. **命令超时崩溃**：新增统一进程执行器。Linux 使用 posix_spawn、非阻塞读取、进程组终止和 waitpid 回收；Windows 分支使用 CreateProcess/Job Object。输出保留有上限，持续输出也不会阻止期限检查。CodeExecTool 和 GitTool 都传递真实退出状态，Python 代码使用参数向量传递，避免 shell 再次解释字符串。
2. **模型超时与对象生命周期**：删除捕获 Agent 后 detach 的线程；改由模型适配器在请求层实施期限。Python 使用 asyncio 的取消机制，在返回前关闭请求/流。同步、流式请求均有本地集成测试。自定义适配器必须明确支持正数期限，否则拒绝该配置。
3. **Python 桥接实际运行问题**：初始化后释放主线程 GIL，支持 TUI 工作线程调用；退出前重新获取并清理解释器；使用构建时 Python 的 site-packages 和源码路径，使不同工作目录启动可用；修复流式 JSON 导入和工具调用 `content=null` 的解析。
4. **文件访问边界**：GrepTool 校验规范化路径；两种目录搜索均逐文件检查符号链接目标；增加相对/绝对越界、符号链接和正常目录内文件的回归测试。
5. **消息协议完整性**：裁剪 assistant 工具调用时同时移除随后工具结果；发送前检查上下文预算；达到迭代上限明确返回停止原因，避免把最后的工具输出冒充最终回答。
6. **数据和失败状态**：向量插入、查询和加载验证维度及非有限值；加载失败保留旧索引；长期记忆验证 embedding 维度；缺少凭据与模型调用失败返回非零状态。
7. **可运行入口与测试**：新增 `--offline` 和 `--query`；修正 Linux 上的 Windows 路径测试假设；Release 下保留测试断言；新增进程后代清理、大量输出、消息配对、向量校验、本地桥接和 CLI 回归测试。

现在可在 WSL Bash 直接运行：

```bash
cd /home/administrator/cpp-agent-runtime
./build/debug/cpp-agent --offline --trace logs/offline.json
./build/debug/cpp-agent --offline --tui
./build/debug/demos/code_review_demo
.venv/bin/ctest --test-dir build/debug --output-on-failure --timeout 30
```

当前限制：真实远端模型仍需用户配置 API Key，本轮未调用远端服务；Windows 分支未在 Windows 编译运行。ASan/UBSan 检查关闭了 leak detection（嵌入式 Python 场景），不代表已完成泄漏审计。路径校验解决了已复现的静态越界，尚未实现抵抗恶意并发替换文件的描述符级沙箱；命令执行工具仍具有当前进程用户的权限。模型正数期限涵盖整个请求/流，0 关闭总期限，但保留 SDK 60 秒 IO 超时。

当前证据：

- `build/ctest-fixed-debug.log`、`build/ctest-fixed-release.log`
- `build/ctest-asan.log`
- `build/runtime-results.json`、`build/tui-smoke.log`
- `logs/code_review.json`、`logs/debug.json`、`logs/refactor.json`、`logs/tui-smoke.json`
- 回归测试源文件位于 `tests/test_regressions.cpp`、`tests/test_python_bridge.cpp`、`tests/test_bridge_server.py`、`tests/test_cli.py`

---

## 以下为修复前的基线记录（历史结果）

下文的 9/11 结果、问题描述、源代码行号和“两处平台适配”范围均属于首次评估；当前状态以上方修复后结果为准。

验证日期：2026-10-14。环境：WSL Ubuntu、GCC 13.3.0、Python 3.12.3、项目内 `.venv`，Debug 构建。

## 结论

项目在补充两处 Linux 平台适配后可以完成构建，三个离线 Agent demo 均能执行真实工具并生成 trace。现有 CTest 共 11 个条目（8 个功能测试程序、3 个 benchmark），本次通过 9 个，失败 2 个。真实模型调用因未配置 API Key 未能验证。

它具备较好的学习和演示价值，但目前存在命令超时崩溃、文件搜索边界不完整、消息截断破坏调用关系等问题，不宜视为已验证可靠的生产运行时。

## 本次完成的工作

- 在项目内创建 `.venv`，安装 cmake、pybind11、openai 及其依赖。未安装到系统 Python。
- 先对原始代码执行配置和构建，保留失败日志。
- 修改 `src/tool/code_exec_tool.cpp`、`src/tool/git_tool.cpp`：Windows 保留 `_popen/_pclose`，POSIX 使用 `popen/pclose`；Git 的 POSIX 路径改为带 shell 引用的 `git -C`。
- 验证 Git 路径包含空格和单引号时仍能正常执行 status。Windows 分支本次未运行。
- 完整 Debug 构建、完整 CTest、三个离线 demo、主程序入口及针对性边界实验。
- 未修改原有测试断言，未修复下述运行时问题，因此结果反映的是“仅平台适配后的代码”。

## 实测结果

| 检查 | 结果 | 说明 |
|---|---|---|
| 原始代码构建 | 失败 | 两个工具引用 Linux 不存在的 `_popen/_pclose` |
| 平台适配后 Debug 构建 | 成功 | 主程序、8 个测试程序、3 个 benchmark、4 个 demo 均编译成功 |
| scheduler / message / react-loop | 通过 | 断言启用 |
| fs-tool | 失败 | Windows 路径语义被用于 Linux 测试，详见下文 |
| memory | 通过 | 包括 TTL 等测试 |
| tools | 段错误 | 1 秒超时的测试实际等待约 10 秒后崩溃，后续用例未运行 |
| parallel / day22 | 通过 | day22 的超时用例主动等待后台线程结束，未覆盖提前析构 |
| 3 个 benchmark | 通过 | 只说明程序可执行；Debug 数据不用于验证 README 的 Release 性能宣称 |
| code_review_demo | 成功 | 7 个 trace 步骤，2 个工具结果，全部 success=true |
| debug_demo | 成功 | 7 个 trace 步骤，2 个工具结果，全部 success=true |
| refactor_demo | 成功 | 7 个 trace 步骤，3 个工具结果，全部 success=true |
| 主程序默认入口 | 启动成功，模型调用失败 | Python SDK 加载成功；缺少 API Key；失败后仍返回退出码 0 |

CTest 总耗时约 20.08 秒。三个离线 demo 使用固定脚本化模型响应，不能证明真实模型的推理或代码审查能力；本次额外检查了 trace 内的工具结果。

## 优先处理的问题

### P1：命令执行超时没有可靠终止进程，而且会崩溃（已实测）

位置：`src/tool/code_exec_tool.cpp` 的 `popen_read()`。

读线程通过引用访问 `pipe`，超时分支在主线程调用 `pipe.reset()`。这既没有可靠的子进程终止协议，又让管道关闭与异步读取发生交叠。本次 `test-tools` 在执行 sleep(10)、配置 1 秒超时的用例时，约 10 秒后才打印超时日志，随后段错误。

应使用可管理进程句柄/PID 的进程实现，明确终止、回收、输出读取和线程退出顺序；测试要断言实际耗时和子进程退出，不能仅检查输出包含 timeout。

### P1：目录搜索缺少完整的根目录约束（已实测）

- `src/tool/grep_tool.cpp` 直接接受绝对路径或拼接相对路径，没有校验规范化后的路径是否仍在 root 内。
- `src/tool/fs_tool.cpp` 校验最初的搜索路径，但递归搜索文件时没有重新检查文件符号链接的最终目标。

本次仅用 `build/review-fixture` 内自建的数据：将 allowed 子目录作为工具根目录，GrepTool 通过 `../outside.txt` 读到其外部的标记文本；FileSystemTool 搜索 allowed 时，也通过其中的文件符号链接读到了相同文本。

应统一路径校验，并覆盖递归枚举中的每一个文件目标。文件工具的局部路径限制也不能替代任意命令执行工具所需的进程隔离。

### P1：模型超时线程的对象生命周期不安全（静态检查）

位置：`src/agent/react_loop.cpp:93` 与超时分支的 `worker.detach()`。

线程捕获 `this`，并访问外部引用的 LLM；超时返回后调用方可以立即析构 Agent 或 LLM，而后台线程仍可能访问它们。复制输入消息并不能解决这些对象的生命周期问题。现有 `test_timeout_triggers()` 主动 sleep 400ms 等后台线程结束，避开了这个场景。本次没有用内存检测器复现这一项。

### P1：截断历史会拆散工具调用和工具结果（已实测）

位置：`src/agent/message.cpp` 的 `MessageHistory::truncate_to()`。

构造 user → assistant(tool_calls) → tool(result) 后截断到 1 token，最终只剩：

```json
{"content":"ok","name":"echo","role":"tool","tool_call_id":"c1"}
```

对应的 assistant 调用已经被删除，消息链关系被破坏。应按完整调用组裁剪，并在发送前验证调用与结果配对。本次没有发送此消息到真实 API。

### P2：错误状态可能被误报为成功（已实测）

- `CodeExecTool` 执行 `exit 7` 后仍返回 `success=true`，因为未利用子进程退出码。
- `src/main.cpp` 的默认入口捕获 LLM smoke test 异常后仍返回 0；本次缺少 API Key 正好复现。

应向调用者传播明确失败状态，防止 Agent 或 CI 把失败当作成功。

### P2：测试与跨平台验证还不充分

- `tests/test_fs_tool.cpp:150` 要求 `..\\..\\..\\Windows` 被判断为越界。Linux 将反斜杠当作文件名字符，所以返回的是目录不存在。前一个使用 `/` 的真实路径穿越检查已经通过；这项失败并不说明该检查绕过了沙箱。
- 测试大量依赖标准 `assert`，常规 Release 构建会定义 NDEBUG，使断言失效。因此 README 中 Release 下的“全通过”不能替代断言启用的功能测试。本次使用 Debug。
- 一些测试对结果检查偏弱，例如 Git 测试只要求非空输出，命令报错文本也可能满足条件。

### P2：向量维度不一致被静默接受（已实测）

位置：`src/memory/vector_index.h`。

插入二维向量 `[1,0]`，使用一维查询 `[1]`，仍返回相似度 1；实现只计算两个向量长度的最小值。应在插入、查询、加载时验证维度，避免给出看似合理但语义不正确的结果。

## 项目评价

优点是主线清楚：LLMAdapter、ToolRegistry、MessageHistory、ReActLoop 和 TraceRecorder 的边界容易理解；依赖注入让核心逻辑能脱离网络测试；离线 demo 和结构化 trace 有利于学习控制流及定位问题。

不足集中在异常路径和生命周期：进程管理、后台线程所有权、消息协议完整性和文件边界还没有形成可靠闭环。现有测试对正常流程的覆盖比对失败流程更充分。

架构描述也应收敛：实际 ReAct 工具并行使用 std::async，协程 Scheduler 并不是这条主执行路径的调度基础。空协程微基准和 sleep 工具的并行加速，不能直接推出真实 LLM Agent 的端到端加速。

建议学习和改进的顺序：先修进程超时与退出状态，再补目录边界和消息裁剪测试，随后处理模型超时的线程生命周期，最后再讨论线程池和性能优化。

## 复现命令

在 WSL Ubuntu 的 Bash 中执行：

```bash
cd /home/administrator/cpp-agent-runtime
source .venv/bin/activate

cmake -S . -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -Dpybind11_DIR="$(python -m pybind11 --cmakedir)" \
  -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build/debug -j 4
ctest --test-dir build/debug --output-on-failure --timeout 60

./build/debug/demos/code_review_demo
./build/debug/demos/debug_demo
./build/debug/demos/refactor_demo
```

当前 ctest 会复现上述两项失败。命令执行测试包含一个约 10 秒的等待。

## 原始证据

所有构建、日志与临时实验位于已忽略的 build 目录；demo trace 位于已忽略的 logs 目录。

- `build/configure-debug.log`：首次配置。
- `build/build-debug.log`：原始代码的 Linux 编译失败。
- `build/build-debug-portable.log`：平台适配后的完整构建。
- `build/ctest-debug.log`：9/11 测试结果。
- `build/debug/Testing/Temporary/LastTest.log`：完整测试输出与 benchmark 数据。
- `build/runtime-results.json`：demo 退出码及 trace 核对结果。
- `build/main-smoke.log`：缺少凭据的模型调用失败。
- `build/review_probe.cpp`、`build/review-probe.log`：边界问题的最小实验及输出。
- `logs/code_review.json`、`logs/debug.json`、`logs/refactor.json`：本次运行生成的 trace。
