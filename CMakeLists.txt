# --- 1. Define Your Project ---
cmake_minimum_required(VERSION 3.13)
project(uma_serve LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)

# Enable the compile‑commands export
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Enable CTest at the top-level so `ctest` discovers tests
include(CTest)
enable_testing()

# --- 2. Configure llama.cpp (The "Engine") ---
# Set llama.cpp / ggml build options BEFORE adding it.
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")

# Prefer Metal-only build for UMA serving; explicitly disable BLAS/Accelerate to avoid
# registering extra backends (which increases reserved compute buffers on Metal).
set(GGML_METAL ON  CACHE INTERNAL "")
set(GGML_BLAS OFF  CACHE INTERNAL "")
set(GGML_ACCELERATE OFF CACHE INTERNAL "")

# Legacy/deprecated options kept for compatibility (mapped inside llama.cpp)
set(LLAMA_METAL ON CACHE INTERNAL "")

set(LLAMA_BUILD_TESTS OFF CACHE INTERNAL "")
set(LLAMA_BUILD_EXAMPLES OFF CACHE INTERNAL "")

# --- 3. Add the llama.cpp Submodule ---
# This pulls in the llama.cpp project and defines its targets
set(LLAMA_ACCELERATE OFF CACHE INTERNAL "")
add_subdirectory(external/llama.cpp)
add_subdirectory(external/googletest)
include(GoogleTest)

# --- 4. Define YOUR Executable (umad) ---
add_executable(umad
    src/umad/main.cpp
    src/runtime/config.cpp
    src/runtime/model.cpp
    src/runtime/tokens.cpp
    src/sched/scheduler.cpp
    src/sched/sampling.cpp
    src/sched/bmt.cpp
    src/sched/policy.cpp
    src/metrics/metrics.cpp
    src/ipc/uds_server.cpp
    src/ipc/protocol.cpp
    src/ipc/session_manager.cpp
)

# Platform-specific sources
if(APPLE)
    # kqueue-based poller (macOS/BSD only)
    target_sources(umad PRIVATE src/ipc/poller_kqueue.cpp)
endif()

# --- 5. Link Your Server to the Engine ---
#
#    *** THIS IS THE FIX ***
#
# We only need to link against the main 'llama' target.
# It "transitively" carries all of its own dependencies.
# So, because LLAMA_METAL=ON, 'llama' now automatically
# includes all the ggml_metal code AND tells CMake to link
# against the system Metal frameworks.
#
target_link_libraries(umad PRIVATE llama)

# Make internal headers importable as "runtime/..."
target_include_directories(umad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --- 6. CLI executable (uma-cli) ---
add_executable(uma_cli
    src/cli/main.cpp
    src/ipc/protocol.cpp
)
target_link_libraries(uma_cli PRIVATE)
target_include_directories(uma_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --- 7. Define Unit Tests ---
add_executable(uma_unit_tests
    tests/cpp/test_main.cpp
    tests/cpp/ipc_protocol_test.cpp
    tests/cpp/bmt_test.cpp
    tests/cpp/policy_test.cpp
    tests/cpp/sampling_test.cpp
    src/ipc/protocol.cpp
    src/sched/bmt.cpp
    src/sched/policy.cpp
    src/sched/sampling.cpp
)
target_link_libraries(uma_unit_tests PRIVATE gtest_main)
target_include_directories(uma_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
gtest_discover_tests(uma_unit_tests)
