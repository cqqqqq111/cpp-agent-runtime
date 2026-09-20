"""
cpp-agent-runtime: Python LLM adapter layer.

This module serves as a thin glue layer between the C++ agent runtime
and various LLM providers (OpenAI, Anthropic, local models). The heavy
lifting — scheduling, tool execution, memory management — happens in C++.
"""

from .llm_client import LLMClient, call_llm, call_llm_streaming

__all__ = ["LLMClient", "call_llm", "call_llm_streaming"]
