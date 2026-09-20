# ---- Benchmark executables ---------------------------------------------------
# Day 20: each benchmark is a standalone executable with its own main().
# They print a results table and are registered as CTest tests so that
# `ctest -R bench` runs them all.
#
#   Day 20 Task 1: bench_scheduler  (coroutine scheduler throughput)
#   Day 20 Task 2: bench_tools      (tool latency / throughput)
#   Day 20 Task 3: bench_react      (ReAct loop end-to-end)

# --- Coroutine scheduler throughput ---
add_executable(bench-scheduler bench_scheduler.cpp)
target_include_directories(bench-scheduler PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bench-scheduler PRIVATE cpp-agent-core)
add_test(NAME bench-scheduler COMMAND bench-scheduler)

# --- Tool latency / throughput ---
add_executable(bench-tools bench_tools.cpp)
target_include_directories(bench-tools PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bench-tools PRIVATE cpp-agent-core)
add_test(NAME bench-tools COMMAND bench-tools)

# --- ReAct loop end-to-end ---
add_executable(bench-react bench_react.cpp)
target_include_directories(bench-react PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bench-react PRIVATE cpp-agent-core)
add_test(NAME bench-react COMMAND bench-react)
