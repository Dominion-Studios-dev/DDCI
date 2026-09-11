cmake_minimum_required(VERSION 3.20)
project(DDCI VERSION 1.1.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE Release)
endif()

# ==========================================================================
# Aggressive performance tuning (Release builds)
#
#   -O3            maximum optimization
#   -march=native  tune codegen for this machine (host ISA extensions)
#   -mavx2         enable AVX2 intrinsic support (SIMD sanitizer path)
#   -flto          whole-program link-time optimization
#   -DNDEBUG       drop assertions / debug checks
#
# The final ddci executable is additionally stripped (-s) at LINK time via a
# target property below, yielding the minimal 1.4 MB deployment binary.
# Applied to every target above (all static libraries + ddci binary).
# ==========================================================================
include(CheckIPOSupported)
check_ipo_supported(RESULT _DDCI_IPO_SUPPORTED OUTPUT _DDCI_IPO_ERROR)

if(CMAKE_BUILD_TYPE MATCHES "Release")
    if(_DDCI_IPO_SUPPORTED)
        set(CMAKE_CXX_FLAGS_RELEASE
            "-O3 -march=native -mavx2 -flto -DNDEBUG")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        message(STATUS "DDCI: release flags -O3 -march=native -mavx2 "
                       "-flto -DNDEBUG with full LTO enabled")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE
            "-O3 -march=native -mavx2 -DNDEBUG")
        message(WARNING "LTO unavailable (${_DDCI_IPO_ERROR}); "
                        "building without -flto")
    endif()
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ==========================================================================
# Dependencies (FetchContent) — all static
# ==========================================================================
include(FetchContent)

# Force static builds before FetchContent_MakeAvailable so cpr and curl
# are compiled as .a archives with zero .so runtime dependencies.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static libraries" FORCE)
set(CPR_BUILD_TESTS OFF CACHE BOOL "Disable CPR tests" FORCE)
set(CPR_GENERATE_COVERAGE OFF CACHE BOOL "Disable coverage" FORCE)
set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "Use embedded curl" FORCE)

FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG        1.9.9
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cpr)

# ==========================================================================
# Mimalloc — global drop-in allocator override.
#
# Linked as a STATIC library into the final ddci executable. With
# MI_OVERRIDE=ON it defines strong malloc/free/realloc symbols that
# interpose libc for EVERY library in the binary (curl, cpr, OpenSSL,
# SQLite, nlohmann, and libstdc++ operator new/delete which routes through
# malloc). Nothing else needs a change — the whole process becomes a
# mimalloc process.
# ==========================================================================
set(MI_OVERRIDE ON CACHE BOOL "Override malloc/free process-wide" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "Static only" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "Build static library" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "No mimalloc tests" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "No separate object archive" FORCE)

