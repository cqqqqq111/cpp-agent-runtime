"""
LLM Client: thin wrapper over OpenAI-compatible APIs.

The C++ runtime calls into this module via pybind11. This file is
purposefully kept simple — all orchestration logic lives in C++.
"""

import os
import sys
from typing import Any

from openai import OpenAI


# ---------------------------------------------------------------------------
# .env loading (no external dependency)
# ---------------------------------------------------------------------------

def _load_dotenv() -> None:
    """Load a `.env` file into os.environ, if present.

    The embedded Python interpreter may run with CWD = build/, so we search
    a few likely locations. Existing environment variables take precedence
    over `.env` values (setdefault).
    """
    here = os.path.dirname(os.path.abspath(__file__))  # .../python
    candidates = [
        os.path.join(os.path.dirname(here), ".env"),   # project root
        os.path.join(os.getcwd(), ".env"),
        os.path.join(os.getcwd(), "..", ".env"),
    ]

    for path in candidates:
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    key, _, value = line.partition("=")
                    key = key.strip()
                    value = value.strip().strip('"').strip("'")
                    if key:
                        os.environ.setdefault(key, value)
        except OSError:
            continue
        return  # first found wins


_load_dotenv()

# Default model — DeepSeek / OpenAI-compatible providers override via .env.
DEFAULT_MODEL = os.environ.get("LLM_MODEL", "gpt-4o-mini")


class LLMClient:
    """Minimal OpenAI-compatible chat client for the C++ agent runtime."""

    def __init__(
        self,
        api_key: str | None = None,
        base_url: str | None = None,
        model: str = DEFAULT_MODEL,
    ):
        self._api_key = api_key or os.environ.get("OPENAI_API_KEY", "")
        self._base_url = base_url or os.environ.get("OPENAI_BASE_URL")
        self._model = model
        self._client: OpenAI | None = None

    @property
    def client(self) -> OpenAI:
        if self._client is None:
            kwargs: dict[str, Any] = {"api_key": self._api_key}
            if self._base_url:
                kwargs["base_url"] = self._base_url
            self._client = OpenAI(**kwargs)
        return self._client

    def chat(
        self,
        messages: list[dict[str, Any]],
        tools: list[dict[str, Any]] | None = None,
        *,
        temperature: float = 0.2,
        max_tokens: int = 4096,
    ) -> dict[str, Any]:
        """
        Send a chat completion request.

        Args:
            messages: List of {"role": ..., "content": ...} dicts.
            tools: Optional list of OpenAI tool definitions.
            temperature: Sampling temperature.
            max_tokens: Max tokens in the response.

        Returns:
            A dict with keys:
              - "content": str | None
              - "tool_calls": list[dict] | None
              - "finish_reason": str
              - "usage": dict (prompt_tokens, completion_tokens, total_tokens)
              - "model": str
        """
        kwargs: dict[str, Any] = {
            "model": self._model,
            "messages": messages,
            "temperature": temperature,
            "max_tokens": max_tokens,
        }
        if tools:
            kwargs["tools"] = tools

        response = self.client.chat.completions.create(**kwargs)
        choice = response.choices[0]
        msg = choice.message

        result: dict[str, Any] = {
            "content": msg.content,
            "tool_calls": None,
            "finish_reason": choice.finish_reason,
            "usage": {
                "prompt_tokens": response.usage.prompt_tokens if response.usage else 0,
                "completion_tokens": response.usage.completion_tokens if response.usage else 0,
                "total_tokens": response.usage.total_tokens if response.usage else 0,
            },
            "model": response.model,
        }

        if msg.tool_calls:
            result["tool_calls"] = [
                {
                    "id": tc.id,
                    "type": tc.type,
                    "function": {
                        "name": tc.function.name,
                        "arguments": tc.function.arguments,
                    },
                }
                for tc in msg.tool_calls
            ]

        return result

    def chat_streaming(
        self,
        messages: list[dict[str, Any]],
        tools: list[dict[str, Any]] | None = None,
        *,
        temperature: float = 0.2,
        max_tokens: int = 4096,
    ):
        """
        Stream chat completion tokens.

        Yields each token delta as a dict: {"type": "content"|"tool_call", "data": ...}
        """
        kwargs: dict[str, Any] = {
            "model": self._model,
            "messages": messages,
            "temperature": temperature,
            "max_tokens": max_tokens,
            "stream": True,
        }
        if tools:
            kwargs["tools"] = tools

        stream = self.client.chat.completions.create(**kwargs)
        for chunk in stream:
            delta = chunk.choices[0].delta if chunk.choices else None
            if delta is None:
                continue
            if delta.content:
                yield {"type": "content", "data": delta.content}
            if delta.tool_calls:
                for tc in delta.tool_calls:
                    yield {
                        "type": "tool_call",
                        "data": {
                            "index": tc.index,
                            "id": tc.id,
                            "function_name": tc.function.name if tc.function else None,
                            "arguments": tc.function.arguments if tc.function else None,
                        },
                    }


