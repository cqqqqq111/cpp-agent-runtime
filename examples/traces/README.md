# Trace 样本：一次失败运行的完整记录

本目录存放**真实运行**产出的 trace，用作 `TraceRecorder` 的实证。

`real_agent_run.json` 不是构造的演示数据，是一次真实 agent 运行后落盘的文件。它记录的是
一次**失败**——这正是它有价值的地方：成功的运行看起来都差不多，失败才暴露系统的边界。

---

## 运行概况

| 项 | 值 |
|---|---|
| 任务 | 「用工具找出 `src` 目录里所有的 TODO/FIXME 注释」 |
| 迭代轮数 | 6（撞上 `max_iterations` 上限） |
| LLM 调用 | 6 次 |
| 工具调用 | 16 次（15 成功 / **1 失败**） |
| Token 消耗 | 9067 |
| 墙钟耗时 | 8996 ms |
| 最终答案 | **不是答案**——是最后一条 grep 的原始输出 |

一个本该一轮结束的检索任务，跑了 6 轮仍未收敛。**只看终端输出，你只会看到"agent 好像没找到
TODO"；打开 trace，才知道它到底在干什么。**

---

## Trace 暴露的三个缺陷

### ① 工具契约不匹配（iteration 4）

LLM 传入了 `(?i)todo|fixme|xxx|hack`。这是 Python / PCRE 的内联 flag 语法，
而项目用的是 `std::regex` 的 ECMAScript 文法，**不支持 `(?...)`**：

```
phase:   OBSERVE
success: false
error:   Invalid '(?...)' zero-width assertion in regular expression
```

这条记录的价值在于**精确定位到是哪个参数踩的雷**。工具本身没有崩溃（异常隔离返回
`success=false`），错误被记进 trace、作为下一轮的观测回给 LLM——这正是 §"异常隔离"
设计要解决的问题。

> 根因：LLM 的训练语料里正则大多是 Python/PCRE 方言，而 `std::regex` 默认文法与之有差异。
> 升级方向：改用 `std::regex::ECMAScript` 之外的文法，或接入 PCRE2，或在工具描述里显式声明支持的语法。

### ② 静默的错误答案（iteration 5）

LLM 改用中文关键词搜索，pattern 为 `待办|待修复|未完成`。经 GBK 编码后落进 JSON，
变成了：

```
pattern='//.*(todo|fixme|����|���޸�|δ���)'
0 match(es) in 32 file(s)
```

**工具没有报错**，正常返回"0 匹配"。这是最危险的一类失败——agent 以为自己在工作，
实际在搜一个根本不存在的字符串，而且结果看起来完全正常。

这类问题**无法靠读代码或看终端输出发现**，只有结构化 trace 能把原始参数原样留下来。

> 根因：Windows 控制台/argv 的 GBK 编码与 JSON 要求的 UTF-8 不匹配。
> 同类问题在别处也踩到过（见 `benchmarks/COMPARISON_CXX_VS_PYTHON.md` 的 Python 侧编码处理）。

### ③ 循环检测完全失效（贯穿全程）

6 轮问的其实是同一个问题，只是每轮换了说法：

```
TODO|FIXME  →  TODO / FIXME（拆成两次）  →  search_content "TODO|FIXME"
→  (?i)todo|fixme|xxx|hack  →  +NOTE  →  ...
```

`detect_loop` **一次都没有触发**。原因是当前指纹是**精确匹配**：它比较的是工具名+参数，
而这几轮的参数各不相同，指纹自然不同。

这暴露了循环检测算法的真实边界：

| 能抓住 | 抓不住 |
|---|---|
| 完全重复的调用 | 换参数、换工具、换写法的「语义级原地打转」 |
| ABAB 式振荡 | 单调但不收敛的搜索（本例即是） |

> 升级方向：从「参数精确指纹」升级为「**动作语义指纹**」——例如对工具名+参数做归一化
> （排序 key、剥离大小写/空白差异），或引入「连续 N 轮没有产生新信息」的收敛判据。

---

## 附带的时序证据

trace 记录每个 step 的微秒级时间戳，可以还原耗时分布。本 run 中 5 次 THINK 的间隔为：

```
928 ms · 1040 ms · 1112 ms · 2444 ms · 1226 ms   =  6.75 s
```

占总墙钟 8996 ms 的 **75%**。结论：**瓶颈是 LLM 网络往返，而不是本地引擎。**

这个判断直接决定了优化方向——如果去优化本地工具执行，最多只能影响剩下的 25%。

---

## 自己复现

```bash
./build/demos/real_agent_demo.exe        # 生成新的 trace 到 logs/
python web/serve.py                      # 启动本地服务，在浏览器里回放
```

`web/index.html` 提供 trace 回放界面，按 THINK / ACT / OBSERVE 分色展示每一步。

---

## 关于本文件的清洗

原始 trace 由本机运行产生，其中的工具输出含有本机绝对路径与用户名。
提交前已做清洗：绝对路径前缀被移除（`src` 等相对路径保留），用户名替换为 `<user>`。
**除此之外内容未经改动**——包括那条失败的记录和那条乱码的 pattern。
