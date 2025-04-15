/*
dlib.c - Dynamic library loader
Copyright (C) 2023  LekKit <github.com/LekKit>
                    0xCatPKG <0xCatPKG@rvvm.dev>
                    0xCatPKG <github.com/0xCatPKG>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "dlib.h"

#if !defined(USE_NO_DLIB) && defined(_WIN32) && !defined(UNDER_CE)
#include <windows.h>

#define DLIB_WIN32_IMPL 1

#elif !defined(USE_NO_DLIB) && !defined(__EMSCRIPTEN__)                                                                \
    && (defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__))
#include <dlfcn.h>

#define DLIB_POSIX_IMPL 1

#if defined(__COSMOPOLITAN__)
// Support Cosmopolitan libc & foreign ABI (MSABI, etc) via cosmo_dltramp()
#define dlopen(lib, flags) cosmo_dlopen(lib, flags)
#define dlsym(lib, symbol) cosmo_dltramp(cosmo_dlsym(lib, symbol))
#define dlclose(lib)       cosmo_dlclose(lib)
#endif

#endif

// RVVM internal headers come after system headers because of safe_free()
#include "utils.h"

SOURCE_OPTIMIZATION_SIZE

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
    wchar_t u16_name[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, lib_name, -1, u16_name, STATIC_ARRAY_SIZE(u16_name));

    // Try to get module from already loaded modules
    HMODULE handle = GetModuleHandleW(u16_name);
    if (handle) {
        // Prevent unloading an existing module
        flags &= ~DLIB_MAY_UNLOAD;
    } else {
        handle = LoadLibraryW(u16_name);
    }

    if (handle) {
        dlib_ctx_t* lib = safe_new_obj(dlib_ctx_t);
        lib->handle     = handle;
        lib->flags      = flags;
        return lib;
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
    free(lib);
}

void* dlib_resolve(dlib_ctx_t* lib, const char* symbol_name)
{
    void* ret = NULL;
    UNUSED(symbol_name);
    if (lib) {
#if defined(DLIB_WIN32_IMPL)
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
