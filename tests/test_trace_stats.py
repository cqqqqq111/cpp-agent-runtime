import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from trace_stats import distribution, summarize

assert distribution([])['p95'] is None
assert distribution([3]) == dict(count=1, mean=3, p50=3, p95=3)
assert distribution(range(1, 21))['p95'] == 19
assert distribution([10, 20])['p50'] == 15
trace = {'summary': {'performance': dict(elapsed_ms=20, llm_ms=10, tools_wall_ms=5,
         tool_execution_ms=5, tool_queue_ms=0, status='timeout')},
         'steps': [{'request_metrics': dict(duration_ms=10, first_text_ms=None, status='timeout')} ]}
result = summarize([trace, {'summary': {}}])
assert result['metrics']['elapsed_ms']['count'] == 1
assert result['metrics']['first_text_ms']['count'] == 0
assert result['request_statuses'] == {'timeout': 1}
assert result['skipped_legacy'] == 1
print('Percentiles, missing first text, timeout and legacy trace handling passed')
