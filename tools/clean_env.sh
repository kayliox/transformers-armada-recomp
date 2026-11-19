# (build robustness) Settings in this PC's environment that would change what CMake or the Python tools do here and
# nowhere else, left out of the build / setup / update (and of every program they start; the PC is not changed), and
# said once. The Linux twin of build.ps1's list, narrower on purpose: CC / CXX / CFLAGS / CXXFLAGS / LDFLAGS and
# CMAKE_PREFIX_PATH are a Linux user's (or a packager's) way to choose the compiler and the libraries, and build.sh
# honours them; what is left out here never belongs to this build:
#   CMAKE_TOOLCHAIN_FILE, CMAKE_GENERATOR*, CMAKE_PROJECT_INCLUDE*, CMAKE_PROJECT_TOP_LEVEL_INCLUDES (another project's
#   toolchain or generator: build.sh names its own, Ninja), PYTHONPATH / PYTHONHOME (another Python's modules or home
#   break the tools' Python).
# Sourced by build.sh, setup.sh and update.sh: tfa_clean_env [who]  (who: the word in the line it prints, e.g. "build").
tfa_clean_env() {
    local v left=()
    for v in CMAKE_TOOLCHAIN_FILE CMAKE_GENERATOR CMAKE_GENERATOR_PLATFORM CMAKE_GENERATOR_TOOLSET CMAKE_GENERATOR_INSTANCE \
             CMAKE_PROJECT_INCLUDE CMAKE_PROJECT_INCLUDE_BEFORE CMAKE_PROJECT_TOP_LEVEL_INCLUDES PYTHONPATH PYTHONHOME; do
        if [ -n "${!v:-}" ]; then left+=("$v"); unset "$v"; fi
    done
    if [ "${#left[@]}" -gt 0 ]; then
        echo "  (settings of this PC's environment left out of the ${1:-build}: ${left[*]})"
    fi
    return 0
}
