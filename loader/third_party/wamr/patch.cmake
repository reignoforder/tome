# SPDX-License-Identifier: GPL-3.0-or-later
# Tome's patches to WAMR 2.4.5 (TOME.md, "Patches"), run in WAMR's source
# folder: cmake -P patch.cmake. Each replaces one exact piece of text, and
# fails if it isn't there (a different WAMR); a file already patched is left
# as it is.

function(tome_patch file old new)
    file(READ "${file}" text)
    string(FIND "${text}" "${new}" done)
    if(NOT done EQUAL -1)
        return()
    endif()
    string(FIND "${text}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "WAMR patch: ${file} doesn't have the text Tome patches")
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    file(WRITE "${file}" "${text}")
endfunction()

set(init core/shared/platform/windows/platform_init.c)
set(internal core/shared/platform/windows/platform_internal.h)

# 1. MinGW has its own ssize_t (int on 32-bit, where SSIZE_T is long).
tome_patch(${internal}
"typedef SSIZE_T ssize_t;
"
"#ifndef __MINGW32__ /* MinGW has its own (int on 32-bit, not SSIZE_T's long) */
typedef SSIZE_T ssize_t;
#endif
")

# 2. No Winsock: only WASI uses sockets.
tome_patch(${init}
"    if (init_winsock() != 0) {
        return -1;
    }
"
"#ifndef TOME_WAMR_NO_SOCKETS /* Tome: no sockets without WASI */
    if (init_winsock() != 0) {
        return -1;
    }
#endif
")
tome_patch(${init}
"    deinit_winsock();

    os_thread_sys_destroy();
"
"#ifndef TOME_WAMR_NO_SOCKETS
    deinit_winsock();
#endif

    os_thread_sys_destroy();
")

# 3. GCC ignores __declspec(thread) on MinGW (a warning): every thread-local
#    would be one global.
tome_patch(${internal}
"#define os_thread_local_attribute __declspec(thread)
"
"#if defined(__MINGW32__) && defined(__GNUC__)
/* GCC ignores __declspec(thread) on MinGW (a warning, not an error): every
   \"thread-local\" would be one global, e.g. the cached stack boundary. */
#define os_thread_local_attribute __thread
#else
#define os_thread_local_attribute __declspec(thread)
#endif
")
