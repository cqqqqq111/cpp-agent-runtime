# ---- Test executables -------------------------------------------------------
# Keep test assertions enabled even in Release builds.
if(MSVC)
    add_compile_options(/UNDEBUG)
else()
    add_compile_options(-UNDEBUG)
endif()
# Each test file has its own main(). We build them as separate executables.
#
# Tests added per day:
#   Day 2:  test_scheduler  (coroutine scheduler)
#   Day 3:  test_message    (Message, MessageHistory, Tool pipeline)
#   Day 4:  test_react_loop (ReAct loop)
#   Day 8-10:test_memory    (WorkingMemory, LongTermMemory)
#   Day 10: test_tools      (Tool execution)

# --- Scheduler tests ---
add_executable(test-scheduler test_scheduler.cpp)
target_include_directories(test-scheduler PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test-scheduler PRIVATE cpp-agent-core)
add_test(NAME scheduler COMMAND test-scheduler)

# --- Message + Tool tests ---
add_executable(test-message test_message.cpp)
target_include_directories(test-message PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test-message PRIVATE cpp-agent-core)
add_test(NAME message COMMAND test-message)

# --- ReAct loop tests ---
add_executable(test-react-loop test_react_loop.cpp)
target_include_directories(test-react-loop PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test-react-loop PRIVATE cpp-agent-core)
add_test(NAME react-loop COMMAND test-react-loop)

# --- FileSystem tool tests ---
add_executable(test-fs-tool test_fs_tool.cpp)
target_include_directories(test-fs-tool PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test-fs-tool PRIVATE cpp-agent-core)
add_test(NAME fs-tool COMMAND test-fs-tool)

# --- Working memory tests ---
add_executable(test-memory test_memory.cpp)
target_include_directories(test-memory PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test-memory PRIVATE cpp-agent-core)
add_test(NAME memory COMMAND test-memory)

# --- Tools tests (CodeExec + Grep + Git) ---
add_executable(test-tools test_tools.cpp)
target_include_directories(test-tools PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test-tools PRIVATE cpp-agent-core)
add_test(NAME tools COMMAND test-tools)

# --- Day 11: Parallel + streaming tests ---
add_executable(test-parallel test_parallel.cpp)
target_include_directories(test-parallel PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test-parallel PRIVATE cpp-agent-core)
add_test(NAME parallel COMMAND test-parallel)

# --- Day 22: Persistence + per-iteration timeout tests ---
add_executable(test-day22 test_day22.cpp)
target_include_directories(test-day22 PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test-day22 PRIVATE cpp-agent-core)
add_test(NAME day22 COMMAND test-day22)

add_executable(test-regressions test_regressions.cpp)
target_link_libraries(test-regressions PRIVATE cpp-agent-core)
add_test(NAME regressions COMMAND test-regressions)

add_executable(test-python-bridge test_python_bridge.cpp)
target_link_libraries(test-python-bridge PRIVATE cpp-agent-core)
add_test(NAME python-bridge COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_bridge_server.py $<TARGET_FILE:test-python-bridge> $<TARGET_FILE:cpp-agent>)
set_tests_properties(python-bridge PROPERTIES TIMEOUT 25)

add_test(NAME offline-cli COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py $<TARGET_FILE:cpp-agent>)

add_executable(test-metrics test_metrics.cpp)
target_link_libraries(test-metrics PRIVATE cpp-agent-core)
add_test(NAME metrics COMMAND test-metrics)
add_test(NAME trace-stats COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_trace_stats.py)

add_test(NAME transport COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_transport.py)
set_tests_properties(transport PROPERTIES TIMEOUT 30)
