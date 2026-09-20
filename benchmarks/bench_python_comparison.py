#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Python 侧对照组 —— 与 C++ benchmark 同机对比。

背景：
  项目早期宣称的 C++ 数字（协程切换 ~3.5ns、并行工具 6.1×、向量检索
  100K/17ms）证明了「我很快」，但还缺一块：没有在同一台机器上测 Python/asyncio
  的等价负载，所以「比 Python 快多少」一直是引用常识而非实测。

  本脚本补上这块。只用标准库（asyncio / threading / concurrent.futures），
  不依赖 LangChain / FAISS / numpy，测 4 组等价负载，最后与 C++ 侧数字并列成
  对比表，把「C++ vs Python」变成实测结论。

运行：
  python benchmarks/bench_python_comparison.py

测量约定（对齐 benchmarks/bench_common.h）：
  - time.perf_counter() 计时，报告 mean / P50 / P99（nearest-rank）
  - 只测「运行」阶段；任务/协程的「创建」不计时（C++ 侧 scheduler.cpp 同样把
    tasks.push_back 放在计时起点之前）

边界（诚实标注，与 C++ 侧 README 对齐）：
  - asyncio 的 task 切换会经过 event loop（含调度器固定开销），测的是生态真实
    成本而非纯函数调用——这正是它比 C++ yield 慢 ~3 个数量级的原因。
  - Windows 定时器粒度 ~15.6ms：sleep(1ms) 实际睡 ~15ms。串行/并行用同一 sleep，
    相对加速比不受影响，但绝对数值被放大（C++ 侧同样受影响，故可比）。
  - 纯 Python 的 CPU 密集任务受 GIL 限制，threading 无法真并行；C++ 无 GIL
    才能真并行——这是「为什么用 C++」最硬的一条论据。
