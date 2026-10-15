# ============================================================================
# cpp-agent-runtime — CMake build configuration
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(
    cpp-agent-runtime
    VERSION  0.1.0
    DESCRIPTION "High-performance AI Agent runtime engine in C++20"
    LANGUAGES CXX
)

# ---- C++ standard ----------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Compiler-specific coroutine setup -------------------------------------
# C++20 coroutines need different flags per compiler
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0)
        # GCC 10-13: coroutines are experimental
        add_compile_options(-fcoroutines)
        message(STATUS "GCC < 14: adding -fcoroutines flag")
    endif()
    # GCC 14+ has native coroutine support, no flag needed
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang 14+ needs -stdlib=libc++ for full coroutine support on some platforms
    message(STATUS "Clang detected: ensure libc++ or equivalent coroutine support")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # MSVC: /std:c++20 implies coroutine support (VS 2022 17.0+)
    message(STATUS "MSVC detected: coroutines enabled via /std:c++20")
endif()

# ---- Common compiler warnings ----------------------------------------------
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- Python 3 + pybind11 ---------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
execute_process(COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
    OUTPUT_VARIABLE PYBIND11_CMAKE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_CMAKE_DIR}")
find_package(pybind11 REQUIRED)

message(STATUS "Python:   ${Python3_EXECUTABLE} (v${Python3_VERSION})")
message(STATUS "Includes: ${Python3_INCLUDE_DIRS}")
message(STATUS "Libraries:${Python3_LIBRARIES}")
message(STATUS "pybind11: ${pybind11_DIR}")

# ---- FetchContent dependencies ---------------------------------------------
include(FetchContent)

# fmt (string formatting)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.1.4
)
FetchContent_MakeAvailable(fmt)

# spdlog (logging)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(spdlog)

# nlohmann/json (JSON parsing) — vendored single header (third_party/nlohmann).
# The full-repo FetchContent fetch proved fragile in this environment
# (network flakiness repeatedly corrupted the clone), so we vendor the
# header-only single_include directly and expose the same imported target.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

# TUI uses raw ANSI escape codes — no external dependency needed.
# Terminal UI is implemented in src/tui/terminal_ui.cpp

# ---- Source files ----------------------------------------------------------
# Shared core library (used by both cpp-agent and tests)
set(CORE_SOURCES
    src/core/scheduler.cpp
    src/agent/message.cpp
    src/agent/react_loop.cpp
    src/memory/working_memory.cpp
    src/tool/tool_base.cpp
    src/tool/fs_tool.cpp
    src/tool/code_exec_tool.cpp
    src/tool/process.cpp
    src/tool/grep_tool.cpp
    src/tool/git_tool.cpp
    src/tui/terminal_ui.cpp
    src/llm/llm_adapter.cpp
    src/llm/py_bridge.cpp
    src/tracing/trace_recorder.cpp
)

add_library(cpp-agent-core STATIC ${CORE_SOURCES})
target_compile_definitions(cpp-agent-core PRIVATE
    CPP_AGENT_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
    CPP_AGENT_PYTHON_SITE="${Python3_SITELIB}"
    CPP_AGENT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_include_directories(cpp-agent-core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(cpp-agent-core PRIVATE
    ${Python3_INCLUDE_DIRS}
)

target_link_libraries(cpp-agent-core PUBLIC
    fmt::fmt
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    pybind11::embed
    Python3::Python
)

# ---- Main executable -------------------------------------------------------

add_executable(cpp-agent src/main.cpp)

target_include_directories(cpp-agent PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(cpp-agent PRIVATE
    cpp-agent-core
)

# ---- Copy Python modules to build directory --------------------------------
add_custom_command(TARGET cpp-agent POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/python
        $<TARGET_FILE_DIR:cpp-agent>/python
    COMMENT "Copying Python modules to build directory"
)

# ---- Tests (optional) ------------------------------------------------------
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Benchmarks (optional) -------------------------------------------------
option(BUILD_BENCHMARKS "Build benchmarks" ON)
if(BUILD_BENCHMARKS)
    enable_testing()
    add_subdirectory(benchmarks)
endif()

# ---- Demos (optional) ------------------------------------------------------
# Day 21: scripted, offline agent scenarios that emit viewable traces.
option(BUILD_DEMOS "Build the scripted agent demos" ON)
if(BUILD_DEMOS)
    add_subdirectory(demos)
endif()

# ---- Print summary ---------------------------------------------------------
message(STATUS "")
message(STATUS "cpp-agent-runtime v${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