# ---------------------------------------------------------------------------
# Module-level convenience functions (called from C++ via pybind11)
# ---------------------------------------------------------------------------

_default_client: LLMClient | None = None


def _get_client() -> LLMClient:
    global _default_client
    if _default_client is None:
        _default_client = LLMClient()
    return _default_client


def call_llm(
    prompt: str,
    *,
    model: str = DEFAULT_MODEL,
    system_prompt: str | None = None,
) -> str:
    """
    Simple single-turn LLM call. Returns the response text.

    This is the primary entry point from C++ during Day 1 scaffolding.
    Later days will use the full chat() interface with multi-turn
    messages and tool definitions.
    """
    messages: list[dict[str, Any]] = []
    if system_prompt:
        messages.append({"role": "system", "content": system_prompt})
    messages.append({"role": "user", "content": prompt})

    client = _get_client()
    client._model = model  # allow override per-call
    result = client.chat(messages)
    return result.get("content", "")


def call_llm_streaming(prompt: str, *, model: str = DEFAULT_MODEL) -> str:
    """Streaming variant — accumulates and returns full response."""
    messages = [{"role": "user", "content": prompt}]
    client = _get_client()
    client._model = model
    parts: list[str] = []
    for token in client.chat_streaming(messages):
        if token["type"] == "content":
            parts.append(token["data"])
    return "".join(parts)


# ---------------------------------------------------------------------------
# Day 4: Full multi-turn + tools API (the real agent interface)
# ---------------------------------------------------------------------------

def call_llm_messages(
    messages_json: str,
    tools_json: str = "[]",
    *,
    model: str = DEFAULT_MODEL,
) -> str:
    """
    Full agent LLM call. Takes JSON-encoded messages and tools, returns
    a JSON response dict with keys: content, tool_calls, finish_reason,
    usage, model.

    Args:
        messages_json: JSON array of {"role":..., "content":..., ...} objects.
        tools_json: JSON array of OpenAI tool definitions (or "[]").
        model: Model name override.

    Returns:
        JSON string of the full response dict (see LLMClient.chat return format).
    """
    import json

    try:
        messages = json.loads(messages_json)
        tools = json.loads(tools_json) if tools_json and tools_json != "[]" else None
    except json.JSONDecodeError as e:
        return json.dumps({
            "content": None,
            "tool_calls": None,
            "finish_reason": "error",
            "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
            "model": model,
            "_error": f"JSON decode error: {e}",
        })

    client = _get_client()
    client._model = model
    result = client.chat(messages, tools)
    return json.dumps(result)


# ---------------------------------------------------------------------------
# Day 11: Streaming multi-turn + tools
# ---------------------------------------------------------------------------

