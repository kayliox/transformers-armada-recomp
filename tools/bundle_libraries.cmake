cmake_minimum_required(VERSION 3.21)

# Copies the FFmpeg libraries the executable needs beside it, with their own dependencies, and the game asks the
# loader to look there first (transformers/CMakeLists.txt: BUILD_RPATH $ORIGIN, DT_RPATH).
#
# Why: on Linux the game takes its movie decoding from the FFmpeg the build found (pkg-config), so the executable
# asks for that build's libavcodec ABI. A build made in a distribution container - a Steam Deck's tfa-build, Ubuntu
# 24.04 with libavcodec.so.60 - then runs on SteamOS, which has no FFmpeg at all or another ABI (FFmpeg 8.1's .62).
# Nothing is copied when FFmpeg is linked statically: the loader then names no libav* library.
#
#   cmake -DEXECUTABLE=<path to TransformersRecomp> -DCOPY_DESTINATION=<folder> -P bundle_libraries.cmake
#
# glibc, libstdc++ and libgcc come from the host on purpose: the build machine's are the only ones that fit the
# machine that runs it. So do the graphics, audio and input stacks: on this executable's DT_RPATH the host's own GPU
# driver (Mesa, loaded at run time) looks for its dependencies in the game folder first, and the container's older
# libGL/libdrm/libxcb/ALSA would shadow the host's on a newer SteamOS and stop the game starting. FFmpeg's other
# dependencies (the codec libraries, zlib, zstd and the like) have no such host state and travel with it.

if(NOT EXISTS "${EXECUTABLE}")
    message(FATAL_ERROR "bundle_libraries: no executable at ${EXECUTABLE}")
endif()

# The C and C++ runtime, and the libraries whose drivers, plugins and configuration belong to the running machine.
set(host_libraries
    "^libc\\.so" "^libm\\.so" "^libmvec\\.so" "^libpthread\\.so" "^libdl\\.so" "^librt\\.so" "^libresolv\\.so"
    "^libutil\\.so" "^ld-linux" "^libgcc_s\\.so" "^libstdc\\+\\+\\.so"
    "^libGL" "^libEGL" "^libvulkan" "^libdrm" "^libgbm" "^libX" "^libxcb" "^libwayland" "^libasound" "^libpulse"
    "^libudev" "^libdbus" "^libva" "^libvdpau")

# ldd names "name => path" for each resolved library; a name with no path (vdso, the loader) has nothing to copy.
function(list_libraries path out_var)
    execute_process(COMMAND ldd "${path}" OUTPUT_VARIABLE output ERROR_QUIET RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "bundle_libraries: ldd failed on ${path}")
    endif()
    set(found "")
    string(REPLACE "\n" ";" lines "${output}")
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*([^ \t]+) => (/[^ \t]+)")
            list(APPEND found "${CMAKE_MATCH_1}|${CMAKE_MATCH_2}")
        elseif(line MATCHES "^[ \t]*([^ \t]+) => not found")
            # Copied as an empty path: the walk below cannot bundle what the loader cannot find.
            list(APPEND found "${CMAKE_MATCH_1}|")
        endif()
    endforeach()
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

function(is_host_library name out_var)
    set(host FALSE)
    foreach(pattern IN LISTS host_libraries)
        if("${name}" MATCHES "${pattern}")
            set(host TRUE)
            break()
        endif()
    endforeach()
    set(${out_var} ${host} PARENT_SCOPE)
endfunction()

# Start from the FFmpeg family the executable itself names; walk what those libraries name in turn.
list_libraries("${EXECUTABLE}" listed)
set(queue "")
foreach(entry IN LISTS listed)
    string(REPLACE "|" ";" pair "${entry}")
    list(GET pair 0 name)
    if(name MATCHES "^lib(av|sw|postproc)")
        list(APPEND queue "${entry}")
    endif()
endforeach()

set(copied_names "")
set(copied 0)
while(queue)
    list(POP_FRONT queue entry)
    string(REPLACE "|" ";" pair "${entry}")
    list(GET pair 0 name)
    list(GET pair 1 path)
    if(name IN_LIST copied_names)
        continue()
    endif()
    is_host_library("${name}" host)
    if(host)
        continue()
    endif()
    if(path STREQUAL "")
        message(FATAL_ERROR "bundle_libraries: ${name} is not found; run CMake with the folder holding it in "
            "LD_LIBRARY_PATH (${EXECUTABLE})")
    endif()
    # FOLLOW_SYMLINK_CHAIN: ldd names versioned libraries (libavcodec.so.60, a link to ...so.60.31.102), and a
    # plain copy would leave a dangling link in the bundle.
    file(COPY "${path}" DESTINATION "${COPY_DESTINATION}" FOLLOW_SYMLINK_CHAIN)
    if(NOT EXISTS "${COPY_DESTINATION}/${name}")
        message(FATAL_ERROR "bundle_libraries: ${name} is not usable beside the executable (dangling link)")
    endif()
    message(STATUS "bundle_libraries: ${name} beside the executable")
    list(APPEND copied_names "${name}")
    math(EXPR copied "${copied} + 1")
    # libavcodec asks for its codec libraries and so on; DT_RPATH finds those here too.
    list_libraries("${path}" dependencies)
    list(APPEND queue ${dependencies})
endwhile()
message(STATUS "bundle_libraries: ${copied} libraries copied beside the executable")