FetchContent_Declare(
    mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG        v2.1.9
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(mimalloc)

# ==========================================================================
# Core library (config.cpp, UI render engine, Git workspace telemetry)
# ==========================================================================
add_library(ddci_core STATIC
    src/core/config.cpp
    src/core/vibe.cpp
    src/ui/ui_render.cpp
    src/workspace/workspace_context.cpp
)

target_include_directories(ddci_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ==========================================================================
# Storage library (SQLite wrapper)
# ==========================================================================
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
find_package(Threads REQUIRED)

add_library(ddci_storage STATIC
    src/storage/db_client.cpp
)

target_include_directories(ddci_storage PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ddci_storage PUBLIC
    ddci_core
    PkgConfig::SQLITE3
)

# ==========================================================================
# Memory library (L1/L2/L3 pipeline coordinator)
#   - memory_manager  : L1/L2/L3 staged pipeline (engine)
#   - context_manager : sliding-window L1 with async L3 offload (REPL)
#   - pruner          : deterministic context compression engine
#   - simd_cache      : AVX2 SIMD semantic cache (FMA cosine kernel; relies
#                       on the global -march=native -mavx2 flags in the
#                       Release tuning block above)
# ==========================================================================
add_library(ddci_memory STATIC
    src/memory/memory_manager.cpp
    src/memory/context_manager.cpp
    src/memory/pruner.cpp
    src/memory/simd_cache.cpp
)

target_include_directories(ddci_memory PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ddci_memory PUBLIC
    ddci_core
    ddci_storage
    Threads::Threads
)

# ==========================================================================
# Telemetry library (usage tracker, billing CSV, mmap logger)
# ==========================================================================
add_library(ddci_telemetry STATIC
    src/telemetry/usage_tracker.cpp
    src/telemetry/mmap_logger.cpp
)

target_include_directories(ddci_telemetry PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ddci_telemetry PUBLIC
    ddci_core
)

# ==========================================================================
# Net library (Tier 0-2: request timing, persistent connections, health)
# ==========================================================================
add_library(ddci_net STATIC
    src/net/request_timing.cpp
    src/net/connection_manager.cpp
    src/net/endpoint_health.cpp
    src/net/timing_log.cpp
)

target_include_directories(ddci_net PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ddci_net PUBLIC
    ddci_core
    cpr::cpr
)

# ==========================================================================
# Runtime library (Arena scratchpad, SIMD matcher, speculative DAG engine)
#   - ddci/mem/arena.hpp     : header-only thread-local bump arena + ArenaWriter
#   - core/simd_matcher      : AVX-512/AVX2/NEON byte-scan and cosine kernels
#   - runtime/dag_executor   : lock-free speculative DAG (atomic bitmasks,
#                              MPMC ring, zero-copy arena slabs)
# ==========================================================================
add_library(ddci_runtime STATIC
    src/core/simd_matcher.cpp
    src/runtime/dag_executor.cpp
)

target_include_directories(ddci_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ddci_runtime PUBLIC
    ddci_core
    Threads::Threads
)

# ==========================================================================
# Network library (cpr HTTP client)
# ==========================================================================
add_library(ddci_network STATIC
    src/network/api_client.cpp
)

target_include_directories(ddci_network PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ddci_network PUBLIC
    ddci_core
    ddci_net
    ddci_telemetry
    ddci_security
    ddci_memory
    ddci_runtime
    cpr::cpr
)

# ==========================================================================
# Security library (sanitizer, secure erase)
# ==========================================================================
add_library(ddci_security STATIC
    src/security/sanitizer.cpp
)

target_include_directories(ddci_security PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ==========================================================================
# Governor library (rate limiter)
# ==========================================================================
add_library(ddci_governor STATIC
    src/governor/rate_governor.cpp
)

target_include_directories(ddci_governor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ==========================================================================
# Engine library (Agent orchestrator, Graph-of-Thought multi-branch)
# ==========================================================================
add_library(ddci_engine STATIC
    src/engine/agent.cpp
    src/engine/got_engine.cpp
)

target_include_directories(ddci_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ddci_engine PUBLIC
    ddci_core
    ddci_storage
    ddci_memory
    ddci_network
    ddci_telemetry
)

# ==========================================================================
# DDCI executable
# ==========================================================================
add_executable(ddci
    src/main.cpp
)

# Strip the final binary in Release builds (deployment footprint: ~1.4 MB).
# Applied only at link time so intermediate object/archive compilation is
# not affected by the link-only -s flag.
if(CMAKE_BUILD_TYPE MATCHES "Release")
    set_target_properties(ddci PROPERTIES LINK_FLAGS_RELEASE "-s")
endif()

target_link_libraries(ddci PRIVATE
    ddci_core
    ddci_storage
    ddci_memory
    ddci_network
    ddci_net
    ddci_engine
    ddci_security
    ddci_telemetry
    ddci_governor
    ddci_runtime
    mimalloc-static
    cpr::cpr
)

# ==========================================================================
# Install targets
# ==========================================================================
install(TARGETS ddci_core ddci_storage ddci_memory ddci_network ddci_net ddci_engine ddci_security ddci_telemetry ddci_governor ddci_runtime ddci
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/core/ DESTINATION include/ddci/core FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/ui/ DESTINATION include/ddci/ui FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/workspace/ DESTINATION include/ddci/workspace FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/storage/ DESTINATION include/ddci/storage FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/memory/ DESTINATION include/ddci/memory FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/network/ DESTINATION include/ddci/network FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/engine/ DESTINATION include/ddci/engine FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/security/ DESTINATION include/ddci/security FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/telemetry/ DESTINATION include/ddci/telemetry FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/governor/ DESTINATION include/ddci/governor FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/ddci/net/ DESTINATION include/ddci/net FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/ddci/mem/ DESTINATION include/ddci/mem FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/ddci/core/ DESTINATION include/ddci/core FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/ddci/runtime/ DESTINATION include/ddci/runtime FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/utils/ DESTINATION include/ddci/utils FILES_MATCHING PATTERN "*.hpp")
