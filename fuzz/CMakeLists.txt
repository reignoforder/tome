# SPDX-License-Identifier: GPL-3.0-or-later
# libFuzzer targets for the core parsers that read untrusted bytes (lobby
# chat lines, packages, tome.mod.json, map scripts and paths, the update
# check's reply). Host build with clang, separate from the mingw build:
#   cmake -S fuzz -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build-fuzz && build-fuzz/fuzz_handshake fuzz/corpus/handshake
# tools/fuzz.sh builds and runs them all.
cmake_minimum_required(VERSION 3.20)
project(tome_fuzz C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "libFuzzer needs clang: -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++")
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include(${ROOT}/cmake/ccache.cmake)  # TOME_CCACHE
set(SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=undefined)
set(CORE ${ROOT}/loader/src/core)

add_library(fuzz_core STATIC
    ${CORE}/blp.cpp
    ${CORE}/chat.cpp
    ${CORE}/desync.cpp
    ${CORE}/edition.cpp
    ${CORE}/frames.cpp
    ${CORE}/handshake.cpp
    ${CORE}/jass.cpp
    ${CORE}/json.cpp
    ${CORE}/keys.cpp
    ${CORE}/log_view.cpp
    ${CORE}/mappings.cpp
    ${CORE}/metadata.cpp
    ${CORE}/metadata_check.cpp
    ${CORE}/netmsg.cpp
    ${CORE}/lobbymsg.cpp
    ${CORE}/objects.cpp
    ${CORE}/package.cpp
    ${CORE}/path_guard.cpp
    ${CORE}/popup.cpp
    ${CORE}/resolver.cpp
    ${CORE}/settings.cpp
    ${CORE}/signature.cpp
    ${CORE}/update_check.cpp
    ${CORE}/version.cpp
    ${CORE}/wasm_abi.cpp
    ${CORE}/wasm_layout.cpp
    ${CORE}/zip.cpp
)
# Monocypher, compiled as C++ (as in loader/, which has no C compiler).
set(MONOCYPHER ${ROOT}/loader/third_party/monocypher)
set_source_files_properties(${MONOCYPHER}/monocypher.c ${MONOCYPHER}/monocypher-ed25519.c PROPERTIES LANGUAGE CXX)
target_sources(fuzz_core PRIVATE ${MONOCYPHER}/monocypher.c ${MONOCYPHER}/monocypher-ed25519.c)
target_include_directories(fuzz_core PUBLIC ${ROOT}/loader/src ${ROOT}/sdk/include ${MONOCYPHER})
target_compile_options(fuzz_core PUBLIC -g -O1 -fno-exceptions -fno-rtti ${SANITIZERS} -fsanitize=fuzzer-no-link)
target_link_options(fuzz_core PUBLIC ${SANITIZERS})

foreach(name handshake netmsg lobbymsg desync chat zip inflate json jass paths blp signature update wasm_abi wasm_layout)
    add_executable(fuzz_${name} fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE fuzz_core)
    target_compile_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
endforeach()

