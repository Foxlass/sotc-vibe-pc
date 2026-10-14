cmake_minimum_required(VERSION 3.21)

project(SotcPC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PS2X_BUILD_RECOMP ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
# Test tools (vu1_recomp, gs_replay, ps2x_tests). PS2Recomp is EXCLUDE_FROM_ALL, so they are only
# built when requested by name (e.g. build.bat vu1_recomp); a plain build.bat is unaffected.
option(PS2X_BUILD_TEST "Configure PS2Recomp test tools (vu1_recomp, gs_replay, tests)" ON)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_DEBUG_UI OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_FFMPEG OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)

# ---------------------------------------------------------------------------
# Build throttling (Ninja job pools). Generated game code and VU1 code compile with heavy
# optimization; one compiler per logical core exhausts RAM and stalls the desktop on 16 GB
# machines. Those compiles share a pool of min(cores / 2, RAM / 3 GB) jobs, and the /LTCG link of
# sotc.exe runs alone. Everything else keeps Ninja's default parallelism.
# ---------------------------------------------------------------------------
cmake_host_system_information(RESULT SOTC_HOST_CORES QUERY NUMBER_OF_LOGICAL_CORES)
cmake_host_system_information(RESULT SOTC_HOST_MEMORY_MB QUERY TOTAL_PHYSICAL_MEMORY)
math(EXPR SOTC_JOBS_BY_CORES "${SOTC_HOST_CORES} / 2")
math(EXPR SOTC_JOBS_BY_MEMORY "${SOTC_HOST_MEMORY_MB} / 3072")
set(SOTC_DEFAULT_HEAVY_JOBS ${SOTC_JOBS_BY_CORES})
if(SOTC_JOBS_BY_MEMORY LESS SOTC_DEFAULT_HEAVY_JOBS)
    set(SOTC_DEFAULT_HEAVY_JOBS ${SOTC_JOBS_BY_MEMORY})
endif()
if(SOTC_DEFAULT_HEAVY_JOBS LESS 1)
    set(SOTC_DEFAULT_HEAVY_JOBS 1)
endif()
set(SOTC_HEAVY_JOBS ${SOTC_DEFAULT_HEAVY_JOBS} CACHE STRING
    "Parallel compiles for generated game/VU1 code (default: min(cores/2, RAM/3GB))")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS sotc_heavy_compile=${SOTC_HEAVY_JOBS} sotc_heavy_link=1)
message(STATUS "Heavy compile jobs: ${SOTC_HEAVY_JOBS} (${SOTC_HOST_CORES} logical cores, ${SOTC_HOST_MEMORY_MB} MB RAM)")

add_subdirectory(External/PS2Recomp EXCLUDE_FROM_ALL)
add_subdirectory(Port)
