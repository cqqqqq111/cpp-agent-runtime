/**
 * @file py_bridge.cpp
 * @brief pybind11 embedded-Python bridge.
 *
 * This is THE critical integration point. The C++ agent runtime calls Python
 * for LLM / embedding work and gets results back. Everything else — scheduling,
 * tool execution, memory, TUI — stays in C++.
 *
 * Architecture note (Day 1):
 *   C++ main.cpp
 *     └─ call_llm_bridge(prompt)       ← free function, usable anywhere
 *          └─ py::module::import("python.llm_client")
 *               └─ call_llm(prompt)    ← Python function
 *                    └─ openai.chat.completions.create(...)
 *
 * This simple chain will evolve Day 11+ to support:
 *   - Multi-turn messages (structured JSON across the bridge)
 *   - Streaming (SSE → C++ callback via pybind11)
 *   - Connection pooling (reuse the Python interpreter)
 */

#include "llm/py_bridge.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace py = pybind11;

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

/// The embedded Python interpreter lives for the whole process lifetime.
/// We store handles to the call_llm / call_llm_streaming functions so we
/// don't re-import the module on every call.
namespace {

/// Guard that owns the interpreter lifecycle.
std::unique_ptr<py::scoped_interpreter> g_interpreter = nullptr;

/// Cached callable — avoids repeated module lookups.
py::object g_call_llm = {};
py::object g_call_llm_streaming = {};
py::object g_call_llm_messages = {};  // Day 4: multi-turn + tools
py::object g_call_embedding = {};      // Day 9: text → vector
py::object g_call_streaming_msgs = {}; // Day 11: streaming multi-turn
py::object g_llm_config = {};          // Day 17: config status

/// Have we successfully initialized?
bool g_initialized = false;

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int init_python_bridge() {
    if (g_initialized) {
        spdlog::warn("init_python_bridge: already initialized, skipping.");
        return 0;
    }

    try {
        spdlog::debug("Starting embedded Python interpreter...");

        // Start the interpreter. pybind11 only allows ONE scoped_interpreter
        // per process; we hold it for the entire lifetime.
        g_interpreter = std::make_unique<py::scoped_interpreter>();

        // Add the project's python/ directory to sys.path so we can import
        // our llm_client module regardless of CWD.
        py::module_ sys = py::module_::import("sys");
        py::list path = sys.attr("path");

        // Try build directory first, then source directory as fallback
        path.attr("insert")(0, "./python");
        path.attr("insert")(0, "../python");

        // Import our Python module and cache the callables.
        py::module_ llm = py::module_::import("python.llm_client");
        g_call_llm = llm.attr("call_llm");
        g_call_llm_streaming = llm.attr("call_llm_streaming");
        g_call_llm_messages = llm.attr("call_llm_messages");
        g_call_embedding = llm.attr("call_embedding");
        g_call_streaming_msgs = llm.attr("call_llm_messages_streaming");
        g_llm_config = llm.attr("llm_config");

        g_initialized = true;
        spdlog::info("Python bridge initialized (found python.llm_client).");
        return 0;

    } catch (const py::error_already_set& e) {
        spdlog::critical("Python bridge init failed: {}", e.what());
        return 1;
    } catch (const std::exception& e) {
        spdlog::critical("Python bridge init failed: {}", e.what());
        return 1;
    }
}

void shutdown_python_bridge() {
    if (!g_initialized) return;

    spdlog::debug("Shutting down Python bridge...");

    // Release Python object handles before destroying the interpreter.
    g_call_llm = {};
    g_call_llm_streaming = {};
    g_call_llm_messages = {};
    g_call_embedding = {};
    g_call_streaming_msgs = {};
    g_llm_config = {};

    // Destroy the interpreter.
    g_interpreter.reset();
    g_initialized = false;

    spdlog::info("Python bridge shut down.");
}

std::string call_llm_bridge(const std::string& prompt) {
    if (!g_initialized) {
        throw std::runtime_error(
            "Python bridge not initialized. Call init_python_bridge() first.");
    }

    try {
        // Release the GIL during the network call (non-blocking for other threads
        // that might need Python).
        py::gil_scoped_release release;
        // Wait — we need the GIL to CALL the function. Let pybind11 manage it.
        // Actually, since gil_scoped_release releases the GIL, we need to
        // re-acquire before calling Python. The pattern is: release GIL,
        // do C++ work (none here), re-acquire, call Python.
    } catch (...) {
        // scope guard: gil_scoped_release destructor re-acquires GIL
    }

    try {
        // Call Python's call_llm(prompt) -> str
        py::gil_scoped_acquire gil;
        py::object result = g_call_llm(prompt);
        return result.cast<std::string>();

    } catch (const py::error_already_set& e) {
        std::string msg = std::string("Python LLM call failed: ") + e.what();
        spdlog::error("{}", msg);
        throw std::runtime_error(msg);
    }
}

std::string call_llm_streaming_bridge(const std::string& prompt) {
    if (!g_initialized) {
        throw std::runtime_error(
            "Python bridge not initialized. Call init_python_bridge() first.");
    }

    try {
        py::gil_scoped_acquire gil;
        py::object result = g_call_llm_streaming(prompt);
        return result.cast<std::string>();

    } catch (const py::error_already_set& e) {
        std::string msg = std::string("Python streaming LLM call failed: ") + e.what();
        spdlog::error("{}", msg);
        throw std::runtime_error(msg);
    }
}

std::string call_llm_messages_bridge(const std::string& messages_json,
                                     const std::string& tools_json) {
    if (!g_initialized) {
        throw std::runtime_error(
            "Python bridge not initialized. Call init_python_bridge() first.");
    }

    try {
        py::gil_scoped_acquire gil;
        py::object result = g_call_llm_messages(messages_json, tools_json);
        return result.cast<std::string>();

    } catch (const py::error_already_set& e) {
        std::string msg = std::string("Python multi-turn LLM call failed: ") + e.what();
        spdlog::error("{}", msg);
        throw std::runtime_error(msg);
    }
}

std::string call_embedding_bridge(const std::string& text) {
    if (!g_initialized) {
        throw std::runtime_error(
            "Python bridge not initialized. Call init_python_bridge() first.");
    }

    try {
        py::gil_scoped_acquire gil;
        py::object result = g_call_embedding(text);

        // Convert Python list to JSON array string
        py::list vec = result.cast<py::list>();
        py::object json_mod = py::module_::import("json");
        py::object dumps = json_mod.attr("dumps");
        return dumps(vec).cast<std::string>();

    } catch (const py::error_already_set& e) {
        std::string msg = std::string("Python embedding call failed: ") + e.what();
        spdlog::error("{}", msg);
        throw std::runtime_error(msg);
    }
}

void call_llm_streaming_bridge(
    const std::string& messages_json,
    const std::string& tools_json,
    const std::function<void(const std::string&)>& on_token,
    const std::function<void(const std::string&)>& on_complete) {
    if (!g_initialized) throw std::runtime_error("Python bridge not initialized.");

    try {
        py::gil_scoped_acquire gil;
        py::cpp_function py_on_token(
            [on_token](const std::string& token) { on_token(token); });
        py::cpp_function py_on_complete(
            [on_complete](const std::string& result) { on_complete(result); });

        g_call_streaming_msgs(
            messages_json, tools_json,
            py::arg("on_token") = py_on_token,
            py::arg("on_complete") = py_on_complete);
    } catch (const py::error_already_set& e) {
        std::string msg = std::string("Python streaming call failed: ") + e.what();
        spdlog::error("{}", msg);
        throw std::runtime_error(msg);
    }
}

std::string llm_config_bridge() {
    if (!g_initialized) {
        throw std::runtime_error(
            "Python bridge not initialized. Call init_python_bridge() first.");
    }

    try {
        py::gil_scoped_acquire gil;
        py::object result = g_llm_config();
        return result.cast<std::string>();
    } catch (const py::error_already_set& e) {
        std::string msg = std::string("llm_config call failed: ") + e.what();
        spdlog::error("{}", msg);
        throw std::runtime_error(msg);
    }
}
