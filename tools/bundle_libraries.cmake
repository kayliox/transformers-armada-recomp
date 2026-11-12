# Copies the shared libraries the executable needs beside it, except the C and C++ runtime the host must provide,
# and the game asks the loader to look there first (transformers/CMakeLists.txt: BUILD_RPATH $ORIGIN).
#
# Why: on Linux the game takes its movie decoding from the FFmpeg the build found (pkg-config), so the executable
# asks for that build's libavcodec ABI. A build made in a distribution container - a Steam Deck's tfa-build, Ubuntu
# 24.04 with libavcodec.so.60 - then runs on SteamOS, which has no FFmpeg at all or another ABI (FFmpeg 8.1's .62).
# Nothing is copied when FFmpeg is linked statically: the loader then names no libav* library.
#
#   cmake -DEXECUTABLE=<path to TransformersRecomp> -DCOPY_DESTINATION=<folder> -P bundle_libraries.cmake
#
# glibc, libstdc++ and libgcc come from the host on purpose: the build machine's are the only ones that fit the
# machine that runs it.

if(NOT EXISTS "${EXECUTABLE}")
    message(FATAL_ERROR "bundle_libraries: no executable at ${EXECUTABLE}")
endif()

execute_process(COMMAND ldd "${EXECUTABLE}" OUTPUT_VARIABLE listed ERROR_QUIET RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "bundle_libraries: ldd failed on ${EXECUTABLE}")
endif()

# The runtime the host provides. Everything else travels with the game.
set(system_libraries libc.so libm.so libmvec.so libpthread.so libdl.so librt.so libresolv.so libutil.so ld-linux
    libgcc_s.so libstdc++.so)

string(REPLACE "\n" ";" lines "${listed}")
set(copied 0)
foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*([^ \t]+) => (/[^ \t]+)")
        set(name "${CMAKE_MATCH_1}")
        set(path "${CMAKE_MATCH_2}")
        set(keep TRUE)
        foreach(system IN LISTS system_libraries)
            string(FIND "${name}" "${system}" at)
            if(at EQUAL 0)
                set(keep FALSE)
            endif()
        endforeach()
        if(keep)
            # FOLLOW_SYMLINK_CHAIN: ldd names versioned libraries (libavcodec.so.60, a link to ...so.60.31.102), and a
            # plain copy would leave a dangling link in the bundle.
            file(COPY "${path}" DESTINATION "${COPY_DESTINATION}" FOLLOW_SYMLINK_CHAIN)
            if(NOT EXISTS "${COPY_DESTINATION}/${name}")
                message(FATAL_ERROR "bundle_libraries: ${name} is not usable beside the executable (dangling link)")
            endif()
            message(STATUS "bundle_libraries: ${name} beside the executable")
            math(EXPR copied "${copied} + 1")
        endif()
    endif()
endforeach()
message(STATUS "bundle_libraries: ${copied} libraries copied beside the executable")
