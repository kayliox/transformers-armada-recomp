cmake_minimum_required(VERSION 3.21)

# Lets targets choose /Z7 (debug info inside each .obj) instead of /Zi.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()

# Game build: PS2Recomp's runtime + IOP libraries and the generated game code.
# The recompiler tools are built separately from PS2Recomp's own top-level
# project (see build.ps1), because their CMake files expect to be the root.
project(TransformersRecomp LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The runtime's SSE4.1 intrinsics: MSVC takes them without a flag, GCC and Clang need -msse4.1 (PS2Recomp's own CI
# passes it on the command line).
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_compile_options(-msse4.1)
endif()

set(PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/PS2Recomp")
if(NOT EXISTS "${PS2RECOMP_DIR}/ps2xRuntime/CMakeLists.txt")
    message(FATAL_ERROR "PS2Recomp submodule is missing. Run: git submodule update --init --recursive")
endif()

# PS2Recomp's "aggressive logs" also define PS2_FUNCTION_LOG_TRACKER, which
# makes every generated function write (and flush) enter/exit lines to
# ps2_log.txt: hundreds of MB per minute and most of the frame time. Off unless
# a call trace is really needed.
option(TF_TRACE_CALLS "Trace every recompiled function call to ps2_log.txt (very slow)" OFF)
set(PS2X_ENABLE_AGRESSIVE_LOGS ${TF_TRACE_CALLS} CACHE BOOL "" FORCE)

add_subdirectory("${PS2RECOMP_DIR}/ps2xIOP" ps2xIOP)
add_subdirectory("${PS2RECOMP_DIR}/ps2xRuntime" ps2xRuntime)

# The runtime's own sample runner is replaced by the game executable below.
set_target_properties(ps2EntryRunner PROPERTIES EXCLUDE_FROM_ALL ON EXCLUDE_FROM_DEFAULT_BUILD ON)

# CMake's RelWithDebInfo uses /Ob1, which only inlines functions marked
# `inline`: the runtime's small helpers (GS rasterizer, VU, DMA, IOP) then stay
# calls in the hottest loops. Inline like Release does; the libraries are
# small, so this costs little build time.
if(MSVC)
    foreach(lib ps2_runtime ps2_iop)
        target_compile_options(${lib} PRIVATE $<$<CONFIG:RelWithDebInfo>:/Ob2 /Oi>)
    endforeach()
endif()

add_subdirectory(transformers)