"""

import sys

# Windows 中文控制台默认 GBK；强制 stdout 用 UTF-8，避免中文与特殊符号乱码/崩溃。
# 若在 cmd.exe（GBK）里仍乱码，属终端显示问题，不影响脚本逻辑与数值。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

import asyncio
import concurrent.futures
import math
import random
import threading
import time

# ---------------------------------------------------------------------------
# 统计工具（对齐 C++ bench_common.h 的 nearest-rank 百分位）
# ---------------------------------------------------------------------------

def _percentile(sorted_vals, p):
    n = len(sorted_vals)
    if n == 0:
        return 0.0
    idx = math.ceil(p / 100.0 * n)
    idx = 0 if idx == 0 else idx - 1
    idx = min(idx, n - 1)
    return sorted_vals[idx]


def _summarize_ms(samples_s):
    """samples_s: 每次测量耗时（秒）。返回 (mean, p50, p99)，单位 ms。"""
    ms = sorted(s * 1000.0 for s in samples_s)
    mean = sum(ms) / len(ms)
    return mean, _percentile(ms, 50.0), _percentile(ms, 99.0)


def _run_reps_ms(fn, reps):
    """fn 返回单次耗时（秒）。收集 reps 次，返回 (mean, p50, p99) ms。"""
    samples = []
    for _ in range(reps):
        t0 = time.perf_counter()
        fn()
        samples.append(time.perf_counter() - t0)
    return _summarize_ms(samples)


def _print_header():
    print(f"{'Benchmark':<38}{'Mean':>10}{'P50':>10}{'P99':>10}  Unit")
    print("-" * 38 + " " + "-" * 10 + " " + "-" * 10 + " " + "-" * 10 + "  ----")


def _print_row(name, mean, p50, p99, unit="ms"):
    print(f"{name:<38}{mean:>10.3f}{p50:>10.3f}{p99:>10.3f}  {unit}")


# 全局收集各 section 的关键结果，供最后的对比表引用
RESULTS = {}


# ============================================================================
# 1. asyncio task 切换（对应 C++ 的 co_await yield）
# ============================================================================

async def _asyncio_worker(k):
    for _ in range(k):
        await asyncio.sleep(0)  # 1 次 await = 1 次让出/切换


def _run_asyncio_switch(n_tasks, k_iters):
    """返回单次场景耗时（秒）。协程创建在计时起点之前。"""
    async def scenario():
        tasks = [asyncio.create_task(_asyncio_worker(k_iters)) for _ in range(n_tasks)]
        t0 = time.perf_counter()
        await asyncio.gather(*tasks)
        return time.perf_counter() - t0
    return asyncio.run(scenario())


def bench_asyncio_switch():
    print("\n=== 1. asyncio task switch（N 协程 x K 次让出）===")
    print("对应 C++ 的 bench_scheduler（yield ~3.5ns）")
    _print_header()

    best_per_switch_ns = None
    for n, k, reps in [(100, 500, 3), (1000, 500, 3), (100, 2000, 3)]:
        total_switches = n * k
        def run():
            return _run_asyncio_switch(n, k)
        mean, p50, p99 = _run_reps_ms(run, reps)
        per_switch_ns = mean * 1e6 / total_switches  # ms → ns，摊到每次切换
        _print_row(f"{n} tasks x{k} ({total_switches} switches)", mean, p50, p99)
        print(f"  -> per-switch: {per_switch_ns:,.0f} ns")
        if best_per_switch_ns is None or per_switch_ns < best_per_switch_ns:
            best_per_switch_ns = per_switch_ns

    RESULTS["asyncio_per_switch_ns"] = best_per_switch_ns or 0.0


# ============================================================================
# 2. 线程上下文切换（ping-pong）
# ============================================================================

def _thread_roundtrip(rounds):
    """两个线程 ping-pong，返回每次往返耗时（秒）。"""
    e_a = threading.Event()
    e_b = threading.Event()
    stop = threading.Event()

    def worker():
        while True:
            e_a.wait()
            e_a.clear()
            if stop.is_set():
                return
            e_b.set()

    t = threading.Thread(target=worker, daemon=True)
    t.start()
    # warmup：让 GIL / 调度器 / 页表进入稳态
    for _ in range(2000):
        e_a.set()
        e_b.wait()
        e_b.clear()

    t0 = time.perf_counter()
    for _ in range(rounds):
        e_a.set()
        e_b.wait()
        e_b.clear()
    elapsed = time.perf_counter() - t0

    stop.set()
    e_a.set()  # 唤醒 worker 让其看到 stop
    t.join(timeout=2.0)
    return elapsed / rounds


def bench_thread_switch():
    print("\n=== 2. 线程上下文切换（ping-pong）===")
    print("每次往返 = 主线程唤醒 worker + worker 唤醒主线程 ≈ 2 次内核调度")
    _print_header()

    rounds = 100_000
    samples_s = []
    for _ in range(3):
        samples_s.append(_thread_roundtrip(rounds))
    mean, p50, p99 = _summarize_ms(samples_s)
    per_roundtrip_us = mean * 1000.0  # ms → us（mean 已是每次往返的耗时）
    _print_row(f"thread ping-pong ({rounds} roundtrips)", mean, p50, p99)
    print(f"  -> per-roundtrip: {per_roundtrip_us:,.2f} us")

    RESULTS["thread_per_roundtrip_us"] = per_roundtrip_us


# ============================================================================
# 3. 并行 I/O 工具（sleep 1ms 模拟阻塞调用）
# ============================================================================

def _io_sleep(_=None):
    time.sleep(0.001)  # 1ms；Windows 定时器粒度 ~15.6ms 会放大绝对值


def bench_parallel_io():
    print("\n=== 3. 并行 I/O 工具（sleep 1ms x N，模拟阻塞工具调用）===")
    print("对应 C++ 的 bench_react SleepTool（串行 vs 并行 6.1x）")
    _print_header()

    serial_8 = None
    thread_8 = None
    for n in (4, 8):
        # 串行
        def serial():
            for _ in range(n):
                _io_sleep()
        sm = _run_reps_ms(serial, 5)
        _print_row(f"N={n} serial", *sm)

        # 多线程（ThreadPoolExecutor，对应 C++ 的 std::async）
        def thread_par():
            with concurrent.futures.ThreadPoolExecutor(max_workers=n) as ex:
                list(ex.map(_io_sleep, range(n)))
        tm = _run_reps_ms(thread_par, 5)
        _print_row(f"N={n} threading", *tm)

        # asyncio（并发而非并行：同一线程内定时器重叠）
        def asyncio_par():
            async def sc():
                await asyncio.gather(*[asyncio.sleep(0.001) for _ in range(n)])
            asyncio.run(sc())
        am = _run_reps_ms(asyncio_par, 5)
        _print_row(f"N={n} asyncio.gather", *am)

        if n == 8:
            serial_8, thread_8 = sm[0], tm[0]

    if serial_8 and thread_8:
        speedup = serial_8 / thread_8
        print(f"  -> N=8 串行/多线程加速比: {speedup:.2f}x")
        print(f"     （注意：I/O 等待场景下 asyncio 也能重叠，加速不是 C++ 独有优势；")
        print(f"      C++ 多线程的真正优势在 CPU 密集 + 无 GIL，见第 4 节）")
        RESULTS["io_speedup"] = speedup


# ============================================================================
# 4. 并行 CPU 密集（GIL 演示）
# ============================================================================

_WORK = 1_000_000  # 纯 Python 忙循环次数，调小可缩短运行时间


def _cpu_task(_=None):
    acc = 0
    for i in range(_WORK):
        acc += (i * i) & 0xFFFF
    return acc


def bench_parallel_cpu():
    print("\n=== 4. 并行 CPU 密集（GIL 演示：threading 无法真并行）===")
    print("这是 C++ 无 GIL 的核心优势 —— 同一份 CPU 密集负载在 C++ 里真并行")
    _print_header()

    n = 4

    def serial():
        for _ in range(n):
            _cpu_task()
    sm = _run_reps_ms(serial, 3)
    _print_row(f"N={n} CPU serial", *sm)

    def thread_par():
        with concurrent.futures.ThreadPoolExecutor(max_workers=n) as ex:
            list(ex.map(_cpu_task, range(n)))
    tm = _run_reps_ms(thread_par, 3)
    _print_row(f"N={n} CPU threading", *tm)

    speedup = sm[0] / tm[0] if tm[0] else 0.0
    print(f"  -> threading 加速比: {speedup:.2f}x（≈1 说明 GIL 让『并行』退化为串行）")
    print(f"     对比：C++ std::async 跑同一负载可接近 {n:.0f}x（真多核并行）")
    RESULTS["cpu_speedup"] = speedup


# ============================================================================
# 5. 纯 Python 暴力余弦检索（对应 C++ 的 VectorIndex）
# ============================================================================

def bench_python_vector_search():
    print("\n=== 5. 纯 Python 暴力余弦检索（dim=128）===")
    print("对应 C++ bench_tools 的 VectorIndex::search（10K/1.3ms，100K/17ms）")
    _print_header()

    dim = 128
    vec_10k_ms = None
    for n, reps in [(1000, 3), (10000, 3)]:
        random.seed(0)
        vecs = [[random.uniform(-1, 1) for _ in range(dim)] for _ in range(n)]
        query = [random.uniform(-1, 1) for _ in range(dim)]

        def search():
            scored = []
            for i, v in enumerate(vecs):
                dot = 0.0
                for a, b in zip(query, v):
                    dot += a * b
                scored.append((dot, i))
            scored.sort(key=lambda x: -x[0])
            return scored[:10]

        def run():
            t0 = time.perf_counter()
            search()
            return time.perf_counter() - t0

        samples = [run() for _ in range(reps)]
        mean, p50, p99 = _summarize_ms(samples)
        _print_row(f"search {n} vectors (dim={dim})", mean, p50, p99)
        if n == 10000:
            vec_10k_ms = mean

    if vec_10k_ms:
        # 线性外推到 100K（纯 Python 暴力检索是 O(n·d)，线性）
        print(f"  -> 线性外推到 100K 向量: ~{vec_10k_ms * 10:.0f} ms")
        RESULTS["vector_10k_ms"] = vec_10k_ms


# ============================================================================
# 对比总结表
# ============================================================================

def print_summary():
    # C++ 参考值来自 benchmarks/README.md（同机复测前视为参考）
    cpp_yield_ns = 3.5
    cpp_task_us = 0.1
    cpp_io_speedup = 6.1
    cpp_vector_100k_ms = 17.0
    cpp_vector_10k_ms = 1.3

    print("\n" + "=" * 80)
    print("对比总结：C++（参考值，来自 benchmarks/README.md） vs Python（本机实测）")
    print("=" * 80)
    print("⚠ 要得到可信结论，请在同一台机器重新跑 C++ bench-*，把实测值替换下表参考值。")
    print()

    # 1. 协程/任务切换
    a_switch = RESULTS.get("asyncio_per_switch_ns", 0.0)
    t_switch_us = RESULTS.get("thread_per_roundtrip_us", 0.0)
    print(f"【1 协程 / 任务切换】")
    print(f"  C++ 协程 yield       : ~{cpp_yield_ns} ns")
    print(f"  Python asyncio switch: ~{a_switch:,.0f} ns  (~{a_switch / cpp_yield_ns:,.0f}x 慢)")
    print(f"  Python 线程往返      : ~{t_switch_us:,.2f} us  (~{t_switch_us * 1000 / cpp_yield_ns:,.0f}x 慢)")
    print()

    # 2. 并行 I/O
    print(f"【2 并行 I/O 工具（sleep 1ms x 8）】")
    print(f"  C++  串行->并行 : {cpp_io_speedup}x")
    print(f"  Python threading : {RESULTS.get('io_speedup', 0.0):.2f}x")
    print(f"  （I/O 场景 asyncio 也能重叠；真正的分水岭在 CPU 密集 + GIL）")
    print()

    # 3. CPU 密集
    print(f"【3 并行 CPU 密集（N=4）】")
    print(f"  Python threading 加速比 : {RESULTS.get('cpu_speedup', 0.0):.2f}x（GIL 限制，≈串行）")
    print(f"  C++   std::async       : 可接近 4x（真多核，无 GIL）")
    print()

    # 4. 向量检索
    v10k = RESULTS.get("vector_10k_ms", 0.0)
    print(f"【4 向量检索（dim=128）】")
    print(f"  C++   10K : ~{cpp_vector_10k_ms} ms   100K : ~{cpp_vector_100k_ms} ms")
    print(f"  Python 10K : ~{v10k:.0f} ms   100K(外推) : ~{v10k * 10:.0f} ms")
    if v10k > 0:
        print(f"  （C++ 在 10K 上约快 {v10k / cpp_vector_10k_ms:,.0f}x）")
    print()

    print("结论：把上表数字换成你同机复测的结果，就是「我 vs Python」的可信论据。")
    print("     day2 文档里「asyncio task switch ~100ns」是错的，实测见上表第 1 项。")


# ============================================================================
# main
# ============================================================================

def main():
    print("=" * 80)
    print("Python 侧对照基准（纯标准库，用于与 C++ bench-* 同机对比）")
    print("=" * 80)

    bench_asyncio_switch()
    bench_thread_switch()
    bench_parallel_io()
    bench_parallel_cpu()
    bench_python_vector_search()

    print_summary()


if __name__ == "__main__":
    main()
