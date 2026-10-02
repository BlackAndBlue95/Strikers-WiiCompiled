cmake_minimum_required(VERSION 3.25)
project(strikers_wiicompiled)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WiiCompiled requires a 64-bit Clang toolchain")
endif()

if(WIN32 AND MINGW AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    set(MKW_PLATFORM_WINDOWS TRUE)
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|ARM64)$")
    # The first native macOS target is Apple Silicon. Intel and universal
    # binaries remain future compatibility work; do not silently claim them.
    set(MKW_PLATFORM_MACOS TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|aarch64|arm64|ARM64)$")
    set(MKW_PLATFORM_LINUX TRUE)
else()
    message(FATAL_ERROR
        "WiiCompiled supports 64-bit LLVM-MinGW Clang on Windows, native Linux x86_64/aarch64, or Apple Clang on macOS arm64")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "WiiCompiled only supports Release builds")
endif()

option(MKW_BUILD_PRODUCTS "Build translated WiiCompiled product targets" ON)

# Preprocessor definitions that belong to this project's own code (the runtime,
# the translated shards and the product glue) and to nothing else. They are
# applied directory-scoped, immediately after the aurora add_subdirectory() call,
# so every target created afterwards (mkw_runtime_common, the translated shard
# libraries and the products in cmake/PublicProducts.cmake) inherits them while
# aurora-main and its third-party tree stay unaffected.
set(MKW_PROJECT_COMPILE_DEFINITIONS NOMINMAX)

set(CMAKE_CXX_STANDARD 17)

# Linux guest-fiber scheduling (runtime/src/host_context.cpp) needs a symmetric
# stackful-coroutine primitive to stand in for Win32 Fibers. libco's co_switch() transfers
# directly to any other created coroutine, matching SwitchToFiber's semantics exactly (unlike
# asymmetric resume/yield coroutine libraries, which would need every call site restructured).
# Vendored from upstream (higan-emu/libco @ e18e09d, 2019-10-16, ISC license; valgrind.h is
# separately BSD-style licensed, see third_party/libco/LICENSE). Windows keeps native Fibers
# and macOS uses the project's x18-safe AArch64 assembly backend, so this target is Linux-only.
if(MKW_PLATFORM_LINUX)
    add_library(mkw_libco STATIC third_party/libco/libco.c)
    add_library(mkw::libco ALIAS mkw_libco)
    target_include_directories(mkw_libco PUBLIC third_party/libco)
    set_target_properties(mkw_libco PROPERTIES UNITY_BUILD OFF)
endif()

# Runtime configuration is real TOML, parsed by toml11 rather than a project-
# specific line parser. Keep it header-only and vendored so disconnected release
# builds have exactly the same parser as developer builds.
add_library(mkw_toml11 INTERFACE)
add_library(mkw::toml11 ALIAS mkw_toml11)
target_include_directories(mkw_toml11 SYSTEM INTERFACE third_party/toml11)
target_compile_features(mkw_toml11 INTERFACE cxx_std_17)

# Riivolution XML is parsed with pugixml, matching the parser used by Dolphin.
# The four upstream files from v1.15 (ee86beb30e4973f5feffe3ce63bfa4fbadf72f38)
# are vendored so builds remain usable with FETCHCONTENT_FULLY_DISCONNECTED.
add_library(mkw_pugixml STATIC third_party/pugixml/pugixml.cpp)
add_library(mkw::pugixml ALIAS mkw_pugixml)
target_include_directories(mkw_pugixml PUBLIC third_party/pugixml)
target_compile_features(mkw_pugixml PUBLIC cxx_std_17)
set_target_properties(mkw_pugixml PROPERTIES UNITY_BUILD OFF)

