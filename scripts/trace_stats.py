"""Performance trace summaries; nearest-rank P95, no interpolation."""
import argparse
from collections import Counter, defaultdict
import glob
import json
import math
from pathlib import Path
import statistics


def distribution(values):
    values = sorted(values)
    if not values:
        return dict(count=0, mean=None, p50=None, p95=None)
    return dict(count=len(values), mean=statistics.mean(values), p50=statistics.median(values),
                p95=values[math.ceil(.95 * len(values)) - 1])


def summarize(traces):
    samples = defaultdict(list)
    statuses = Counter()
    request_statuses = Counter()
    tools = defaultdict(lambda: dict(queue=[], execution=[], failures=0, input_bytes=0, output_bytes=0))
    skipped = 0
    for trace in traces:
        perf = trace.get('summary', {}).get('performance')
        if not perf:
            skipped += 1
            continue
        for key in ('elapsed_ms', 'llm_ms', 'tools_wall_ms', 'tool_execution_ms', 'tool_queue_ms'):
            samples[key].append(perf[key])
        statuses[perf['status']] += 1
        for step in trace.get('steps', []):
            request = step.get('request_metrics')
            if request:
                samples['request_ms'].append(request['duration_ms'])
                request_statuses[request['status']] += 1
                if request['first_text_ms'] is not None:
                    samples['first_text_ms'].append(request['first_text_ms'])
            for metric in step.get('tool_metrics', []):
                tool = tools[metric['name']]
                tool['queue'].append(metric['queue_ms'])
                tool['execution'].append(metric['execution_ms'])
                tool['failures'] += not metric['success']
                tool['input_bytes'] += metric['input_bytes']
                tool['output_bytes'] += metric['output_bytes']
    return dict(
        metrics={key: distribution(samples[key]) for key in ('elapsed_ms', 'llm_ms', 'tools_wall_ms',
                 'tool_execution_ms', 'tool_queue_ms', 'request_ms', 'first_text_ms')},
        statuses=dict(statuses), request_statuses=dict(request_statuses), skipped_legacy=skipped,
        tools={name: {**metric, 'queue': distribution(metric['queue']),
                      'execution': distribution(metric['execution'])} for name, metric in sorted(tools.items())})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('traces', nargs='+', help='Trace JSON files or glob patterns')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    paths = sorted({path for pattern in args.traces for path in glob.glob(pattern)})
    if not paths:
        parser.error('No matching files')
    report = summarize([json.loads(Path(path).read_text(encoding='utf-8')) for path in paths])
    if not report['metrics']['elapsed_ms']['count']:
        parser.error('No performance metrics found; schema 1.1 traces required')
    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
        return
    print(f"Run status: {report['statuses']} | Skipped legacy: {report['skipped_legacy']}")
    print(f"Request status: {report['request_statuses']}")
    print(f"{'Metric (ms)':36} {'Count':>6} {'Mean':>10} {'P50':>10} {'P95':>10}")
    def row(name, stats):
        values = [f"{stats[k]:.3f}" if stats[k] is not None else 'N/A' for k in ('mean', 'p50', 'p95')]
        print(f"{name:36} {stats['count']:6} " + ' '.join(f'{value:>10}' for value in values))
    for key, metric in report['metrics'].items():
        row(key, metric)
    for name, metric in report['tools'].items():
        row(name + ' / queue', metric['queue'])
        row(name + ' / execution', metric['execution'])
        print(f"  failures={metric['failures']}, input={metric['input_bytes']} B, output={metric['output_bytes']} B")


if __name__ == '__main__':
    main()
