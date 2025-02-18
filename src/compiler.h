/*
compiler.h - Compilers tricks and features
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef COMPILER_H
#define COMPILER_H

#include <stdint.h>

/*
 * Compiler feature detection
 */

#if defined(__GNUC__) || defined(__clang__) || defined(__llvm__) || defined(__INTEL_COMPILER)
#define GNU_EXTS 1
#endif

// GCC version checking
#if defined(__GNUC__) && !defined(__clang__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)
#define GCC_CHECK_VER(major, minor) (__GNUC__ > major || \
        (__GNUC__ == major && __GNUC_MINOR__ >= minor))
#else
#define GCC_CHECK_VER(major, minor) 0
#endif

// Clang version checking
#if defined(__clang__)
#define CLANG_CHECK_VER(major, minor) (__clang_major__ > major || \
          (__clang_major__ == major && __clang_minor__ >= minor))
#else
#define CLANG_CHECK_VER(major, minor) 0
#endif

// Uniformly define __SANITIZE_THREAD__, __SANITIZE_ADDRESS__, __SANITIZE_MEMORY__ on Clang & GCC
#if defined(GNU_EXTS) && defined(__has_feature)
#if __has_feature(address_sanitizer) && !defined(__SANITIZE_ADDRESS__)
#define __SANITIZE_ADDRESS__
#endif
#if __has_feature(thread_sanitizer) && !defined(__SANITIZE_THREAD__)
#define __SANITIZE_THREAD__
#endif
#if __has_feature(memory_sanitizer) && !defined(__SANITIZE_MEMORY__)
#define __SANITIZE_MEMORY__
#endif
#endif

// Check GNU attribute presence
#if defined(GNU_EXTS) && defined(__has_attribute)
#define GNU_ATTRIBUTE(attr) __has_attribute(attr)
#else
#define GNU_ATTRIBUTE(attr) 0
#endif

// Check GNU builtin presence
#if defined(GNU_EXTS) && defined(__has_builtin)
#define GNU_BUILTIN(builtin) __has_builtin(builtin)
#else
#define GNU_BUILTIN(builtin) 0
#endif

// Check header presence
#if defined(__has_include)
#define CHECK_INCLUDE(include, urgent) __has_include(#include)
#elif defined(GNU_EXTS)
#define CHECK_INCLUDE(include, urgent) (urgent)
#else
#define CHECK_INCLUDE(include, urgent) 1
#endif

/*
 * Optimization hints
 */

// Branch optimization hints
#if GNU_BUILTIN(__builtin_expect)
#define likely(x)     __builtin_expect(!!(x),1)
#define unlikely(x)   __builtin_expect(!!(x),0)
#else
#define likely(x)     (x)
#define unlikely(x)   (x)
#endif

// Memory prefetch hints
#if GNU_BUILTIN(__builtin_prefetch) && !defined(NO_PREFETCH)
#define mem_prefetch(addr, rw, loc) __builtin_prefetch(addr, !!(rw), loc)
#else
#define mem_prefetch(addr, rw, loc)
#endif

// Force-inline function attribute
#if GNU_ATTRIBUTE(__always_inline__) && !defined(USE_RELAXED_INLINING) && !defined(__SANITIZE_THREAD__)
// ThreadSanitizer doesn't play well with __always_inline__
#define forceinline inline __attribute__((__always_inline__))
#elif defined(_MSC_VER) && !defined(USE_RELAXED_INLINING)
#define forceinline __forceinline
#else
#define forceinline inline
#endif

// Never inline this function
#if GNU_ATTRIBUTE(__noinline__)
#define no_inline      __attribute__((__noinline__))
#elif defined(_MSC_VER)
#define no_inline      __declspec(noinline)
#else
#define no_inline
#endif

// Never inline a function, assume it's a slow path, and minimize pessimizations at the call size
#if CLANG_CHECK_VER(17, 0) && GNU_ATTRIBUTE(__preserve_most__) && (defined(__x86_64__) || defined(__aarch64__))
/*
 * This is used to remove unnecessary register spills from algorithm fast path
 * when a slow path call is present. Hopefully one day similar thing will appear in GCC.
 *
 * This attribute is BROKEN before Clang 17 and generates broken binaries if used <17!!!
 */
#define slow_path __attribute__((__preserve_most__,__noinline__,__cold__))
#elif GNU_ATTRIBUTE(__cold__)
#define slow_path no_inline __attribute__((__cold__))
#else
#define slow_path no_inline
#endif

// Inline all function calls into the caller marked with flatten_calls. Use with care!
#if GNU_ATTRIBUTE(__flatten__)
#define flatten_calls __attribute__((__flatten__))
#else
#define flatten_calls
#endif

// Whole-source optimization pragmas (Clang doesn't support this)
#if GCC_CHECK_VER(4, 4)
#define SOURCE_OPTIMIZATION_NONE _Pragma("GCC optimize(\"O0\")")
#define SOURCE_OPTIMIZATION_O2 _Pragma("GCC optimize(\"O2\")")
#define SOURCE_OPTIMIZATION_O3 _Pragma("GCC optimize(\"O3\")")
#else
#define SOURCE_OPTIMIZATION_NONE
#define SOURCE_OPTIMIZATION_O2
#define SOURCE_OPTIMIZATION_O3
#endif
#if GCC_CHECK_VER(12, 1)
#define SOURCE_OPTIMIZATION_SIZE _Pragma("GCC optimize(\"Oz\")")
#elif GCC_CHECK_VER(4, 4)
#define SOURCE_OPTIMIZATION_SIZE _Pragma("GCC optimize(\"Os\")")
#else
#define SOURCE_OPTIMIZATION_SIZE
#endif

