"""Run a bounded, read-only live-model benchmark from the project root."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from trace_stats import summarize

ROOT = Path(__file__).resolve().parents[1]
QUERY = (
    "请使用 filesystem 的 read_file 读取 agent/agent_config.h。"
    "只根据文件，用中文列出 max_iterations、max_context_tokens、iteration_timeout_ms "
    "三个字段的默认值，并说明 iteration_timeout_ms=0 的含义。回答不超过120字。"
    "不要读取其他文件。"
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--max-seconds', type=int, default=600)
    parser.add_argument('--executable', default='build/release/cpp-agent')
    parser.add_argument('--no-stream', action='store_true', help='Use provider token usage from non-streaming responses')
    parser.add_argument('--scenario', choices=('code', 'echo'), default='code',
                        help='code sends only agent_config.h; echo sends synthetic text only')
    args = parser.parse_args()
    if not 1 <= args.runs <= 20 or not 1 <= args.max_seconds <= 600:
        parser.error('Use 1..20 runs and a total budget of 1..600 seconds')
    executable = (ROOT / args.executable).resolve()
    sys.path.insert(0, str(ROOT / 'python'))
    import llm_client
    config = json.loads(llm_client.llm_config())
    if not config['configured']:
        parser.error('Configure OPENAI_API_KEY in .env first')
    secrets = [os.environ.get('OPENAI_API_KEY', '')]
    directory = ROOT / 'logs' / 'live-performance' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    directory.mkdir(parents=True)
    tool_root = directory / 'input'
    tool_root.mkdir()
    if args.scenario == 'code':
        (tool_root / 'agent').mkdir()
        shutil.copyfile(ROOT / 'src' / 'agent' / 'agent_config.h', tool_root / 'agent' / 'agent_config.h')
        query = QUERY
    else:
        query = 'Call echo exactly once with message "performance-probe". Then reply briefly confirming the result.'
    manifest = dict(model=config['model'], query=query, streaming=not args.no_stream, scenario=args.scenario,
                    tools=['filesystem'] if args.scenario == 'code' else ['echo'],
                    max_output_tokens=int(os.environ.get('LLM_MAX_OUTPUT_TOKENS', '4096')),
                    thinking_mode=os.environ.get('LLM_THINKING_MODE', 'provider-default'),
                    tool_root=str(tool_root.relative_to(ROOT)),
                    requested_runs=args.runs, max_seconds=args.max_seconds, samples=[])
    started = time.monotonic()
    traces = []
    print(f'Traces: {directory.relative_to(ROOT)}', flush=True)
    for index in range(args.runs):
        remaining = args.max_seconds - (time.monotonic() - started)
        if remaining <= 0:
            break
        trace = directory / f'run-{index + 1:02d}.json'
        command = [str(executable), '--tool-root', str(tool_root),
                   '--tools', 'filesystem' if args.scenario == 'code' else 'echo',
                   '--query', query, '--trace', str(trace)] + ([] if args.no_stream else ['--stream'])
        run_started = time.monotonic()
        try:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace', timeout=min(250, remaining))
            output = result.stdout + result.stderr
            exit_code = result.returncode
        except subprocess.TimeoutExpired:
            output = 'Benchmark process exceeded its wall-clock budget.\n'
            exit_code = 124
        for secret in secrets:
            if secret:
                output = output.replace(secret, '[REDACTED]')
        (directory / f'run-{index + 1:02d}.log').write_text(output, encoding='utf-8')
        sample = dict(index=index + 1, exit_code=exit_code,
                      process_ms=(time.monotonic() - run_started) * 1000)
        if trace.exists():
            data = json.loads(trace.read_text(encoding='utf-8'))
            traces.append(data)
            metrics = data['summary']['performance']
            sample.update(status=metrics['status'], elapsed_ms=metrics['elapsed_ms'],
                          llm_calls=metrics['llm_calls'], tool_calls=metrics['tool_calls'])
        manifest['samples'].append(sample)
        (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding='utf-8')
        print(json.dumps(sample), flush=True)
        if exit_code:
            print('Stopped after failed run; inspect its local trace/log before retrying.', flush=True)
            break
    report = summarize(traces)
    (directory / 'summary.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False), flush=True)
    return 0 if len(manifest['samples']) == args.runs and all(s['exit_code'] == 0 for s in manifest['samples']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
