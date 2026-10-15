"""Local HTTP/SSE integration test; no remote model or credentials required."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import tempfile


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert body["model"] == "local-test-model"
        assert body['max_tokens'] == 256
        assert body['thinking'] == {'type': 'disabled'}
        messages = body["messages"]
        limited = any(m.get('content') == 'limited' for m in messages)
        if any(m.get('content') == 'selected-tools' for m in messages):
            assert [t['function']['name'] for t in body['tools']] == ['echo']
        if any(m.get("content") == "force-api-error" for m in messages):
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"error":{"message":"local test error","type":"invalid_request_error"}}')
            return
        slow = any(m.get("content") == "slow" for m in messages)
        finished = any(m["role"] == "tool" and m.get("content") == "[echo] bridge" for m in messages)
        streaming = body.get("stream", False)
        if slow and not streaming:
            time.sleep(1.2)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream" if streaming else "application/json")
        self.end_headers()
        try:
            if streaming:
                assert body['stream_options'] == {'include_usage': True}
                def emit(delta, finish=None):
                    chunk = {"id": "test", "object": "chat.completion.chunk", "created": 1,
                             "model": "local-test-model", "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
                    self.wfile.write(("data: " + json.dumps(chunk) + "\n\n").encode())
                    self.wfile.flush()
                if limited:
                    emit({'tool_calls': [{'index': 0, 'id': 'partial', 'type': 'function',
                          'function': {'name': 'echo', 'arguments': '{'}}]}, 'length')
                elif slow:
                    emit({"role": "assistant", "content": "partial"})
                    time.sleep(1.2)
                    emit({"content": "late"}, "stop")
                elif finished:
                    emit({"content": "tool pipeline "})
                    emit({"content": "complete"}, "stop")
                else:
                    emit({"tool_calls": [{"index": 0, "id": "call_1", "type": "function",
                          "function": {"name": "echo", "arguments": '{"message":'}}]})
                    emit({"tool_calls": [{"index": 0, "function": {"arguments": '"bridge"}'}}]}, "tool_calls")
                usage = {'id': 'test', 'object': 'chat.completion.chunk', 'created': 1,
                         'model': 'local-test-model', 'choices': [],
                         'usage': {'prompt_tokens': 10, 'completion_tokens': 3, 'total_tokens': 13}}
                if not any(m.get('content') == 'no-usage' for m in messages):
                    self.wfile.write(('data: ' + json.dumps(usage) + '\n\n').encode())
                self.wfile.write(b"data: [DONE]\n\n")
            else:
                message = {"role": "assistant", "content": "tool pipeline complete" if finished else None}
                if not finished:
                    message["tool_calls"] = [{"id": "call_1", "type": "function",
                        "function": {"name": "echo", "arguments": '{"message":"bridge"}'}}]
                if limited:
                    message['tool_calls'][0]['function']['arguments'] = '{'
                response = {"id": "test", "object": "chat.completion", "created": 1, "model": "local-test-model",
                            "choices": [{"index": 0, "message": message, "finish_reason": 'length' if limited else ("stop" if finished else "tool_calls")}],
                            "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}}
                self.wfile.write(json.dumps(response).encode())
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when the client cancels a slow request.


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
env = os.environ.copy()
env.update(OPENAI_API_KEY="local-test-only", OPENAI_BASE_URL=f"http://127.0.0.1:{server.server_port}/v1",
           LLM_MODEL="local-test-model", NO_PROXY="127.0.0.1,localhost",
           LLM_MAX_OUTPUT_TOKENS="256", LLM_THINKING_MODE="disabled")
try:
    subprocess.run([str(Path(sys.argv[1]).resolve())], env=env, check=True, timeout=20)
    if len(sys.argv) > 2:
        executable = str(Path(sys.argv[2]).resolve())
        with tempfile.TemporaryDirectory(prefix='cpp-agent-live-cli-') as directory:
            for streaming in (False, True):
                trace_path = Path(directory) / ('stream.json' if streaming else 'batch.json')
                args = [executable, '--query', 'selected-tools', '--tools', 'echo', '--trace', str(trace_path),
                        '--tool-root', directory] + (['--stream'] if streaming else [])
                run = subprocess.run(args, cwd=directory, env=env, capture_output=True, text=True, timeout=10)
                assert run.returncode == 0, run.stdout + run.stderr
                trace = json.loads(trace_path.read_text())
                assert trace['query']['user_input'] == 'selected-tools'
                assert trace['config']['model'] == 'local-test-model'
                assert trace['summary']['final_answer'] == 'tool pipeline complete'
                assert trace['summary']['performance']['llm_calls'] == 2
                assert trace['summary']['performance']['tool_calls'] == 1
                first_text = trace['steps'][-1]['request_metrics']['first_text_ms']
                assert (first_text is not None) == streaming
                assert trace['summary']['total_tokens'] == (26 if streaming else 4)
                assert trace['steps'][-1]['llm_response']['usage_available']
            for streaming in (False, True):
                trace_path = Path(directory) / 'limited.json'
                run = subprocess.run([executable, '--query', 'limited', '--trace', str(trace_path)] +
                                     (['--stream'] if streaming else []), cwd=directory, env=env,
                                     capture_output=True, text=True, timeout=10)
                trace = json.loads(trace_path.read_text())
                assert run.returncode == 1
                assert trace['summary']['performance']['status'] == 'output_limit'
                assert trace['summary']['performance']['tool_calls'] == 0
            trace_path = Path(directory) / 'no-usage.json'
            run = subprocess.run([executable, '--stream', '--query', 'no-usage', '--trace', str(trace_path)],
                                 cwd=directory, env=env, capture_output=True, text=True, timeout=10)
            assert run.returncode == 0
            trace = json.loads(trace_path.read_text())
            assert trace['summary']['total_tokens'] == 0
            assert trace['steps'][-1]['llm_response']['usage_available'] is False
            assert trace['steps'][-1]['llm_response']['completion_tokens'] == 0
            for selection in ('unknown', '', 'echo,', 'echo,,grep'):
                run = subprocess.run([executable, '--tools', selection], cwd=directory, env=env,
                                     capture_output=True, text=True, timeout=10)
                assert run.returncode == 1
            trace_path = Path(directory) / 'error.json'
            run = subprocess.run([executable, '--query', 'force-api-error', '--trace', str(trace_path)],
                                 cwd=directory, env=env, capture_output=True, text=True, timeout=10)
            assert run.returncode == 1
            assert json.loads(trace_path.read_text())['summary']['performance']['status'] == 'error'
        print('Real CLI query, streaming, trace and API error handling passed against local HTTP server')
finally:
    server.shutdown()
    server.server_close()
    thread.join()
