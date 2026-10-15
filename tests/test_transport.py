"""Connection reuse, cancellation, lease limits and cleanup, all on localhost."""
from concurrent.futures import ThreadPoolExecutor
import contextvars
import json
import os
from pathlib import Path
import sys
import subprocess
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from transport_server import TransportServer

for key in list(os.environ):
    if key.lower() in ('http_proxy', 'https_proxy', 'all_proxy', 'no_proxy'):
        os.environ.pop(key)
os.environ.update(OPENAI_API_KEY='local-test-only', OPENAI_BASE_URL='http://127.0.0.1:1/v1',
                  LLM_MODEL='local-model', LLM_MAX_SESSIONS='2', LLM_MAX_OUTPUT_TOKENS='256',
                  LLM_THINKING_MODE='disabled')
import llm_client as llm


def request(client, text='fast', timeout=2000, callback=None, model='local-model'):
    return llm._request(client, [{'role': 'user', 'content': text}], [], timeout,
                        on_chunk=callback, model=model)


def expect_error(exception, function):
    try:
        function()
    except exception:
        return
    raise AssertionError('Expected ' + exception.__name__)


with TransportServer() as server:
    client = llm.LLMClient(api_key='local-test-only', base_url=server.url)
    # Normal and SSE requests share the same connection. Callback affinity is
    # unchanged even if the same idle session is next leased by another thread.
    for index in range(10):
        caller = threading.get_ident()
        def check_thread(chunk):
            assert threading.get_ident() == caller
        request(client, callback=check_thread if index % 2 else None)
    assert server.connections == 1, server.connections
    with ThreadPoolExecutor(max_workers=1) as workers:
        assert workers.submit(request, client).result().choices[0].message.content == 'local-model'
    assert server.connections == 1
    # Context and callback thread must follow the current lease holder.
    marker = contextvars.ContextVar('caller')
    def stream_on_thread(index):
        marker.set(index)
        caller = threading.get_ident()
        chunks = []
        def receive(chunk):
            assert threading.get_ident() == caller
            assert marker.get() == index
            if chunk.choices and chunk.choices[0].delta.content:
                chunks.append(chunk.choices[0].delta.content)
        request(client, callback=receive, model=f'stream-{index}')
        return ''.join(chunks)
    with ThreadPoolExecutor(max_workers=4) as workers:
        assert list(workers.map(stream_on_thread, range(8))) == [f'stream-{i}' for i in range(8)]

    # A rejected request does not silently trigger a paid retry or poison the
    # reusable connection. Verify both permanent and temporary HTTP errors.
    for status in ('401', '503'):
        before = len(server.requests)
        from openai import APIStatusError
        expect_error(APIStatusError, lambda: request(client, text=status))
        assert len(server.requests) == before + 1
        assert request(client).choices[0].message.content == 'local-model'

    # Timeouts cancel/close the response before returning. No stream callback
    # can escape into the next agent lifetime.
    for streaming in (False, True):
        chunks = []
        started = time.monotonic()
        expect_error(TimeoutError, lambda: request(client, 'slow', 80,
                     (lambda chunk: chunks.append(chunk)) if streaming else None))
        assert time.monotonic() - started < .6
        count = len(chunks)
        assert request(client).choices[0].message.content == 'local-model'
        time.sleep(.4)
        assert len(chunks) == count

    def bad_callback(chunk):
        raise ValueError('caller callback failed')
    expect_error(ValueError, lambda: request(client, callback=bad_callback))
    assert request(client).choices[0].message.content == 'local-model'
    before = len(server.requests)
    expect_error(ValueError, lambda: request(client, timeout=-1))
    assert len(server.requests) == before

    # Multiple real concurrent callers keep independent request/model options,
    # and never exceed the configured two active transport sessions.
    with server.lock:
        server.peak_requests = 0
    with ThreadPoolExecutor(max_workers=6) as workers:
        tasks = [workers.submit(request, client, 'delay', 3000, None, f'model-{i}') for i in range(6)]
        assert [f.result().choices[0].message.content for f in tasks] == [f'model-{i}' for i in range(6)]
    assert server.peak_requests == 2, server.peak_requests

    # Queue wait consumes the *same* deadline; an expired queued request must
    # never be sent after a session becomes free.
    with ThreadPoolExecutor(max_workers=3) as workers:
        held = [workers.submit(request, client, 'hold', 3000) for _ in range(2)]
        with server.lock:
            assert server.lock.wait_for(lambda: server.active_requests == 2, timeout=2)
        before = len(server.requests)
        expect_error(TimeoutError, lambda: request(client, timeout=50))
        assert len(server.requests) == before
        # Close waits for in-flight requests and rejects new leases.
        closing = workers.submit(client.close)
        time.sleep(.03)
        assert not closing.done()
        server.release_requests.set()
        for future in held:
            future.result()
        closing.result(timeout=3)
    client.close()  # idempotent
    expect_error(RuntimeError, lambda: request(client))
    with server.lock:
        assert server.lock.wait_for(lambda: server.open_connections == 0, timeout=2)

    # The module-level API also isolates model arguments under concurrency;
    # this catches mutation of _default_client._model before acquiring a lease.
    os.environ['OPENAI_BASE_URL'] = server.url
    with ThreadPoolExecutor(max_workers=4) as workers:
        tasks = [workers.submit(llm.call_llm_messages,
            json.dumps([{'role': 'user', 'content': 'delay'}]), model=f'agent-{i}', timeout_ms=3000)
            for i in range(4)]
        assert [json.loads(f.result())['content'] for f in tasks] == [f'agent-{i}' for i in range(4)]
    llm.shutdown_llm_clients()
    assert json.loads(llm.call_llm_messages('[{"role":"user","content":"fast"}]',
                      model='after-reinit'))['content'] == 'after-reinit'
    llm.shutdown_llm_clients()

    # Recursive requests/shutdown from a stream callback must fail promptly,
    # rather than deadlock trying to run an already-running event loop.
    nested = llm.LLMClient(api_key='local-test-only', base_url=server.url)
    expect_error(RuntimeError, lambda: request(nested, callback=lambda chunk: request(nested)))
    expect_error(RuntimeError, lambda: request(nested, callback=lambda chunk: nested.close()))
    assert request(nested).choices[0].message.content == 'local-model'
    nested.close()

    # Standalone Python users also get cleanup at interpreter exit, including
    # the resolver executor needed for a hostname rather than a numeric IP.
    environment = os.environ.copy()
    environment['OPENAI_BASE_URL'] = server.url.replace('127.0.0.1', 'localhost')
    environment['PYTHONPATH'] = str(Path(__file__).resolve().parents[1] / 'python')
    process = subprocess.run([sys.executable, '-W', 'error::ResourceWarning', '-c',
        'import llm_client; print(llm_client.call_llm("fast", model="exit-test"))'],
        env=environment, capture_output=True, text=True, timeout=5)
    assert process.returncode == 0, process.stderr
    assert process.stdout.strip() == 'exit-test'
    assert not process.stderr, process.stderr

print('Reuse, SSE, callback affinity/errors, timeouts, bounded concurrency, model isolation and shutdown passed')
