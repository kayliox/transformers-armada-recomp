# Transformers (SLUS-20668) executable: generated game code + PS2Recomp runtime.

set(TF_WORK_DIR "${CMAKE_SOURCE_DIR}/work" CACHE PATH
    "Folder holding the recompiler output produced by build.ps1")
set(TF_GENERATED_DIR "${TF_WORK_DIR}/generated")
set(TF_BATCH_DIR "${TF_WORK_DIR}/batches")

# The ~22k generated function files are compiled through batch files that
# tools/sync_generated.py writes, one per 16 KB of guest code. Batches are
# keyed by address, so recompiling after new functions are found only rebuilds
# the batches covering them (CMake's own unity batches shift with file order).
file(GLOB TF_BATCH_SOURCES CONFIGURE_DEPENDS "${TF_BATCH_DIR}/batch_*.cpp")
set(TF_REGISTER_FUNCTIONS "${TF_GENERATED_DIR}/register_functions.cpp")

file(GLOB TF_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
# The decompiled code (decomp/README.md), a folder per part of the game.
file(GLOB_RECURSE TF_DECOMP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/decomp/*.cpp")
# The native PC renderer (native_render/README.md): Direct3D 12 in place of the emulated VIF1/VU1/GS.
file(GLOB TF_NATIVE_RENDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/native_render/*.cpp")
# (The large images' BC7 encoder, TODO 94: bc7enc, MIT / public domain - native_render/ext/bc7enc/README.txt.)
list(APPEND TF_NATIVE_RENDER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/native_render/ext/bc7enc/bc7enc.cpp")
# Direct3D 12 and its GL interop are Windows' alone; elsewhere nr_none.cpp stands in. On Linux TF_VKD3D_DIR (a static
# vkd3d 2.1 or newer, Wine's Direct3D 12 on Vulkan) brings nr_d3d.cpp back: nr_none.cpp then stubs only the GL interop.
if(WIN32)
    list(FILTER TF_NATIVE_RENDER_SOURCES EXCLUDE REGEX "/nr_none\\.cpp$")
else()
    set(TF_VKD3D_DIR "" CACHE PATH "A static vkd3d (2.1 or newer) install: the native renderer on Vulkan; empty: none")
    if(TF_VKD3D_DIR AND EXISTS "${TF_VKD3D_DIR}/lib/libvkd3d-utils.a")
        set(TF_NATIVE_VKD3D ON)
        list(FILTER TF_NATIVE_RENDER_SOURCES EXCLUDE REGEX "/nr_glshare\\.cpp$")
        message(STATUS "TransformersRecomp: the native renderer through vkd3d (${TF_VKD3D_DIR})")
    else()
        list(FILTER TF_NATIVE_RENDER_SOURCES EXCLUDE REGEX "/nr_(d3d|glshare)\\.cpp$")
    endif()
endif()
# (per file, not for the target: a new definition or include folder there would compile all of the game's code again)
if(TF_NATIVE_VKD3D)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/native_render/nr_capture.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/native_render/nr_none.cpp"
        PROPERTIES COMPILE_DEFINITIONS TF_NATIVE_RENDERER=1)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/native_render/nr_d3d.cpp" PROPERTIES
        COMPILE_DEFINITIONS TF_NATIVE_RENDERER=1 INCLUDE_DIRECTORIES "${TF_VKD3D_DIR}/include/vkd3d;${TF_VKD3D_DIR}/include")
    set(TF_VKD3D_LIBS "${TF_VKD3D_DIR}/lib/libvkd3d-utils.a" "${TF_VKD3D_DIR}/lib/libvkd3d.a"
        "${TF_VKD3D_DIR}/lib/libvkd3d-shader.a" dl pthread m)
endif()

# The version the build is made from (TODO 104, updates.h): tf_version.h, written at every build by
# version.cmake (only when it changes: the commit, its branch and date, a copy of the home repository
# or a development checkout).
set(TF_VERSION_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/tf_version")
set(TF_VERSION_COMMIT_OVERRIDE "" CACHE STRING "The commit a build says it is made from (prebuilt binaries, tests); empty: git's")
set(TF_VERSION_KIND_OVERRIDE "" CACHE STRING "install or dev (prebuilt binaries, tests); empty: found from the folders")
add_custom_target(tf_version
    COMMAND "${CMAKE_COMMAND}" -DSOURCE=${CMAKE_SOURCE_DIR} -DOUTPUT=${TF_VERSION_HEADER_DIR}/tf_version.h
            -DCOMMIT_OVERRIDE=${TF_VERSION_COMMIT_OVERRIDE} -DKIND_OVERRIDE=${TF_VERSION_KIND_OVERRIDE}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/version.cmake"
    BYPRODUCTS "${TF_VERSION_HEADER_DIR}/tf_version.h"
    COMMENT "tf_version.h"
    VERBATIM)
# (its folder for updates.cpp only: an include folder for the whole target would change every file's
# command line, and the first build after this change would compile all of the game's code again)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/updates.cpp"
    PROPERTIES INCLUDE_DIRECTORIES "${TF_VERSION_HEADER_DIR}")

# The native renderer's shaders built in (nr_d3d.cpp, cache::loadBaked): build.ps1 runs the linked executable with
# --bake-shaders to write this table, then links again if it changed. Until then (or with another build path) an
# empty one: the shaders are compiled at run time and kept in the shader cache, as before.
set(TF_BAKED_SHADERS "${CMAKE_CURRENT_BINARY_DIR}/baked/nr_shaders_baked.inc")
if(NOT EXISTS "${TF_BAKED_SHADERS}")
    file(WRITE "${TF_BAKED_SHADERS}" "// Empty until build.ps1 runs TransformersRecomp --bake-shaders (nr_d3d.cpp).
static const BakedShader kBakedShaders[] = {{0ull, nullptr, 0u}};
")
endif()

if(NOT TF_BATCH_SOURCES OR NOT EXISTS "${TF_REGISTER_FUNCTIONS}")
    message(WARNING
        "No generated game code in ${TF_WORK_DIR}.
"
        "Run build.ps1 (build.sh on Linux) with your own Transformers (USA) disc image first; "
        "only the tools and runtime will be built.")
    # The port's own code still compiles without a disc (CI: tf_port_objects). vu1_native.cpp needs microcode made
    # from the disc, so it is left out.
    set(TF_PORT_CHECK_SOURCES ${TF_GAME_SOURCES} ${TF_DECOMP_SOURCES} ${TF_NATIVE_RENDER_SOURCES})
    list(FILTER TF_PORT_CHECK_SOURCES EXCLUDE REGEX "/vu1_native\\.cpp$")
    add_library(tf_port_objects OBJECT EXCLUDE_FROM_ALL ${TF_PORT_CHECK_SOURCES})
    add_dependencies(tf_port_objects tf_version)
    target_include_directories(tf_port_objects PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/decomp" "${CMAKE_CURRENT_SOURCE_DIR}/native_render"
        "${CMAKE_CURRENT_BINARY_DIR}/baked")
    target_link_libraries(tf_port_objects PRIVATE ps2_runtime)
    if(PS2X_ENABLE_DEBUG_UI)
        target_link_libraries(tf_port_objects PRIVATE rlImGui imgui)
        target_compile_definitions(tf_port_objects PRIVATE PS2X_ENABLE_DEBUG_UI=1)
    endif()
    return()
endif()

list(LENGTH TF_BATCH_SOURCES TF_BATCH_COUNT)
message(STATUS "Transformers: ${TF_BATCH_COUNT} batches of generated code")

add_executable(TransformersRecomp
    ${TF_BATCH_SOURCES}
    "${TF_REGISTER_FUNCTIONS}"
    ${TF_GAME_SOURCES}
    ${TF_DECOMP_SOURCES}
    ${TF_NATIVE_RENDER_SOURCES}
)

target_include_directories(TransformersRecomp PRIVATE
    "${TF_GENERATED_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/decomp"
    "${CMAKE_CURRENT_SOURCE_DIR}/native_render"
    "${CMAKE_CURRENT_BINARY_DIR}/baked"
)

target_link_libraries(TransformersRecomp PRIVATE ps2_runtime)

# The grass's VU1 program as straight C++ (vu1_native.cpp includes it): a translation of the game's own microcode,
# so it is made from the game's executable at build time (tools/make_vu1_grass.py), never kept in the repository.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(TF_VU1_GRASS_INC "${TF_GENERATED_DIR}/vu1_grass47dd.inc")
add_custom_command(OUTPUT "${TF_VU1_GRASS_INC}"
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/make_vu1_grass.py"
            "${CMAKE_SOURCE_DIR}/game/SLUS_206.68" "${TF_VU1_GRASS_INC}"
    DEPENDS "${CMAKE_SOURCE_DIR}/game/SLUS_206.68" "${CMAKE_SOURCE_DIR}/tools/make_vu1_grass.py"
            "${CMAKE_SOURCE_DIR}/tools/vu1_grass47dd_lags.json" "${CMAKE_SOURCE_DIR}/tools/vu1_recompile.py"
            "${CMAKE_SOURCE_DIR}/tools/vu1_trace.py" "${CMAKE_SOURCE_DIR}/tools/vu1_batch.py"
    COMMENT "The grass's VU1 program, from the game's executable"
    VERBATIM)
target_sources(TransformersRecomp PRIVATE "${TF_VU1_GRASS_INC}")
set_source_files_properties("${TF_VU1_GRASS_INC}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)

# The private overlay (overlay/overlay.cmake: the mods, registered through src/port_ext.h). Empty or missing: the
# public build. -DTF_OVERLAY_DIR= (empty) builds the public game from a tree that has the overlay.
set(TF_OVERLAY_DIR "${CMAKE_SOURCE_DIR}/overlay" CACHE PATH "The private overlay (mods); empty or missing = the public build")
if(TF_OVERLAY_DIR AND EXISTS "${TF_OVERLAY_DIR}/overlay.cmake")
    include("${TF_OVERLAY_DIR}/overlay.cmake")
else()
    message(STATUS "TransformersRecomp: the public build (no overlay)")
endif()

add_dependencies(TransformersRecomp tf_version)
# The exe's icon, made from the player's disc into the build folder (icon.cmake, TODO 198).
include("${CMAKE_CURRENT_SOURCE_DIR}/icon.cmake")
if(WIN32)
    target_link_libraries(TransformersRecomp PRIVATE d3d12 dxgi d3dcompiler)
elseif(TF_NATIVE_VKD3D)
    target_link_libraries(TransformersRecomp PRIVATE ${TF_VKD3D_LIBS})
endif()

# register_functions.cpp is one 50 MB constructor filling the function table
# (~640k assignments). It runs once at startup, so optimizing it only makes it
# the slowest file in the build by far; compile it on its own, unoptimized.
# (Same finding as BT3-Recomp's docs/BUILD-TIME.md.)
set_source_files_properties("${TF_REGISTER_FUNCTIONS}" PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/Od>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O0>")
set_source_files_properties(${TF_GAME_SOURCES} ${TF_DECOMP_SOURCES} ${TF_NATIVE_RENDER_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# /Zi funnels every compiler process through one PDB server, which serializes
# a build this wide; /Z7 keeps the debug info in each object file instead.
set_target_properties(TransformersRecomp PROPERTIES
    MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

# Only the fixed runtime headers: the generated all-functions headers change
# whenever the function map does and would invalidate every batch.
target_precompile_headers(TransformersRecomp PRIVATE
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime_macros.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_syscalls.h"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_stubs.h"
)

if(PS2X_ENABLE_DEBUG_UI)
    target_sources(TransformersRecomp PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp")
    set_source_files_properties("${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp"
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(TransformersRecomp PRIVATE rlImGui imgui)
    target_compile_definitions(TransformersRecomp PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

if(MSVC)
    target_compile_options(TransformersRecomp PRIVATE /FS /bigobj)
    # No stack cookies in the generated code: nearly every one of its functions inlines a load
    # with a small local array (the wrapped read at the end of RAM), and each got a check.
    target_compile_options(TransformersRecomp PRIVATE $<$<CONFIG:RelWithDebInfo>:/GS->)
    # (FPS round 4) Inline like Release does (/Ob2), as the runtime libraries already are (top CMakeLists): with
    # RelWithDebInfo's /Ob1 only functions marked inline are expanded, and the standard library's small helpers are
    # not - every std::atomic load in the generated code's inline paths (the scratchpad's pointer behind each inline
    # scratchpad access, the EE checkpoint's flags) was a call to std::_Atomic_address_as. Same build time.
    # -DTF_GENERATED_OB1=ON: /Ob1 as before (A/B).
    option(TF_GENERATED_OB1 "Compile the game target with RelWithDebInfo's /Ob1 instead of /Ob2" OFF)
    if(NOT TF_GENERATED_OB1)
        target_compile_options(TransformersRecomp PRIVATE $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
    # WinAPI and raylib both define CloseWindow and friends.
    target_link_options(TransformersRecomp PRIVATE /FORCE:MULTIPLE)
endif()

# (FPS round 5) Guest loads and stores keep only main RAM's usual case inline in the generated code, the rare paths
# out of line (ps2_runtime_macros.h). -DTF_MEMORY_COLD_INLINE=ON: every rare path inline at each access as before (A/B).
option(TF_MEMORY_COLD_INLINE "Inline every rare path of a guest load/store at each access (before FPS round 5)" OFF)
if(TF_MEMORY_COLD_INLINE)
    target_compile_definitions(TransformersRecomp PRIVATE PS2X_MEMORY_COLD_INLINE=1)
endif()
# The scratchpad's test (a non-constant address) out of line as well (measured faster: the code is smaller);
# -DTF_MEMORY_SCRATCHPAD_OUTLINE=OFF: inline after main RAM's.
option(TF_MEMORY_SCRATCHPAD_OUTLINE "Leave the scratchpad test of a guest load/store to the out-of-line path" ON)
if(TF_MEMORY_SCRATCHPAD_OUTLINE)
    target_compile_definitions(TransformersRecomp PRIVATE PS2X_MEMORY_SCRATCHPAD_OUTLINE=1)
endif()
# -DTF_MEMORY_MIRROR_INLINE=OFF: main RAM's uncached mirror (0x20000000 + offset) left to the out-of-line path.
option(TF_MEMORY_MIRROR_INLINE "Test main RAM's uncached mirror inline in a guest load/store" ON)
if(NOT TF_MEMORY_MIRROR_INLINE)
    target_compile_definitions(TransformersRecomp PRIVATE PS2X_MEMORY_MIRROR_INLINE=0)
endif()

EnableFastReleaseMode(TransformersRecomp)
# Whole-program optimisation for the build that is played (RelWithDebInfo): /GL for the game and the runtime,
# /LTCG for the link (EnableFastReleaseMode's are Release-only). Floating point as before (/fp:precise), no /arch.
# -DTF_LTCG=ON (an A/B: the measurement in docs/RUNTIME.md).
option(TF_LTCG "Whole-program optimisation (/GL + /LTCG) for RelWithDebInfo" OFF)
if(MSVC AND TF_LTCG)
    foreach(target TransformersRecomp ps2_runtime)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo>:/GL>)
        endif()
    endforeach()
    # (not the function table's one constructor, compiled unoptimised on purpose)
    set_property(SOURCE "${TF_REGISTER_FUNCTIONS}" APPEND PROPERTY COMPILE_OPTIONS $<$<CXX_COMPILER_ID:MSVC>:/GL->)
    target_link_options(TransformersRecomp PRIVATE $<$<CONFIG:RelWithDebInfo>:/LTCG /INCREMENTAL:NO>)
    if(TARGET ps2_runtime)
        set_property(TARGET ps2_runtime APPEND PROPERTY STATIC_LIBRARY_OPTIONS $<$<CONFIG:RelWithDebInfo>:/LTCG>)
    endif()
endif()
ps2x_stage_ffmpeg_runtime_dlls(TransformersRecomp)