# WAMR (the loader's download, loader/third_party/wamr/wamr.cmake) with the
# loader's configuration (loader/CMakeLists.txt, target wamr) but for the
# host: x86-64 Linux, with WAMR's Linux platform files and its x86-64
# invokeNative. Instrumented like the rest, so the fuzzer sees its
# coverage. fuzz_wamr: a module through load, Tome's import check,
# instantiation and calls.
include(${ROOT}/loader/third_party/wamr/wamr.cmake)
add_library(fuzz_wamr_runtime STATIC
    ${WAMR}/iwasm/common/arch/invokeNative_em64.s
    ${WAMR}/iwasm/common/wasm_application.c
    ${WAMR}/iwasm/common/wasm_blocking_op.c
    ${WAMR}/iwasm/common/wasm_c_api.c
    ${WAMR}/iwasm/common/wasm_exec_env.c
    ${WAMR}/iwasm/common/wasm_loader_common.c
    ${WAMR}/iwasm/common/wasm_memory.c
    ${WAMR}/iwasm/common/wasm_native.c
    ${WAMR}/iwasm/common/wasm_runtime_common.c
    ${WAMR}/iwasm/common/wasm_shared_memory.c
    ${WAMR}/iwasm/interpreter/wasm_interp_fast.c
    ${WAMR}/iwasm/interpreter/wasm_loader.c
    ${WAMR}/iwasm/interpreter/wasm_runtime.c
    ${WAMR}/shared/mem-alloc/ems/ems_alloc.c
    ${WAMR}/shared/mem-alloc/ems/ems_gc.c
    ${WAMR}/shared/mem-alloc/ems/ems_hmu.c
    ${WAMR}/shared/mem-alloc/ems/ems_kfc.c
    ${WAMR}/shared/mem-alloc/mem_alloc.c
    ${WAMR}/shared/platform/common/posix/posix_blocking_op.c
    ${WAMR}/shared/platform/common/posix/posix_malloc.c
    ${WAMR}/shared/platform/common/posix/posix_memmap.c
    ${WAMR}/shared/platform/common/posix/posix_sleep.c
    ${WAMR}/shared/platform/common/posix/posix_thread.c
    ${WAMR}/shared/platform/common/posix/posix_time.c
    ${WAMR}/shared/platform/linux/platform_init.c
    ${WAMR}/shared/utils/bh_assert.c
    ${WAMR}/shared/utils/bh_bitmap.c
    ${WAMR}/shared/utils/bh_common.c
    ${WAMR}/shared/utils/bh_hashmap.c
    ${WAMR}/shared/utils/bh_leb128.c
    ${WAMR}/shared/utils/bh_list.c
    ${WAMR}/shared/utils/bh_log.c
    ${WAMR}/shared/utils/bh_queue.c
    ${WAMR}/shared/utils/bh_vector.c
    ${WAMR}/shared/utils/runtime_timer.c
)
target_include_directories(fuzz_wamr_runtime PRIVATE
    ${WAMR}/iwasm/interpreter
    ${WAMR}/iwasm/common
    ${WAMR}/shared/platform/linux
    ${WAMR}/shared/platform/include
    ${WAMR}/shared/mem-alloc
    ${WAMR}/shared/utils
)
target_include_directories(fuzz_wamr_runtime PUBLIC ${WAMR}/iwasm/include)
# loader/CMakeLists.txt's list; the platform and target differ (and
# mremap, which Linux has).
target_compile_definitions(fuzz_wamr_runtime PRIVATE
    BH_FREE=wasm_runtime_free
    BH_MALLOC=wasm_runtime_malloc
    BH_PLATFORM_LINUX
    BUILD_TARGET_X86_64
    WASM_HAVE_MREMAP=1
    _GNU_SOURCE
    WASM_DISABLE_HW_BOUND_CHECK=1
    WASM_DISABLE_STACK_HW_BOUND_CHECK=1
    WASM_DISABLE_WAKEUP_BLOCKING_OP=0
    WASM_ENABLE_AOT_INTRINSICS=0
    WASM_ENABLE_BULK_MEMORY=1
    WASM_ENABLE_EXTENDED_CONST_EXPR=0
    WASM_ENABLE_FAST_INTERP=1
    WASM_ENABLE_INSTRUCTION_METERING=1
    WASM_ENABLE_INTERP=1
    WASM_ENABLE_MINI_LOADER=0
    WASM_ENABLE_MULTI_MODULE=0
    WASM_ENABLE_QUICK_AOT_ENTRY=0
    WASM_ENABLE_REF_TYPES=1
    WASM_ENABLE_SHARED_MEMORY=0
    WASM_ENABLE_SHRUNK_MEMORY=1
    WASM_GLOBAL_HEAP_SIZE=10485760
    BH_VPRINTF=tome_wamr_vprintf
)
set_target_properties(fuzz_wamr_runtime PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
# Without UBSan's alignment check: the fast interpreter reads and writes its
# compiled code unaligned on purpose where the CPU allows it (x86, Tome's
# target too: WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS).
target_compile_options(fuzz_wamr_runtime PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-g -O1 ${SANITIZERS} -fno-sanitize=alignment -fsanitize=fuzzer-no-link>)
set_source_files_properties(${WAMR}/iwasm/common/arch/invokeNative_em64.s PROPERTIES
    COMPILE_OPTIONS -Wno-unused-command-line-argument)
target_link_options(fuzz_wamr_runtime PUBLIC ${SANITIZERS} -pthread)

add_executable(fuzz_wamr fuzz_wamr.cpp ${CORE}/wasm_alloc.cpp)
target_include_directories(fuzz_wamr PRIVATE ${ROOT}/loader/src)
target_link_libraries(fuzz_wamr PRIVATE fuzz_wamr_runtime m)
target_compile_options(fuzz_wamr PRIVATE -g -O1 -fno-exceptions -fno-rtti ${SANITIZERS} -fsanitize=fuzzer)
target_link_options(fuzz_wamr PRIVATE -fsanitize=fuzzer)
