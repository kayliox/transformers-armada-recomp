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
# (TODO 290 (17): its BC7 decoder too, same commit - a PCSX2 pack's DDS images read)
list(APPEND TF_NATIVE_RENDER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/native_render/ext/bc7enc/bc7decomp.cpp")
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
    # (main.cpp and nr_capture.cpp ask nr_backend.cpp's nativeBackendChosen() at run time instead: the same question,
    # and right for a Vulkan-only build too.)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/native_render/nr_none.cpp"
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

# The release this build is for (build.ps1 passes it from the disc): its executable's name and CRC32 from its
# config (config/game.json for the USA release, config/versions/<serial>/game.json for the others).
set(TF_GAME_VERSION "SLUS-20668" CACHE STRING "The release built: SLUS-20668 (USA) or a serial under config/versions")
if(TF_GAME_VERSION STREQUAL "SLUS-20668")
    set(TF_GAME_JSON "${CMAKE_CURRENT_SOURCE_DIR}/config/game.json")
else()
    set(TF_GAME_JSON "${CMAKE_CURRENT_SOURCE_DIR}/config/versions/${TF_GAME_VERSION}/game.json")
endif()
file(READ "${TF_GAME_JSON}" _tf_game_json)
string(JSON TF_ELF_NAME GET "${_tf_game_json}" elf_name)
string(JSON _tf_elf_crc GET "${_tf_game_json}" elf_crc32)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TF_GAME_JSON}")
target_compile_definitions(TransformersRecomp PRIVATE TF_ELF_NAME="${TF_ELF_NAME}" TF_ELF_CRC32=0x${_tf_elf_crc}u
                           TF_GAME_SERIAL="${TF_GAME_VERSION}")
if(TF_GAME_VERSION STREQUAL "SLES-52388")
    target_compile_definitions(TransformersRecomp PRIVATE TF_GAME_PAL=1) # (game_version.h: TF_VA)
endif()
message(STATUS "Transformers: release ${TF_GAME_VERSION} (${TF_ELF_NAME})")

# The disc's extracted files: game\ in the checkout, or the install's data\game once its folders were moved (TODO 279,
# docs/INSTALL_LAYOUT_PLAN.md): build.ps1 passes the one it uses. The grass's VU1 program and the exe's icon read it.
set(TF_GAME_DIR "${CMAKE_SOURCE_DIR}/game" CACHE PATH "The disc's extracted files (build.ps1 passes where they are)")

