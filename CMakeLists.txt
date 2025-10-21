# --- 1. Define Your Project ---
cmake_minimum_required(VERSION 3.13)
project(uma_serve LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)

# --- 2. Configure llama.cpp (The "Engine") ---
# Set llama.cpp's build options BEFORE adding it.
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(LLAMA_METAL ON CACHE INTERNAL "")
set(LLAMA_BUILD_TESTS OFF CACHE INTERNAL "")
set(LLAMA_BUILD_EXAMPLES OFF CACHE INTERNAL "")

# --- 3. Add the llama.cpp Submodule ---
# This pulls in the llama.cpp project and defines its targets
add_subdirectory(external/llama.cpp)

# --- 4. Define YOUR Executable (umad) ---
add_executable(umad
    src/umad/main.cpp
    src/runtime/config.cpp
    src/runtime/model.cpp
    src/runtime/infer.cpp
    src/ipc/uds_server.cpp
)

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