def call_llm_messages_streaming(
    messages_json: str,
    tools_json: str = "[]",
    *,
    model: str = DEFAULT_MODEL,
    on_token: "typing.Callable[[str], None] | None" = None,
    on_complete: "typing.Callable[[str], None] | None" = None,
) -> None:
    """
    Streaming variant that calls on_token(content) for each text delta.
    When the stream ends, calls on_complete(final_content).

    Args:
        messages_json: JSON array of messages.
        tools_json: JSON array of tool definitions.
        model: Model name.
        on_token: Called with each content token as it arrives.
        on_complete: Called with the accumulated full content when done.
    """
    import typing  # noqa: F811

    try:
        messages = json.loads(messages_json)
        tools = json.loads(tools_json) if tools_json and tools_json != "[]" else None
    except json.JSONDecodeError as e:
        if on_complete:
            on_complete(json.dumps({
                "content": None, "tool_calls": None, "finish_reason": "error",
                "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
                "model": model, "_error": f"JSON decode error: {e}",
            }))
        return

    client = _get_client()
    client._model = model

    full_content: list[str] = []
    tool_call_buf: dict[int, dict[str, str]] = {}  # Day 19: accumulate tool_call deltas
    stream = client.chat_streaming(messages, tools)

    for chunk in stream:
        if chunk["type"] == "content":
            delta = chunk["data"]
            full_content.append(delta)
            if on_token:
                on_token(delta)
        elif chunk["type"] == "tool_call":
            # Day 19: OpenAI streams tool calls incrementally — id/name arrive
            # on the first delta, arguments are split across chunks. Accumulate
            # by index so the final response matches the non-streaming shape.
            data = chunk["data"]
            idx = data["index"]
            buf = tool_call_buf.setdefault(idx, {"id": "", "function_name": "", "arguments": ""})
            if data.get("id"):
                buf["id"] = data["id"]
            if data.get("function_name"):
                buf["function_name"] = data["function_name"]
            if data.get("arguments"):
                buf["arguments"] += data["arguments"]

    # Day 19: assemble accumulated tool calls into the same shape as chat().
    tool_calls = None
    if tool_call_buf:
        tool_calls = [
            {
                "id": b["id"],
                "type": "function",
                "function": {"name": b["function_name"], "arguments": b["arguments"]},
            }
            for _, b in sorted(tool_call_buf.items())
        ]

    # Build final response
    final = {
        "content": "".join(full_content),
        "tool_calls": tool_calls,
        "finish_reason": "tool_calls" if tool_calls else "stop",
        "usage": {"prompt_tokens": 0, "completion_tokens": len(full_content), "total_tokens": 0},
        "model": model,
    }
    if on_complete:
        on_complete(json.dumps(final))


# ---------------------------------------------------------------------------
# Day 9: Embedding API
# ---------------------------------------------------------------------------

def call_embedding(text: str, *, model: str = "text-embedding-3-small") -> list[float]:
    """
    Convert text to an embedding vector.

    Args:
        text: The input text to embed.
        model: Embedding model name.

    Returns:
        List of float values (1536 for text-embedding-3-small).
    """
    client = _get_client()
    resp = client.client.embeddings.create(input=text, model=model)
    return resp.data[0].embedding


# ---------------------------------------------------------------------------
# Day 17: expose config status to the C++ runtime
# ---------------------------------------------------------------------------

def llm_config() -> str:
    """Return JSON {"configured": bool, "model": str} for the C++ side.

    The C++ TUI uses this to decide between the real LLM and DemoLLM
    without needing to parse `.env` itself (that happens on the Python side).
    """
    import json as _json
    return _json.dumps({
        "configured": bool(os.environ.get("OPENAI_API_KEY", "").strip()),
        "model": DEFAULT_MODEL,
    })


# ---------------------------------------------------------------------------
# Self-test (run with: python llm_client.py)
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    api_key = os.environ.get("OPENAI_API_KEY", "")
    if not api_key:
        print("[SKIP] OPENAI_API_KEY not set. Set it to run a live test.")
        sys.exit(0)

    print("=== LLM Client Self-Test ===")
    client = LLMClient()
    try:
        reply = call_llm("Say 'cpp-agent-runtime is alive!' in exactly 5 words.")
        print(f"  Response: {reply}")
        print("  [PASS] LLM client is working.\n")
    except Exception as e:
        print(f"  [FAIL] LLM call failed: {e}")
        sys.exit(1)