// Pushable size optimization attribute, Clang supports this to some degree
#if CLANG_CHECK_VER(12, 0) && GNU_ATTRIBUTE(__minsize__)
#define PUSH_OPTIMIZATION_SIZE _Pragma("clang attribute push (__attribute__((__minsize__)), apply_to=function)")
#define POP_OPTIMIZATION_SIZE _Pragma("clang attribute pop")
#elif GCC_CHECK_VER(4, 4)
#define PUSH_OPTIMIZATION_SIZE _Pragma("GCC push_options") SOURCE_OPTIMIZATION_SIZE
#define POP_OPTIMIZATION_SIZE _Pragma("GCC pop_options")
#else
#define PUSH_OPTIMIZATION_SIZE
#define POP_OPTIMIZATION_SIZE
#endif

/*
 * Instrumentation helpers, compiler promises
 */

// Assume the pointer is aligned to specific constant pow2 size
#if GNU_BUILTIN(__builtin_assume_aligned)
#define assume_aligned_ptr(ptr, size) __builtin_assume_aligned((ptr), (size))
#else
#define assume_aligned_ptr(ptr, size) (ptr)
#endif

// Allow aliasing for type with this attribute (Same as char type)
#if GNU_ATTRIBUTE(__may_alias__)
#define safe_aliasing __attribute__((__may_alias__))
#else
#define safe_aliasing
#endif

// Warn if return value is unused
#if GNU_ATTRIBUTE(__warn_unused_result__)
#define warn_unused_ret __attribute__((__warn_unused_result__))
#else
#define warn_unused_ret
#endif

// Explicitly mark deallocator for an allocator function
#if GNU_ATTRIBUTE(__malloc__)
#define deallocate_with(deallocator) warn_unused_ret __attribute__((__malloc__,__malloc__(deallocator, 1)))
#else
#define deallocate_with(deallocator) warn_unused_ret
#endif

// Suppress ThreadSanitizer in places with false positives (Emulated load/stores or RCU)
#if defined(__SANITIZE_THREAD__) && !defined(USE_SANITIZE_FULL) && GNU_ATTRIBUTE(__no_sanitize__)
#define TSAN_SUPPRESS __attribute__((__no_sanitize__("thread")))
#else
#define TSAN_SUPPRESS
#endif

// Suppress MemorySanitizer in places with false positives (Non-instrumented syscalls, external libs, etc)
#if defined(__SANITIZE_MEMORY__) && !defined(USE_SANITIZE_FULL) && GNU_ATTRIBUTE(__no_sanitize__)
#define MSAN_SUPPRESS __attribute__((__no_sanitize__("memory")))
#else
#define MSAN_SUPPRESS
#endif

// Call this function upon exit / library unload (GNU compilers only)
#if GNU_ATTRIBUTE(__destructor__)
#define GNU_DESTRUCTOR __attribute__((__destructor__))
#else
#define GNU_DESTRUCTOR
#endif

// Call this function upon startup / library load (GNU compilers only)
#if GNU_ATTRIBUTE(__constructor__)
#define GNU_CONSTRUCTOR __attribute__((__constructor__))
#else
#define GNU_CONSTRUCTOR
#endif

/*
 * Host platform feature detection
 */

// Detect endianness based on __BYTE_ORDER__, and arch ifdefs for older compilers
#if (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || \
    defined(__MIPSEB__) || defined(__ARMEB__)
#define HOST_BIG_ENDIAN 1
#elif defined(_MSC_VER) || !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || \
    defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__)
#define HOST_LITTLE_ENDIAN 1
#endif

// Determine whether host has fast misaligned access (Hint)
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_AMD64) || defined(__aarch64__)
#define HOST_FAST_MISALIGN 1
#else
// Not sure about other arches, misaligns may be very slow or crash
#define HOST_NO_MISALIGN 1
#endif

#define UNUSED(x) (void)x

// Determine host bitness (Hint)
#if UINTPTR_MAX == UINT64_MAX
#define HOST_64BIT 1
#elif UINTPTR_MAX == UINT32_MAX
#define HOST_32BIT 1
#endif

/*
 * Macro helpers
 */

// Unwrap a token or a token value into a string literal
#define MACRO_MKSTRING(x) #x
#define MACRO_TOSTRING(x) MACRO_MKSTRING(x)

// GNU extension that omits file path, use if available
#ifndef __FILE_NAME__
#define __FILE_NAME__ __FILE__
#endif

// Unwraps to example.c@128
#define SOURCE_LINE __FILE_NAME__ "@" MACRO_TOSTRING(__LINE__)

#define MACRO_ASSERT_NAMED(cond, name) typedef char static_assert_at_line_##name[(cond) ? 1 : -1]
#define MACRO_ASSERT_UNWRAP(cond, tok) MACRO_ASSERT_NAMED(cond, tok)

// Static build-time assertions
#ifdef IGNORE_BUILD_ASSERTS
#define BUILD_ASSERT(cond)
#elif __STDC_VERSION__ >= 201112LL && !defined(__chibicc__)
#define BUILD_ASSERT(cond) _Static_assert(cond, MACRO_TOSTRING(cond))
#else
#define BUILD_ASSERT(cond) MACRO_ASSERT_UNWRAP(cond, __LINE__)
#endif

// Same as BUILD_ASSERT, but produces an expression with value 0
#ifdef IGNORE_BUILD_ASSERTS
#define BUILD_ASSERT_EXPR(cond) 0
#else
#define BUILD_ASSERT_EXPR(cond) (sizeof(char[(cond) ? 1 : -1]) - 1)
#endif

#endif
