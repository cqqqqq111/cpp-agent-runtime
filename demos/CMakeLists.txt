# ---- Day 21 demo executables -------------------------------------------------
# Each demo is a standalone executable with its own main(). They drive a real
# ReAct loop with real tools (filesystem / grep) but a canned ScriptedLLM, and
# emit a logs/<name>.json trace viewable in web/index.html. No network, no key.
#
#   code_review_demo  — read file + grep TODO/FIXME → review verdict
#   debug_demo        — grep + read_file → root cause + fix
#   refactor_demo     — grep references + read_file → migration plan

set(DEMO_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/demo_common.h)

# --- Code review scenario ---
add_executable(code_review_demo code_review_demo.cpp)
target_include_directories(code_review_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}          # for "demos/demo_common.h" include path
)
target_link_libraries(code_review_demo PRIVATE cpp-agent-core)

# --- Debug scenario ---
add_executable(debug_demo debug_demo.cpp)
target_include_directories(debug_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(debug_demo PRIVATE cpp-agent-core)

# --- Refactor scenario ---
add_executable(refactor_demo refactor_demo.cpp)
target_include_directories(refactor_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(refactor_demo PRIVATE cpp-agent-core)

# --- Real-LLM one-shot (DeepSeek via .env, real tools) ---
add_executable(real_agent_demo real_agent_demo.cpp)
target_include_directories(real_agent_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(real_agent_demo PRIVATE cpp-agent-core)