# The grass's VU1 program as straight C++ (vu1_native.cpp includes it): a translation of the game's own microcode,
# so it is made from the game's executable at build time (tools/make_vu1_grass.py), never kept in the repository.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(TF_VU1_GRASS_INC "${TF_GENERATED_DIR}/vu1_grass47dd.inc")
add_custom_command(OUTPUT "${TF_VU1_GRASS_INC}"
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/make_vu1_grass.py"
            "${TF_GAME_DIR}/${TF_ELF_NAME}" "${TF_VU1_GRASS_INC}"
    DEPENDS "${TF_GAME_DIR}/${TF_ELF_NAME}" "${CMAKE_SOURCE_DIR}/tools/make_vu1_grass.py"
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

# The launcher (TODO 279, release B: launcher/launcher.cpp): "Transformers Armada.exe", put beside tfa-setup.exe in a
# player's install folder by build.ps1. A small program of its own (no window, the game's icon when there is one),
# built into the same folder as the game.
if(WIN32)
    add_executable(tfa_launcher WIN32 "${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.cpp")
    set_target_properties(tfa_launcher PROPERTIES OUTPUT_NAME "Transformers Armada")
    target_compile_features(tfa_launcher PRIVATE cxx_std_20)
    if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/tf_icon/transformers.rc")
        target_sources(tfa_launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tf_icon/transformers.rc")
    endif()
endif()
if(WIN32)
    target_link_libraries(TransformersRecomp PRIVATE d3d12 dxgi d3dcompiler)
elseif(TF_NATIVE_VKD3D)
    target_link_libraries(TransformersRecomp PRIVATE ${TF_VKD3D_LIBS})
endif()

# Linux: the movie decoding comes from the FFmpeg the build found (pkg-config), so the executable asks the loader for
# that build's libavcodec ABI. The libraries travel beside the executable and it looks there first, so a build made in
# a distribution container (a Steam Deck's tfa-build: Ubuntu 24.04, libavcodec.so.60) runs on SteamOS, which has no
# FFmpeg or another ABI. Nothing is copied when FFmpeg is linked statically.
if(NOT WIN32)
    set_target_properties(TransformersRecomp PROPERTIES BUILD_RPATH "\$ORIGIN")
    # DT_RPATH, not DT_RUNPATH: the loader then searches the game's own folder for the dependencies of the bundled
    # libraries too - libavcodec asks for libswresample and the like, and a RUNPATH is used only for the executable's
    # own entries.
    target_link_options(TransformersRecomp PRIVATE "LINKER:--disable-new-dtags")
    add_custom_command(TARGET TransformersRecomp POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -DEXECUTABLE="$<TARGET_FILE:TransformersRecomp>"
                -DCOPY_DESTINATION="$<TARGET_FILE_DIR:TransformersRecomp>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/../tools/bundle_libraries.cmake"
        COMMENT "Copying the shared libraries the game needs beside it")
endif()

# The Vulkan backend (native_render/vulkan/, chosen at start with TF_GFX_API=vulkan; Direct3D 12 stays the default)
# and its SDL3 window. The Vulkan headers, the volk loader (it loads the system's Vulkan at run time: no Vulkan SDK
# needed to build) and SDL3 (static) are fetched like raylib. Off by default while it is in development (on, every build
# fetches ~310 MB more): -DTF_VULKAN=ON, or build.ps1 -Vulkan. Off: Direct3D 12 alone, nothing fetched.
option(TF_VULKAN "Build the Vulkan backend (TF_GFX_API=vulkan) and its SDL3 window" OFF)
if(TF_VULKAN)
    include(FetchContent)
    # (headers and volk.c only: their own CMake projects are not added - SOURCE_SUBDIR names a folder they do not have)
    FetchContent_Declare(tf_vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG vulkan-sdk-1.4.363.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR not-a-cmake-project)
    FetchContent_Declare(tf_volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG vulkan-sdk-1.4.363.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR not-a-cmake-project)
    set(SDL_SHARED OFF)
    set(SDL_STATIC ON)
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_TESTS OFF)
    set(SDL_EXAMPLES OFF)
    set(SDL_INSTALL OFF)
    FetchContent_Declare(tf_sdl3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.18
        GIT_SHALLOW TRUE)
    # The Vulkan Memory Allocator (its header alone) for the images and buffers.
    FetchContent_Declare(tf_vma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR not-a-cmake-project)
    FetchContent_MakeAvailable(tf_vulkan_headers tf_volk tf_vma)
    if(NOT TARGET SDL3::SDL3-static) # (the runtime's SDL3 platform has it already: PS2X_SDL3)
        FetchContent_MakeAvailable(tf_sdl3)
    endif()
    # The shader compiler for SPIR-V, used by the build only: DXC's release (its dxcompiler.dll, loaded by the bake below;
    # the same HLSL as Direct3D 12's). The Windows SDK's DXC has no SPIR-V; this one does. Nothing of it ships.
    if(WIN32)
        set(TF_DXC_URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip)
        set(TF_DXC_SHA256 ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1)
        FetchContent_Declare(tf_dxc
            URL ${TF_DXC_URL}
            URL_HASH SHA256=${TF_DXC_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_SUBDIR not-a-cmake-project)
        FetchContent_MakeAvailable(tf_dxc)
    elseif(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        # (Linux: the same release's tarball - its libdxcompiler.so, loaded by the bake by its full path)
        set(TF_DXC_URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz)
        set(TF_DXC_SHA256 96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1)
        FetchContent_Declare(tf_dxc
            URL ${TF_DXC_URL}
            URL_HASH SHA256=${TF_DXC_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_SUBDIR not-a-cmake-project)
        FetchContent_MakeAvailable(tf_dxc)
    endif()
    add_library(tf_volk STATIC "${tf_volk_SOURCE_DIR}/volk.c")
    target_include_directories(tf_volk PRIVATE "${tf_volk_SOURCE_DIR}" "${tf_vulkan_headers_SOURCE_DIR}/include")
    target_compile_definitions(tf_volk PRIVATE VK_NO_PROTOTYPES)
    # The HLSL the backend draws with: Direct3D 12's own string literals, taken from nr_d3d.cpp into a header at build time
    # (looked at again when nr_d3d.cpp changes, written only when its shaders did), so both backends draw with one source
    # until the HLSL moves into files of its own. Its own target: the bake and the backend both wait for it.
    set(TF_VK_GEN "${CMAKE_CURRENT_BINARY_DIR}/tf_vulkan_gen")
    add_custom_command(OUTPUT "${TF_VK_GEN}/nr_hlsl_sources.h"
        COMMAND ${CMAKE_COMMAND} "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/native_render/nr_d3d.cpp" "-DOUT=${TF_VK_GEN}/nr_hlsl_sources.h"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/native_render/vulkan/extract_hlsl.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/native_render/nr_d3d.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/native_render/vulkan/extract_hlsl.cmake"
        COMMENT "Vulkan backend: the HLSL from nr_d3d.cpp")
    add_custom_target(tf_vulkan_hlsl DEPENDS "${TF_VK_GEN}/nr_hlsl_sources.h")
    # The SPIR-V built into the game (native_render/vulkan/nr_vulkan_shaders.h): a small program of the build's own
    # (bake_spirv.cpp, never shipped) loads the fetched DXC's dxcompiler.dll and compiles every shader the backend makes,
    # the pixel shader's 64 variants included, into a table of SPIR-V words (nr_spirv_baked.cpp) - again whenever the HLSL
    # or the bake changes (its program is then built again). So the game needs no shader compiler and no dxcompiler.dll.
    # Without a compiler for this system (DXC is fetched for Windows and x86-64 Linux) the table is empty and the backend
    # says so at start.
    set(TF_VK_BAKED "${TF_VK_GEN}/nr_spirv_baked.cpp")
    if(WIN32)
        set(TF_DXC_LIBRARY "${tf_dxc_SOURCE_DIR}/bin/x64/dxcompiler.dll")
        set(TF_DXC_INCLUDE "${tf_dxc_SOURCE_DIR}/inc")
    elseif(DEFINED tf_dxc_SOURCE_DIR)
        set(TF_DXC_LIBRARY "${tf_dxc_SOURCE_DIR}/lib/libdxcompiler.so")
        set(TF_DXC_INCLUDE "${tf_dxc_SOURCE_DIR}/include/dxc")
    endif()
    if(TF_DXC_LIBRARY)
        add_executable(tf_vulkan_bake
            "${CMAKE_CURRENT_SOURCE_DIR}/native_render/vulkan/bake_spirv.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/native_render/vulkan/nr_vulkan_dxc.cpp")
        target_include_directories(tf_vulkan_bake PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native_render"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/native_render/vulkan" "${TF_VK_GEN}" "${TF_DXC_INCLUDE}")
        target_compile_features(tf_vulkan_bake PRIVATE cxx_std_20)
        target_link_libraries(tf_vulkan_bake PRIVATE ${CMAKE_DL_LIBS})
        # (its own folder: not beside the game)
        set_target_properties(tf_vulkan_bake PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tf_vulkan_bake")
        add_dependencies(tf_vulkan_bake tf_vulkan_hlsl)
        # (The bake runs again when the DXC release changes too: its dxcompiler.dll, and a stamp naming the release - the
        # archive's files keep their own dates, so a later release can bring an older one; the stamp is written only when
        # the release named above changes.)
        set(TF_DXC_STAMP "${TF_VK_GEN}/dxc_release.txt")
        set(TF_DXC_NAMED "${TF_DXC_URL} ${TF_DXC_SHA256}\n")
        set(TF_DXC_WAS "")
        if(EXISTS "${TF_DXC_STAMP}")
            file(READ "${TF_DXC_STAMP}" TF_DXC_WAS)
        endif()
        if(NOT TF_DXC_WAS STREQUAL TF_DXC_NAMED)
            file(WRITE "${TF_DXC_STAMP}" "${TF_DXC_NAMED}")
        endif()
        add_custom_command(OUTPUT "${TF_VK_BAKED}"
            COMMAND tf_vulkan_bake "${TF_DXC_LIBRARY}" "${TF_VK_BAKED}"
            DEPENDS tf_vulkan_bake "${TF_DXC_LIBRARY}" "${TF_DXC_STAMP}"
            COMMENT "Vulkan backend: its shaders compiled to SPIR-V (every variant)")
    elseif(NOT EXISTS "${TF_VK_BAKED}")
        file(WRITE "${TF_VK_BAKED}" "// No SPIR-V: no shader compiler for this system yet (transformers/CMakeLists.txt).
#include \"nr_vulkan_shaders.h\"
namespace nr::vulkan::shaders
{
    const Baked kBaked[] = {{0ull, nullptr, 0u}};
    const size_t kBakedCount = 0;
}
")
    endif()
    # The backend is a library of its own (native_render/vulkan/ is outside the native_render/*.cpp glob above): SDL3's
    # and Vulkan's include folders and defines stay on its files. Linked straight to the game target, SDL3's include
    # folders went onto every one of the game's command lines (even through $<LINK_ONLY:...>), and switching between a
    # build with and without this compiled all of the game's code again.
    add_library(tf_vulkan_backend STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/native_render/vulkan/nr_vulkan.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/native_render/vulkan/nr_vulkan_vma.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/native_render/vulkan/nr_vulkan_window.cpp"
        "${TF_VK_BAKED}")
    add_dependencies(tf_vulkan_backend tf_vulkan_hlsl)
    # (Milestone 3) The game's window presented by Vulkan under SDL3's platform (PS2X_SDL3): its overlays drawn with
    # ImGui's Vulkan renderer, built here with volk's functions.
    if(PS2X_SDL3)
        FetchContent_GetProperties(imgui)
        target_sources(tf_vulkan_backend PRIVATE "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
        target_include_directories(tf_vulkan_backend PRIVATE "${imgui_SOURCE_DIR}" "${PS2RECOMP_DIR}/ps2xRuntime/include")
        target_compile_definitions(tf_vulkan_backend PRIVATE IMGUI_IMPL_VULKAN_USE_VOLK TF_VK_GAME_WINDOW=1)
        target_link_libraries(tf_vulkan_backend PRIVATE imgui)
    endif()
    target_include_directories(tf_vulkan_backend PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native_render"
                               "${CMAKE_CURRENT_SOURCE_DIR}/native_render/vulkan" "${tf_volk_SOURCE_DIR}"
                               "${tf_vulkan_headers_SOURCE_DIR}/include" "${tf_vma_SOURCE_DIR}/include" "${TF_VK_GEN}")
    target_compile_definitions(tf_vulkan_backend PRIVATE TF_VULKAN=1 VK_NO_PROTOTYPES VMA_STATIC_VULKAN_FUNCTIONS=0
                               VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
    target_compile_features(tf_vulkan_backend PRIVATE cxx_std_20)
    target_link_libraries(tf_vulkan_backend PRIVATE tf_volk SDL3::SDL3-static)
    target_link_libraries(TransformersRecomp PRIVATE tf_vulkan_backend)
endif()

# nr_backend.cpp chooses the backend: told on its own command line which native ones this build has (Windows' Direct3D
# 12, vkd3d's, Vulkan's), so it never names one that is not linked in (nr_none.cpp stands in otherwise).
set(TF_BACKEND_DEFINITIONS "")
if(WIN32 OR TF_NATIVE_VKD3D)
    list(APPEND TF_BACKEND_DEFINITIONS TF_NATIVE_RENDERER=1)
endif()
if(TF_VULKAN)
    list(APPEND TF_BACKEND_DEFINITIONS TF_VULKAN=1)
endif()
if(TF_BACKEND_DEFINITIONS)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/native_render/nr_backend.cpp" PROPERTIES
        COMPILE_DEFINITIONS "${TF_BACKEND_DEFINITIONS}")
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
    target_sources(TransformersRecomp PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp"
                                              "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_overlay.cpp")
    set_source_files_properties("${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp"
                                "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_overlay.cpp"
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

# (TODO 299, the temporal upscaler) AMD's FidelityFX SDK: the API's headers are vendored (native_render/ext/fidelityfx, MIT);
# its two signed DLLs (the loader and the upscaler, FSR 3.1 / FSR 4) are never committed: fetched here from AMD's own
# repository at a pinned release with their SHA-256, and put next to the game (THIRD_PARTY_NOTICES.md has AMD's notice).
# On by default (the user's decision, 2026-11-12: every build offers FSR 3 / 4, still off in the settings); they are fetched
# once a release - into a folder named after it - and again only when the pin below changes. A failed fetch does not fail
# the build: a warning, and the game simply has no FSR 3 / 4 (it loads the loader at run time and offers the choice only
# when both DLLs are beside it). -DTF_FSR_SDK=OFF (build.ps1 -NoFsrSdk): not fetched.
option(TF_FSR_SDK "Fetch AMD's FidelityFX SDK DLLs (FSR 3.1 / FSR 4) next to the game" ON)
if(TF_FSR_SDK AND WIN32)
    set(TF_FFX_TAG "v2.4.0")
    set(TF_FFX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/fidelityfx-${TF_FFX_TAG}")
    set(TF_FFX_FILES
        "amd_fidelityfx_loader_dx12.dll|e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb"
        "amd_fidelityfx_upscaler_dx12.dll|aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace")
    set(TF_FFX_OK ON)
    foreach(entry IN LISTS TF_FFX_FILES)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 name)
        list(GET parts 1 hash)
        if(EXISTS "${TF_FFX_DIR}/${name}")
            file(SHA256 "${TF_FFX_DIR}/${name}" have)
            if(NOT have STREQUAL hash)
                file(REMOVE "${TF_FFX_DIR}/${name}") # (not the pinned file: fetched again)
            endif()
        endif()
        if(NOT EXISTS "${TF_FFX_DIR}/${name}")
            message(STATUS "FidelityFX SDK ${TF_FFX_TAG}: fetching ${name}")
            # (into a part file, renamed only once whole and checked: a failed or cut download leaves nothing behind)
            file(DOWNLOAD "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/${TF_FFX_TAG}/Kits/FidelityFX/signedbin/${name}"
                 "${TF_FFX_DIR}/${name}.part" TLS_VERIFY ON STATUS fetched)
            list(GET fetched 0 code)
            if(code EQUAL 0)
                # (checked here, not by EXPECTED_HASH: with that, a failed download is a CMake error, not a warning)
                file(SHA256 "${TF_FFX_DIR}/${name}.part" got)
                if(NOT got STREQUAL hash)
                    set(fetched "its SHA-256 is ${got}, not the pinned ${hash}")
                    set(code 1)
                endif()
            endif()
            if(code EQUAL 0)
                file(RENAME "${TF_FFX_DIR}/${name}.part" "${TF_FFX_DIR}/${name}")
            else()
                file(REMOVE "${TF_FFX_DIR}/${name}.part")
                message(WARNING "FidelityFX SDK: ${name} could not be fetched (${fetched}). The game builds without "
                                "FSR 3 / 4; the next build tries again.")
                set(TF_FFX_OK OFF)
            endif()
        endif()
    endforeach()
    if(TF_FFX_OK)
        foreach(entry IN LISTS TF_FFX_FILES)
            string(REPLACE "|" ";" parts "${entry}")
            list(GET parts 0 name)
            add_custom_command(TARGET TransformersRecomp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TF_FFX_DIR}/${name}" "$<TARGET_FILE_DIR:TransformersRecomp>/${name}")
        endforeach()
    endif()
endif()