# TLS for non-Windows guest network HLE (runtime/src/hle/net/network_ssl.cpp) - the Windows path
# uses Schannel (a Windows-only OS API), which has no equivalent on Linux/Android, so this project
# needs its own TLS library there. mbed TLS was chosen over OpenSSL specifically because it cross-
# compiles cleanly for Android with nothing beyond a plain C toolchain (no perl/asm build-script
# dependency the way OpenSSL's build has), matching how this project already prefers toolchain-
# simple libraries. The build fetches a pinned upstream tarball with a checked SHA-256.
# Bump both values in cmake/MbedTLSPin.cmake when updating; the hash comes from
# upstream's signed `mbedtls-<version>-sha256sum.txt` release asset.
#
# The alias exists on every platform so the link lines in cmake/PublicProducts.cmake stay
# platform-independent, but it is only populated where network_ssl.cpp actually compiles the mbed
# TLS path (`#ifndef _WIN32`). Windows keeps Schannel and never fetches it.
add_library(mkw_mbedtls INTERFACE)
add_library(mkw::mbedtls ALIAS mkw_mbedtls)
if(NOT MKW_PLATFORM_WINDOWS)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/MbedTLSPin.cmake")
    include(FetchContent)
    FetchContent_Declare(mkw_mbedtls_upstream
        URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MKW_MBEDTLS_VERSION}/mbedtls-${MKW_MBEDTLS_VERSION}.tar.bz2"
        URL_HASH SHA256=${MKW_MBEDTLS_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    # Subproject mode already disables testing and codegen; it still enables programs.
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(mkw_mbedtls_upstream)
    target_link_libraries(mkw_mbedtls INTERFACE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
endif()

set(MKW_TRANSLATED_COMPILE_JOBS 0 CACHE STRING
    "Cap on concurrently compiling translated shard TUs via a Ninja job pool (0 = uncapped). \
Scheduling only - never affects output bytes, so it is deliberately outside the canonical flag fingerprint.")

set(MKW_AURORA_DIR "${CMAKE_CURRENT_LIST_DIR}/../aurora-main")

# The translator's explicit float casts are PPC single-precision rounding points.
# Fast-math may erase them and change guest-visible integer conversions.
set(MKW_TRANSLATED_PPC_FP_OPTIONS -fno-fast-math -ffp-contract=off)
# The handwritten runtime and aurora: fast-math, but NaN and infinity still exist. Input, config and
# texture code checks std::isfinite/std::isnan, which -ffinite-math-only (part of -ffast-math) folds
# to constants.
set(MKW_RUNTIME_FP_OPTIONS -ffast-math -fno-finite-math-only)

# ----------------------------------------------------------------------
# Third-party: aurora-main (provides SDL3 + GPU backends)
# ----------------------------------------------------------------------
if(NOT EXISTS ${MKW_AURORA_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "Requested aurora-main but ${MKW_AURORA_DIR} is missing")
endif()
set(DAWN_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
if(MKW_PLATFORM_WINDOWS)
    set(DAWN_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
    set(TINT_BUILD_HLSL_WRITER ON CACHE BOOL "" FORCE)
elseif(MKW_PLATFORM_MACOS)
    set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_METAL ON CACHE BOOL "" FORCE)
    set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
else()
    set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
    set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
endif()
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Provide a tiny stub for DXProgrammableCapture when the SDK/PIX headers are
# missing (common on MinGW). Dawn only includes the header; no symbols are
# referenced when PIX isn't present.
if(MKW_PLATFORM_WINDOWS)
    set(MKW_DX_STUB_DIR "${CMAKE_BINARY_DIR}/aurora_dx_stubs")
    if(NOT EXISTS "${MKW_DX_STUB_DIR}/DXProgrammableCapture.h")
        file(MAKE_DIRECTORY ${MKW_DX_STUB_DIR})
        file(WRITE "${MKW_DX_STUB_DIR}/DXProgrammableCapture.h"
"#pragma once\n// Stubbed PIX capture header for Dawn; no functionality when PIX isn't present.\n")
    endif()
endif()
# Deliberately NOT injected project-wide. Only a from-source Dawn build ever
# includes DXProgrammableCapture.h, and this tree consumes Dawn as a prebuilt
# package (AURORA_DAWN_PROVIDER=package), so the old
# include_directories()/add_compile_options(-I...) pair only added a dead
# -I to every one of the ~500 third-party and runtime compile lines. The
# aurora_* targets that do want it get it from the
# target_include_directories() call further down; a from-source Dawn build
# should add it to the Dawn targets the same way.

# Keep SDL3 lean when aurora is enabled.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)

# MinGW/WSL can choke on SDL precompiled headers; disable PCH for aurora/SDL.
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
# Isolate aurora build products to a subdirectory under the main build dir.
add_subdirectory(${MKW_AURORA_DIR} ${CMAKE_BINARY_DIR}/aurora-build EXCLUDE_FROM_ALL)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS OFF)

# Keep upstream Aurora split: several sources intentionally use file-local
# names that collide under this repo's aggressive unity build settings.
set(AURORA_TARGETS aurora_core aurora_gx aurora_pad aurora_si aurora_mtx aurora_vi)
foreach(t ${AURORA_TARGETS})
    if(TARGET ${t})
        set_target_properties(${t} PROPERTIES UNITY_BUILD OFF)
        target_compile_options(${t} PRIVATE -O3 ${MKW_RUNTIME_FP_OPTIONS} -w -pipe)
        if(MKW_PLATFORM_WINDOWS)
            target_include_directories(${t} PRIVATE ${MKW_DX_STUB_DIR})
        endif()
        # Aurora's own sources include Windows headers and call std::min/max;
        # they relied on the old project-wide NOMINMAX that no longer leaks
        # into this subtree, so the define is applied per target here.
        target_compile_definitions(${t} PRIVATE NOMINMAX)
    endif()
endforeach()

# Everything from here down is this project's own code, so this is where the
# project definitions enter the directory scope. A directory-scoped definition
# seeds the COMPILE_DEFINITIONS of targets as they are created, which means:
#   * aurora-main and its third-party tree were added above -> unaffected;
#   * mkw_runtime_common, the translated shard libraries and the products in
#     cmake/PublicProducts.cmake are all created below (that file is include()d,
#     not add_subdirectory()d, so it shares this directory scope) -> they pick
#     the definitions up exactly as before.
if(MKW_PROJECT_COMPILE_DEFINITIONS)
    add_compile_definitions(${MKW_PROJECT_COMPILE_DEFINITIONS})
endif()

# Runtime sources. CONFIGURE_DEPENDS costs one glob re-check per build (measured
# at ~55 ms across the 100+ globs this build already re-verifies for SDL), and it
# buys correctness: without it, a newly added src/*.cpp that is not referenced by
# a registration file is silently never compiled and never errors. The stale-glob
# failure mode is worth far more than the milliseconds.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
if(MKW_PLATFORM_MACOS)
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory.cpp")
    # HostContext's Apple Silicon backend is implemented in a small assembly
    # companion. It must be part of the product runtime as well as the
    # standalone context test; otherwise the final executable is missing
    # mkw_co_init/mkw_co_switch at link time.
    enable_language(ASM)
    list(APPEND SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
else()
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory_macos.cpp")
endif()
set(MKW_PLATFORM_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/platform/host_platform.cpp")
set(MKW_BASE_PRODUCT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/product/base_product.cpp")
# The host ISA guard is the one translation unit that must not receive the
# x86-64-v3 flags, so it gets its own object library instead of riding along in
# mkw_runtime_common. See cmake/PublicProducts.cmake and the file's own header.
set(MKW_CPU_BASELINE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/host_cpu_baseline.cpp")
list(REMOVE_ITEM SOURCES ${MKW_BASE_PRODUCT_SOURCE} ${MKW_CPU_BASELINE_SOURCE} ${MKW_PLATFORM_SOURCE})

# This deliberately small library contains host services that are safe to
# validate before guest memory and fiber work makes a full runtime build viable.
add_library(mkw_platform STATIC "${MKW_PLATFORM_SOURCE}")
target_include_directories(mkw_platform PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(mkw_platform PUBLIC cxx_std_17)
set_target_properties(mkw_platform PROPERTIES UNITY_BUILD OFF)

# Keep these independent from Aurora's BUILD_TESTING option: they validate the
# project's host-platform contracts, not Aurora's third-party test suite.
enable_testing()
add_executable(mkw_platform_paths_tests "${CMAKE_CURRENT_LIST_DIR}/tests/platform_paths_tests.cpp")
target_link_libraries(mkw_platform_paths_tests PRIVATE mkw_platform)
target_compile_features(mkw_platform_paths_tests PRIVATE cxx_std_17)
add_test(NAME mkw_platform_paths_tests COMMAND mkw_platform_paths_tests)

add_executable(mkw_nand_settings_tests "${CMAKE_CURRENT_LIST_DIR}/tests/nand_settings_tests.cpp")
find_package(Threads REQUIRED)
target_link_libraries(mkw_nand_settings_tests PRIVATE Threads::Threads)
target_include_directories(mkw_nand_settings_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(mkw_nand_settings_tests PRIVATE cxx_std_17)
add_test(NAME mkw_nand_settings_tests COMMAND mkw_nand_settings_tests)

add_executable(mkw_sc_serial_tests "${CMAKE_CURRENT_LIST_DIR}/tests/sc_serial_tests.cpp")
target_include_directories(mkw_sc_serial_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(mkw_sc_serial_tests PRIVATE cxx_std_17)
add_test(NAME mkw_sc_serial_tests COMMAND mkw_sc_serial_tests)

# The input expression engine is self-contained, so it can be exercised without
# linking the runtime or SDL.
add_executable(mkw_input_expr_tests
    "${CMAKE_CURRENT_LIST_DIR}/tests/test_expr.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/input_expr.cpp")
target_include_directories(mkw_input_expr_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(mkw_input_expr_tests PRIVATE cxx_std_17)
# The production float policy, so the tests check what ships.
target_compile_options(mkw_input_expr_tests PRIVATE -O3 ${MKW_RUNTIME_FP_OPTIONS})
add_test(NAME mkw_input_expr_tests COMMAND mkw_input_expr_tests)

# SHA-1 known answers for the emulated /dev/sha device.
add_executable(msc_sha1_tests "${CMAKE_CURRENT_LIST_DIR}/tests/sha1_tests.cpp")
target_include_directories(msc_sha1_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(msc_sha1_tests PRIVATE cxx_std_17)
add_test(NAME msc_sha1_tests COMMAND msc_sha1_tests)

# HostContext deliberately keeps the platform-specific context primitive out
# of fiber_manager.cpp. Exercise the Linux libco handoff directly so future
# refactors cannot silently remove its headers, implementation, or link edge.
if(MKW_PLATFORM_LINUX)
    add_executable(mkw_linux_host_context_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/host_context_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/host_context.cpp")
    target_include_directories(mkw_linux_host_context_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/third_party/libco")
    target_compile_features(mkw_linux_host_context_tests PRIVATE cxx_std_17)
    target_link_libraries(mkw_linux_host_context_tests PRIVATE mkw::libco)
    add_test(NAME mkw_linux_host_context_tests COMMAND mkw_linux_host_context_tests)
endif()

if(MKW_PLATFORM_MACOS)
    # Exercise the Apple Silicon context ABI and the public host-memory
    # contracts separately from translated products.
    enable_language(ASM)
    add_executable(mkw_macos_context_abi_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/macos_context_abi_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
    target_compile_features(mkw_macos_context_abi_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_macos_context_abi_tests COMMAND mkw_macos_context_abi_tests)

    add_executable(mkw_macos_host_context_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/host_context_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/host_context.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
    target_include_directories(mkw_macos_host_context_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_features(mkw_macos_host_context_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_macos_host_context_tests COMMAND mkw_macos_host_context_tests)

    add_executable(mkw_macos_guest_flat_memory_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/macos_guest_flat_memory_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory_macos.cpp")
    target_include_directories(mkw_macos_guest_flat_memory_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_features(mkw_macos_guest_flat_memory_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_macos_guest_flat_memory_tests COMMAND mkw_macos_guest_flat_memory_tests)
endif()

# The translator emits the complete, content-addressed source graph. Consuming
# this one manifest keeps configure independent of the generated function files.
if(MKW_BUILD_PRODUCTS)
    set(MKW_TRANSLATED_SHARD_MANIFEST
        "${CMAKE_CURRENT_LIST_DIR}/../generated/build_shards/shards.cmake"
        CACHE FILEPATH "Translator-owned aggregate shard manifest")
    if(NOT EXISTS "${MKW_TRANSLATED_SHARD_MANIFEST}")
        message(FATAL_ERROR
            "Missing translator-owned shard manifest: ${MKW_TRANSLATED_SHARD_MANIFEST}. "
            "Run Translator.Cli emit-build-shards first; see translator/README.md.")
    endif()
    include("${MKW_TRANSLATED_SHARD_MANIFEST}")
    message(STATUS "Translator graph: ${MKW_BASE_FUNCTION_COUNT} translated functions")

    set(MKW_RUNTIME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/PublicProducts.cmake")
else()
    if(MKW_PLATFORM_MACOS)
        # Compile-only audit of native runtime sources. It deliberately avoids
        # translated products until their host dependencies are portable.
        # These sources depend on generated/RuntimeConfig.h, which is emitted for
        # a particular game by the translator and is intentionally unavailable
        # in this platform-only configuration.
        set(MKW_MACOS_NATIVE_AUDIT_SOURCES ${SOURCES})
        list(REMOVE_ITEM MKW_MACOS_NATIVE_AUDIT_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/src/abi_bridge.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/hle/os/os_alarm.cpp")
        add_library(mkw_macos_native_compile OBJECT ${MKW_MACOS_NATIVE_AUDIT_SOURCES})
        target_include_directories(mkw_macos_native_compile PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src"
            "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../aurora-main/include")
        target_compile_features(mkw_macos_native_compile PRIVATE cxx_std_20)
        target_compile_definitions(mkw_macos_native_compile PRIVATE SDL_MAIN_HANDLED TARGET_PC)
        target_link_libraries(mkw_macos_native_compile PRIVATE
            aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx
            mkw::pugixml mkw::toml11)
        set_target_properties(mkw_macos_native_compile PROPERTIES UNITY_BUILD OFF)
    endif()
    add_custom_target(mkw_platform_paths_check DEPENDS mkw_platform)
    message(STATUS "Translated product targets disabled (MKW_BUILD_PRODUCTS=OFF)")
endif()
