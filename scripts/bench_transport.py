"""Local-only HTTP connection reuse benchmark; never reads production credentials."""
import argparse
import json
import os
from pathlib import Path
import statistics
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'python'))
from transport_server import TransportServer

# Override before llm_client loads .env. Every client also receives explicit
# localhost credentials/address; no request can use the real account here.
os.environ.update(OPENAI_API_KEY='local-test-only', LLM_MODEL='local-model',
                  OPENAI_BASE_URL='http://127.0.0.1:1/v1', NO_PROXY='127.0.0.1,localhost')
for key in list(os.environ):
    if key.lower() in ('http_proxy', 'https_proxy', 'all_proxy', 'no_proxy'):
        os.environ.pop(key)
import llm_client


def legacy_request(client, streaming):
    # Reproduce the former lifecycle as a control, using the same current SDK.
    import asyncio
    async def request():
        async with llm_client.AsyncOpenAI(api_key=client._api_key, base_url=client._base_url,
                                          timeout=60.0, max_retries=0) as transport:
            kwargs = dict(model='local-model', messages=[{'role': 'user', 'content': 'fast'}],
                          max_tokens=256, temperature=.2)
            if not streaming:
                return await transport.chat.completions.create(**kwargs)
            stream = await transport.chat.completions.create(**kwargs, stream=True,
                stream_options={'include_usage': True})
            try:
                async for chunk in stream:
                    pass
            finally:
                await stream.close()
    async def bounded():
        return await asyncio.wait_for(request(), 3)
    return asyncio.run(bounded())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runs', type=int, default=20)
    parser.add_argument('--legacy', action='store_true', help='Reproduce old per-request client/event-loop lifecycle')
    args = parser.parse_args()
    if not 2 <= args.runs <= 200:
        parser.error('runs must be 2..200')
    results = []
    for delay in (0, .01):
        for reuse in (False, True):
            with TransportServer(delay) as server:
                client = None
                samples = []
                for index in range(args.runs):
                    started = time.perf_counter()
                    if client is None:
                        client = llm_client.LLMClient(api_key='local-test-only', base_url=server.url,
                                                      model='local-model')
                    if args.legacy:
                        legacy_request(client, bool(index % 2))
                    else:
                        llm_client._request(client, [{'role': 'user', 'content': 'fast'}], [],
                                            timeout_ms=3000, on_chunk=(lambda chunk: None) if index % 2 else None)
                    if not reuse:
                        if hasattr(client, 'close'):
                            client.close()
                        client = None
                    samples.append((time.perf_counter() - started) * 1000)
                if client is not None and hasattr(client, 'close'):
                    client.close()
                results.append(dict(legacy=args.legacy, reuse=reuse, simulated_connection_ms=delay * 1000,
                    requests=len(server.requests), connections=server.connections,
                    mean_ms=statistics.mean(samples), p50_ms=statistics.median(samples),
                    first_ms=samples[0], warm_p50_ms=statistics.median(samples[1:])))
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
