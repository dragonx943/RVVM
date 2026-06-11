/*
dlib.c - Dynamic library loader
Copyright (C) 2023  LekKit <github.com/LekKit>
                    0xCatPKG <0xCatPKG@rvvm.dev>
                    0xCatPKG <github.com/0xCatPKG>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "feature_test.h"

#include "dlib.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

#if !defined(USE_NO_DLIB) && defined(HOST_TARGET_WIN32)

#include <windows.h>

#define DLIB_WIN32_IMPL 1

#elif !defined(USE_NO_DLIB) && defined(HOST_TARGET_POSIX) && !defined(HOST_TARGET_EMSCRIPTEN)

#include <dlfcn.h>

#define DLIB_POSIX_IMPL 1

#if defined(HOST_TARGET_COSMOPOLITAN)

// Support Cosmopolitan libc & foreign ABI (MSABI, etc) via cosmo_dltramp()
#define dlopen(lib, flags) cosmo_dlopen(lib, flags)
#define dlsym(lib, symbol) cosmo_dltramp(cosmo_dlsym(lib, symbol))
#define dlclose(lib)       cosmo_dlclose(lib)

#endif

#endif

struct dlib_ctx {
#if defined(DLIB_WIN32_IMPL)
    HMODULE handle;
#elif defined(DLIB_POSIX_IMPL)
    void* handle;
#endif
    uint32_t flags;
};

#if defined(DLIB_WIN32_IMPL) || defined(DLIB_POSIX_IMPL)

static dlib_ctx_t* dlib_open_internal(const char* lib_name, uint32_t flags)
{
#if defined(DLIB_WIN32_IMPL)
    uint16_t* lib_name_u16 = utf8_to_utf16(lib_name);
    if (lib_name_u16) {
        // Try to get module from already loaded modules
        HMODULE handle = GetModuleHandleW(lib_name_u16);
#if defined(HOST_TARGET_WIN9X)
        if (!handle && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED) {
            // Try ANSI GetModuleHandleA() (Win9x compat)
            handle = GetModuleHandleA(lib_name);
        }
#endif
        if (handle) {
            // Prevent unloading an existing module
            flags &= ~DLIB_MAY_UNLOAD;
        }
        if (!handle) {
            handle = LoadLibraryW(lib_name_u16);
        }
#if defined(HOST_TARGET_WIN9X)
        if (!handle && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED) {
            // Try ANSI LoadLibraryA() (Win9x compat)
            handle = LoadLibraryA(lib_name);
        }
#endif
        safe_free(lib_name_u16);
        if (handle) {
            dlib_ctx_t* lib = safe_new_obj(dlib_ctx_t);
            lib->handle     = handle;
            lib->flags      = flags;
            return lib;
        }
    }
#elif defined(DLIB_POSIX_IMPL)
    void* handle = dlopen(lib_name, RTLD_LAZY | RTLD_LOCAL);
    if (handle) {
        dlib_ctx_t* lib = safe_new_obj(dlib_ctx_t);
        lib->handle     = handle;
        lib->flags      = flags;
        return lib;
    }
#else
    UNUSED(lib_name);
    UNUSED(flags);
#endif

    return NULL;
}

static dlib_ctx_t* dlib_open_named(const char* prefix, const char* lib_name, const char* suffix, uint32_t flags)
{
    char   name[256]  = {0};
    size_t off        = rvvm_strlcpy(name, prefix, sizeof(name));
    off              += rvvm_strlcpy(name + off, lib_name, sizeof(name) - off);
    rvvm_strlcpy(name + off, suffix, sizeof(name) - off);
    return dlib_open_internal(name, flags);
}

#define DLIB_PROBE_INTERNAL(lib_name, flags)                                                                           \
    do {                                                                                                               \
        dlib_ctx_t* lib = dlib_open_internal(lib_name, flags);                                                         \
        if (lib) {                                                                                                     \
            return lib;                                                                                                \
        }                                                                                                              \
    } while (0)

#define DLIB_PROBE_NAMED(prefix, lib_name, suffix, flags)                                                              \
    do {                                                                                                               \
        dlib_ctx_t* lib = dlib_open_named(prefix, lib_name, suffix, flags);                                            \
        if (lib) {                                                                                                     \
            return lib;                                                                                                \
        }                                                                                                              \
    } while (0)

#endif

dlib_ctx_t* dlib_open(const char* lib_name, uint32_t flags)
{
#if defined(DLIB_WIN32_IMPL) || defined(DLIB_POSIX_IMPL)
    if (lib_name) {
        DLIB_PROBE_INTERNAL(lib_name, flags);

        if ((flags & DLIB_NAME_PROBE) && !rvvm_strfind(lib_name, "/")) {
            DLIB_PROBE_NAMED("lib", lib_name, ".so", flags);
            DLIB_PROBE_NAMED("lib", lib_name, ".dll", flags);
            DLIB_PROBE_NAMED("lib", lib_name, ".dylib", flags);
            DLIB_PROBE_NAMED("", lib_name, ".so", flags);
            DLIB_PROBE_NAMED("", lib_name, ".dll", flags);
            DLIB_PROBE_NAMED("", lib_name, ".dylib", flags);
        }
    }
#else
    UNUSED(lib_name);
    UNUSED(flags);
#endif

    return NULL;
}

void dlib_close(dlib_ctx_t* lib)
{
    // Silently ignore load error
    if (lib && (lib->flags & DLIB_MAY_UNLOAD)) {
        rvvm_info("Unloading a library");
#if defined(DLIB_WIN32_IMPL)
        FreeLibrary(lib->handle);
#elif defined(DLIB_POSIX_IMPL)
        dlclose(lib->handle);
#endif
    }
    safe_free(lib);
}

void* dlib_resolve(dlib_ctx_t* lib, const char* symbol_name)
{
    void* ret = NULL;
    UNUSED(symbol_name);
    if (lib) {
#if defined(DLIB_WIN32_IMPL) && defined(HOST_TARGET_WINCE)
        uint16_t* symbol_u16 = utf8_to_utf16(symbol_name);
        if (symbol_u16) {
            ret = (void*)GetProcAddressW(lib->handle, symbol_u16);
        }
#elif defined(DLIB_WIN32_IMPL)
        ret = (void*)GetProcAddress(lib->handle, symbol_name);
#elif defined(DLIB_POSIX_IMPL)
        ret = (void*)dlsym(lib->handle, symbol_name);
#endif
    }
    return ret;
}

void* dlib_get_symbol(const char* lib_name, const char* symbol_name)
{
    if (lib_name) {
        dlib_ctx_t* lib    = dlib_open(lib_name, 0);
        void*       symbol = dlib_resolve(lib, symbol_name);
        dlib_close(lib);
        return symbol;
    }
#if defined(DLIB_POSIX_IMPL) && defined(RTLD_DEFAULT)
    // Resolve a global symbol
    return (void*)dlsym(RTLD_DEFAULT, symbol_name);
#else
    return NULL;
#endif
}

POP_OPTIMIZATION_SIZE
