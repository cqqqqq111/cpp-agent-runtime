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
#include "llm/llm_adapter.h"

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
// Release the initialization thread's GIL so TUI worker threads can call Python.
std::unique_ptr<py::gil_scoped_release> g_gil_release;

/// Cached callable — avoids repeated module lookups.
py::object g_call_llm = {};
py::object g_call_llm_streaming = {};
py::object g_call_llm_messages = {};  // Day 4: multi-turn + tools
py::object g_call_embedding = {};      // Day 9: text → vector
py::object g_call_streaming_msgs = {}; // Day 11: streaming multi-turn
py::object g_llm_config = {};          // Day 17: config status
py::object g_shutdown_clients = {};

/// Have we successfully initialized?
bool g_initialized = false;
bool g_finalized = false;

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int init_python_bridge() {
    // Native SDK dependencies need a process-lifetime interpreter. Reject a
    // second initialization instead of reentering extensions after finalization.
    if (g_finalized) {
        spdlog::error("Python bridge cannot be restarted after shutdown; start a new process.");
        return -1;
    }
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
        path.attr("insert")(0, CPP_AGENT_SOURCE_DIR);
        // The embedded interpreter does not automatically use the build venv.
        py::module_ site = py::module_::import("site");
        site.attr("addsitedir")(CPP_AGENT_PYTHON_SITE);

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
        g_shutdown_clients = llm.attr("shutdown_llm_clients");

        // Release only after local py::object instances above leave scope.
        g_initialized = true;
        spdlog::info("Python bridge initialized (found python.llm_client).");
        // The release is installed below, after initialization locals are destroyed.

    } catch (const py::error_already_set& e) {
        spdlog::critical("Python bridge init failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::critical("Python bridge init failed: {}", e.what());
    }
    if (!g_initialized) { shutdown_python_bridge(); return 1; }
    g_gil_release = std::make_unique<py::gil_scoped_release>();
    return 0;
}

void shutdown_python_bridge() {
    if (!g_interpreter) return;

    spdlog::debug("Shutting down Python bridge...");
    g_gil_release.reset();

    // Client sockets and event loops must close while Python is still alive.
    // Callers join agent threads before shutting down the interpreter.
    try {
        if (g_shutdown_clients) g_shutdown_clients();
    } catch (const py::error_already_set& e) {
        spdlog::error("LLM transport shutdown failed: {}", e.what());
    }

    // Release Python object handles before destroying the interpreter.
    g_call_llm = {};
    g_call_llm_streaming = {};
    g_call_llm_messages = {};
    g_call_embedding = {};
    g_call_streaming_msgs = {};
    g_llm_config = {};
    g_shutdown_clients = {};

    // Destroy the interpreter.
    g_interpreter.reset();
    g_initialized = false;
    g_finalized = true;

    spdlog::info("Python bridge shut down.");
}

std::string call_llm_bridge(const std::string& prompt) {
    if (!g_initialized) {
        throw std::runtime_error(
            "Python bridge not initialized. Call init_python_bridge() first.");
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
                                     const std::string& tools_json,
                                     int timeout_ms, const std::string& model) {
    if (!g_initialized) {
        throw std::runtime_error(
            "Python bridge not initialized. Call init_python_bridge() first.");
    }

    try {
        py::gil_scoped_acquire gil;
        py::dict kwargs;
        kwargs["timeout_ms"] = timeout_ms;
        if (!model.empty()) kwargs["model"] = model;
        py::object result = g_call_llm_messages(messages_json, tools_json, **kwargs);
        return result.cast<std::string>();

    } catch (const py::error_already_set& e) {
        py::gil_scoped_acquire gil;
        if (e.matches(PyExc_TimeoutError)) throw cpp_agent::LLMTimeout("LLM request timeout");
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
    const std::function<void(const std::string&)>& on_complete,
    int timeout_ms, const std::string& model) {
    if (!g_initialized) throw std::runtime_error("Python bridge not initialized.");

    try {
        py::gil_scoped_acquire gil;
        py::cpp_function py_on_token(
            [on_token](const std::string& token) { on_token(token); });
        py::cpp_function py_on_complete(
            [on_complete](const std::string& result) { on_complete(result); });

        py::dict kwargs;
        kwargs["timeout_ms"] = timeout_ms;
        if (!model.empty()) kwargs["model"] = model;
        g_call_streaming_msgs(
            messages_json, tools_json,
            py::arg("on_token") = py_on_token,
            py::arg("on_complete") = py_on_complete, **kwargs);
    } catch (const py::error_already_set& e) {
        py::gil_scoped_acquire gil;
        if (e.matches(PyExc_TimeoutError)) throw cpp_agent::LLMTimeout("LLM streaming request timeout");
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
