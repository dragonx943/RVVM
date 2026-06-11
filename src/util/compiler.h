/*
compiler.h - Compilers tricks and features
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef LEKKIT_COMPILER_TRICKS_H
#define LEKKIT_COMPILER_TRICKS_H

#include <stdint.h>

/*
 * Compiler feature detection
 */

// Detect Clang compiler
#undef COMPILER_IS_CLANG
#if defined(__clang__) || defined(__llvm__)
#define COMPILER_IS_CLANG 1
#endif

// Detect GCC compiler
#undef COMPILER_IS_GCC
#if defined(__GNUC__) && !defined(COMPILER_IS_CLANG) && !defined(__INTEL_COMPILER)
#define COMPILER_IS_GCC 1
#endif

// Detect MSVC compiler
#undef COMPILER_IS_MSVC
#if defined(_MSC_VER)
#define COMPILER_IS_MSVC 1
#endif

// GCC version checking
#undef GCC_CHECK_VER
#if defined(COMPILER_IS_GCC)
#define GCC_CHECK_VER(major, minor) (__GNUC__ > major || (__GNUC__ == major && __GNUC_MINOR__ >= minor))
#else
#define GCC_CHECK_VER(major, minor) 0
#endif

// Clang version checking
#undef CLANG_CHECK_VER
#if defined(COMPILER_IS_CLANG)
#define CLANG_CHECK_VER(major, minor)                                                                                  \
    (__clang_major__ > major || (__clang_major__ == major && __clang_minor__ >= minor))
#else
#define CLANG_CHECK_VER(major, minor) 0
#endif

// Detect GNU extensions support (Inline asm, etc)
#undef GNU_EXTS
#if defined(COMPILER_IS_GCC) || defined(COMPILER_IS_CLANG) /**/                                                        \
    || defined(__INTEL_COMPILER) || defined(__tcc__) || defined(__slimcc__)
#define GNU_EXTS 1
#endif

// Check GNU attribute presence (GCC 5.1+, Clang 3.1+)
#undef GNU_ATTRIBUTE
#undef GNU_DUMMY_ATTRIBUTE
#if defined(GNU_EXTS) && defined(__has_attribute)
#define GNU_ATTRIBUTE(attr) __has_attribute(attr)
#define GNU_DUMMY_ATTRIBUTE __attribute__(())
#else
#define GNU_ATTRIBUTE(attr) 0
#define GNU_DUMMY_ATTRIBUTE
#endif

// Check GNU builtin presence (GCC 10.0+, Clang 3.0+)
#undef GNU_BUILTIN
#if defined(GNU_EXTS) && defined(__has_builtin)
#define GNU_BUILTIN(builtin) __has_builtin(builtin)
#else
#define GNU_BUILTIN(builtin) 0
#endif

// Check GNU feature presence (GCC 14.0+, Clang 3.0+)
#undef GNU_FEATURE
#if defined(GNU_EXTS) && defined(__has_feature)
#define GNU_FEATURE(feature) __has_feature(feature)
#else
#define GNU_FEATURE(feature) 0
#endif

// Check GNU extension presence (GCC 14.0+, Clang 3.0+)
#undef GNU_EXTENSION
#if defined(GNU_EXTS) && defined(__has_extension)
#define GNU_EXTENSION(extension) __has_extension(extension)
#else
#define GNU_EXTENSION(extension) GNU_FEATURE(extension)
#endif

// Detect __SANITIZE_UNDEFINED__, __SANITIZE_THREAD__, __SANITIZE_ADDRESS__, __SANITIZE_MEMORY__
#if defined(USE_UBSAN) && !defined(__SANITIZE_UNDEFINED__)
#define __SANITIZE_UNDEFINED__ 1
#endif
#if (defined(USE_ASAN) || GNU_FEATURE(address_sanitizer)) && !defined(__SANITIZE_ADDRESS__)
#define __SANITIZE_ADDRESS__ 1
#endif
#if (defined(USE_TSAN) || GNU_FEATURE(thread_sanitizer)) && !defined(__SANITIZE_THREAD__)
#define __SANITIZE_THREAD__ 1
#endif
#if (defined(USE_MSAN) || GNU_FEATURE(memory_sanitizer)) && !defined(__SANITIZE_MEMORY__)
#define __SANITIZE_MEMORY__ 1
#endif

// Detect sanitizers to disable isolation/signal handlers
#undef SANITIZERS_ENABLED
#if defined(__SANITIZE_UNDEFINED__) || defined(__SANITIZE_ADDRESS__) /**/                                              \
    || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_MEMORY__)
#define SANITIZERS_ENABLED 1
#endif

// Check header presence (GCC 4.9.2+, Clang 3.0+)
#undef CHECK_INCLUDE
#if defined(GNU_EXTS) && defined(__has_include)
#define CHECK_INCLUDE(include, urgent) __has_include(#include)
#elif defined(GNU_EXTS)
#define CHECK_INCLUDE(include, urgent) (urgent)
#else
#define CHECK_INCLUDE(include, urgent) 1
#endif

/*
 * Host platform feature detection
 */

// Determine host bitness (Possibly neither 32/64 bit)
#undef HOST_64BIT
#undef HOST_32BIT
#if defined(__LP64__) || defined(_M_AMD64) || defined(_M_ARM64)                                                        \
    || (defined(UINTPTR_MAX) && (UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFULL))                                                \
    || (defined(SIZE_MAX) && (SIZE_MAX == 0xFFFFFFFFFFFFFFFFULL))
#define HOST_64BIT 1
#elif !defined(UINTPTR_MAX) || !defined(SIZE_MAX) || (UINTPTR_MAX == 0xFFFFFFFFU) || (SIZE_MAX == 0xFFFFFFFFU)
#define HOST_32BIT 1
#elif !defined(UINTPTR_MAX) || !defined(SIZE_MAX) || (UINTPTR_MAX == 0xFFFFU) || (SIZE_MAX == 0xFFFFU)
#define HOST_16BIT 1
#endif

// Determine integer endianness (Possibly neither big/little endian)
// If neither __BYTE_ORDER__, __BIG_ENDIAN__, __LITTLE_ENDIAN__ are supported by the toolchain,
// an extensive list of big-endian platforms is checked, and little-endian is assumed otherwise
#undef HOST_BIG_ENDIAN
#undef HOST_LITTLE_ENDIAN
#if (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__))             \
    || defined(__BIG_ENDIAN__)                                                                                         \
    || (!defined(__BYTE_ORDER__) && !defined(__LITTLE_ENDIAN__)                                                        \
        && (defined(__MIPSEB__) || defined(__ARMEB__) || defined(__powerpc__) || defined(__sparc__)                    \
            || defined(__m68k__) || defined(__hppa__) || defined(__s390__) || defined(_M_PPC)))
#define HOST_BIG_ENDIAN 1
#elif (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))     \
    || !defined(__BYTE_ORDER__) || defined(__LITTLE_ENDIAN__)
#define HOST_LITTLE_ENDIAN 1
#endif

// Determine FPU endianness (Possibly differs from integer, or neither big/little endian)
#undef HOST_FPU_BIG_ENDIAN
#undef HOST_FPU_LITTLE_ENDIAN
#if (defined(__FLOAT_WORD_ORDER__) && (__FLOAT_WORD_ORDER__ == __ORDER_BIG_ENDIAN__))                                  \
    || (!defined(__FLOAT_WORD_ORDER__) && defined(HOST_BIG_ENDIAN))
#define HOST_FPU_BIG_ENDIAN 1
#elif (defined(__FLOAT_WORD_ORDER__) && (__FLOAT_WORD_ORDER__ == __ORDER_LITTLE_ENDIAN__))                             \
    || (!defined(__FLOAT_WORD_ORDER__) && defined(HOST_LITTLE_ENDIAN))
#define HOST_FPU_LITTLE_ENDIAN 1
#endif

// Detect whether host may perform fast misaligned access (Hint)
#undef HOST_FAST_MISALIGN
#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv_misaligned_fast) || defined(_M_AMD64)               \
    || defined(_M_ARM64)
#define HOST_FAST_MISALIGN 1
#endif

// Disable per-function optimizations on SuperH due to compiler bugs
#if defined(__sh__)
#undef USE_NO_FUNC_OPT
#define USE_NO_FUNC_OPT 1
#endif

/*
 * Macro helpers
 */

// Unwrap a token or a token value into a literal
#undef MACRO_TOSTRING_INTERNAL
#undef MACRO_TOSTRING
#define MACRO_TOSTRING_INTERNAL(x) #x
#define MACRO_TOSTRING(x)          MACRO_TOSTRING_INTERNAL(x)

// Concatenate tokens or token values into a literal
#undef MACRO_CONCAT_INTERNAL
#undef MACRO_CONCAT
#define MACRO_CONCAT_INTERNAL(a, b) a##b
#define MACRO_CONCAT(a, b)          MACRO_CONCAT_INTERNAL(a, b)

// Give a unique variable identifier for each macro instantiation
#undef MACRO_IDENT
#undef MACRO_IDENT_NAME
#define MACRO_IDENT(identifier)            MACRO_CONCAT(identifier, __LINE__)
#define MACRO_IDENT_NAME(identifier, name) MACRO_CONCAT(name, MACRO_CONCAT(identifier, __LINE__))

// GNU extension that omits file path, use if available
#if !defined(__FILE_NAME__)
#define __FILE_NAME__ __FILE__
#endif

// Unwraps to example.c@128
#undef SOURCE_LINE
#define SOURCE_LINE __FILE_NAME__ "@" MACRO_TOSTRING(__LINE__)

// Static build-time assertions
#undef BUILD_ASSERT
#if defined(USE_NO_BUILD_ASSERT)
#define BUILD_ASSERT(cond)
#elif __STDC_VERSION__ >= 202000LL
// Use C23 static_assert() (GCC 14 reports 202000LL for C23 as of now)
#define BUILD_ASSERT(cond) static_assert(cond, MACRO_TOSTRING(cond))
// Use C11 _Static_assert() (chibicc doesn't implement it despite advertising C11 support)
#elif __STDC_VERSION__ >= 201112LL && !defined(__chibicc__)
#define BUILD_ASSERT(cond) _Static_assert(cond, MACRO_TOSTRING(cond))
#else
#define BUILD_ASSERT(cond) typedef char MACRO_CONCAT(static_assert_at_line_, __LINE__)[(cond) ? 1 : -1]
#endif

// Same as BUILD_ASSERT, but produces an expression with value 0
#undef BUILD_ASSERT_EXPR
#if defined(USE_NO_BUILD_ASSERT)
#define BUILD_ASSERT_EXPR(cond) 0
#else
#define BUILD_ASSERT_EXPR(cond) (sizeof(char[(cond) ? 1 : -1]) - 1)
#endif

/*
 * Optimization hints
 */

// Branch optimization hints
#undef likely
#undef unlikely
#if !defined(USE_NO_LIKELY) && (GCC_CHECK_VER(3, 0) || GNU_BUILTIN(__builtin_expect))
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif

// Memory prefetch hints
#undef mem_prefetch
#if !defined(USE_NO_PREFETCH) && (GCC_CHECK_VER(3, 2) || GNU_BUILTIN(__builtin_prefetch))
#define mem_prefetch(addr, rw, locality) __builtin_prefetch((addr), !!(rw), (locality) & 3)
#else
#define mem_prefetch(addr, rw, locality) (((void)(addr)), ((void)(rw)), ((void)(locality)))
#endif

// Force-inline this function
#undef forceinline
#if !defined(USE_NO_FORCEINLINE) && (GCC_CHECK_VER(3, 3) || GNU_ATTRIBUTE(__always_inline__)) /**/                     \
    && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
// Sanitizers don't play well with __always_inline__
#define forceinline inline __attribute__((__always_inline__))
#elif !defined(USE_NO_FORCEINLINE) && defined(COMPILER_IS_MSVC)
#define forceinline __forceinline
#else
#define forceinline inline
#endif

// Never inline this function
#undef no_inline
#if !defined(USE_NO_NOINLINE) && GCC_CHECK_VER(4, 5)
#define no_inline __attribute__((__noinline__, __noclone__))
#elif !defined(USE_NO_NOINLINE) && (GCC_CHECK_VER(3, 2) || GNU_ATTRIBUTE(__noinline__))
#define no_inline __attribute__((__noinline__))
#elif !defined(USE_NO_NOINLINE) && defined(COMPILER_IS_MSVC)
#define no_inline __declspec(noinline)
#else
#define no_inline GNU_DUMMY_ATTRIBUTE
#endif

// Inline all callees into this function. Use with care!
#undef flatten_calls
#if !defined(USE_NO_FLATTEN) && (GCC_CHECK_VER(4, 1) || GNU_ATTRIBUTE(__flatten__))
#define flatten_calls __attribute__((__flatten__))
#else
#define flatten_calls GNU_DUMMY_ATTRIBUTE
#endif

// Optimize function with specific optimization argument (GCC only)
#undef func_gcc_optimize
#if !defined(USE_NO_FUNC_OPT) && GCC_CHECK_VER(4, 4)
#define func_gcc_optimize(arg) __attribute__((__optimize__(arg)))
#else
#define func_gcc_optimize(arg) GNU_DUMMY_ATTRIBUTE
#endif

// Optimize inline function for size (Clang only)
#undef func_clang_minsize
#if !defined(USE_NO_FUNC_OPT) && GNU_ATTRIBUTE(__minsize__)
#define func_clang_minsize __attribute__((__minsize__))
#else
#define func_clang_minsize GNU_DUMMY_ATTRIBUTE
#endif

// Optimize hot function for performance with -O3
#undef func_opt_hot
#if !defined(USE_NO_FUNC_OPT) && GCC_CHECK_VER(4, 4)
#define func_opt_hot __attribute__((__hot__, __optimize__("O3")))
#elif !defined(USE_NO_FUNC_OPT) && GNU_ATTRIBUTE(__hot__)
#define func_opt_hot __attribute__((__hot__))
#else
#define func_opt_hot GNU_DUMMY_ATTRIBUTE
#endif

// Optimize non-inline function for size (-Oz or -Os)
#undef func_opt_size
#if !defined(USE_NO_FUNC_OPT) && GCC_CHECK_VER(12, 1)
#define func_opt_size __attribute__((__optimize__("Oz")))
#elif !defined(USE_NO_FUNC_OPT) && GCC_CHECK_VER(4, 4)
#define func_opt_size __attribute__((__optimize__("Os")))
#else
#define func_opt_size func_clang_minsize
#endif

// Hint cold function, minimize call site intrusion if possible
#undef func_opt_cold
#if GCC_CHECK_VER(4, 5)
#define func_opt_cold __attribute__((__cold__, __noclone__))
#elif GNU_ATTRIBUTE(__cold__)
#define func_opt_cold __attribute__((__cold__))
#else
#define func_opt_cold GNU_DUMMY_ATTRIBUTE
#endif

/*
 * Reduce call site overhead in a less aggressive way
 *
 * NOTE: This affects the function ABI, do not expose such functions dynamically
 */
#undef cold_path
#if defined(__i386__) && GNU_ATTRIBUTE(__stdcall__)
#define cold_path no_inline func_opt_cold __attribute__((__stdcall__))
#else
#define cold_path no_inline func_opt_cold
#endif

/*
 * Minimize call site intrusion, omitting register spill around a slow path
 *
 * NOTE: This affects the function ABI, do not expose such functions dynamically
 * NOTE: __preserve_most__ is broken on Clang <17, and on Clang Windows ARM64
 *
 * Hopefully one day __preserve_most__ makes it's way into GCC.
 */
#undef slow_path
#if !defined(USE_NO_SLOW_PATH) && CLANG_CHECK_VER(17, 0)                                                               \
    && (defined(__x86_64__) || (defined(__aarch64__) && !defined(_WIN32)))
#define slow_path cold_path __attribute__((__preserve_most__))
#else
#define slow_path cold_path
#endif

// Per-source optimization level requests via definition (GCC only)
// Define SOURCE_OPTIMIZATION_SIZE or other desired optimization level at the top
#if !defined(USE_NO_SOURCE_OPT) && defined(SOURCE_OPTIMIZATION_SIZE) && GCC_CHECK_VER(12, 1)
#pragma GCC optimize("Oz")
#elif !defined(USE_NO_SOURCE_OPT) && defined(SOURCE_OPTIMIZATION_SIZE) && GCC_CHECK_VER(4, 4)
#pragma GCC optimize("Os")
#elif !defined(USE_NO_SOURCE_OPT) && defined(SOURCE_OPTIMIZATION_O2) && GCC_CHECK_VER(4, 4)
#pragma GCC optimize("O2")
#elif !defined(USE_NO_SOURCE_OPT) && defined(SOURCE_OPTIMIZATION_O3) && GCC_CHECK_VER(4, 4)
#pragma GCC optimize("O3")
#elif !defined(USE_NO_SOURCE_OPT) && defined(SOURCE_OPTIMIZATION_NONE) && GCC_CHECK_VER(4, 4)
#pragma GCC optimize("O0")
#endif

// Whole-source optimization pragmas (GCC only, prefer to replace with defines)
#undef SOURCE_OPTIMIZATION_NONE
#undef SOURCE_OPTIMIZATION_SIZE
#undef SOURCE_OPTIMIZATION_O2
#undef SOURCE_OPTIMIZATION_O3
#if !defined(USE_NO_SOURCE_OPT) && GCC_CHECK_VER(4, 4)
#define SOURCE_OPTIMIZATION_NONE _Pragma("GCC optimize(\"O0\")")
#define SOURCE_OPTIMIZATION_O2   _Pragma("GCC optimize(\"O2\")")
#define SOURCE_OPTIMIZATION_O3   _Pragma("GCC optimize(\"O3\")")
#else
#define SOURCE_OPTIMIZATION_NONE
#define SOURCE_OPTIMIZATION_O2
#define SOURCE_OPTIMIZATION_O3
#endif
#if !defined(USE_NO_SOURCE_OPT) && GCC_CHECK_VER(12, 1)
#define SOURCE_OPTIMIZATION_SIZE _Pragma("GCC optimize(\"Oz\")")
#elif GCC_CHECK_VER(4, 4)
#define SOURCE_OPTIMIZATION_SIZE _Pragma("GCC optimize(\"Os\")")
#else
#define SOURCE_OPTIMIZATION_SIZE
#endif

// Pushable size optimization attribute (GCC and Clang)
#undef PUSH_OPTIMIZATION_SIZE
#undef POP_OPTIMIZATION_SIZE
#if !defined(USE_NO_SOURCE_OPT) && CLANG_CHECK_VER(12, 0)
#define PUSH_OPTIMIZATION_SIZE                                                                                         \
    _Pragma("clang attribute push (__attribute__((__cold__,__minsize__)), apply_to=function)")
#define POP_OPTIMIZATION_SIZE _Pragma("clang attribute pop")
#elif !defined(USE_NO_SOURCE_OPT) && GCC_CHECK_VER(4, 4)
#define PUSH_OPTIMIZATION_SIZE _Pragma("GCC push_options") SOURCE_OPTIMIZATION_SIZE
#define POP_OPTIMIZATION_SIZE  _Pragma("GCC pop_options")
#else
#define PUSH_OPTIMIZATION_SIZE
#define POP_OPTIMIZATION_SIZE
#endif

/*
 * Optimization promises
 */

// Assert that the pointer is aligned to specific power of two
#undef assume_aligned_ptr
#if !defined(USE_NO_ASSUME) && (GCC_CHECK_VER(4, 7) || GNU_BUILTIN(__builtin_assume_aligned))
#define assume_aligned_ptr(ptr, size) __builtin_assume_aligned((ptr), (size))
#else
#define assume_aligned_ptr(ptr, size) (ptr)
#endif

// Assert that this location is never reached
#undef must_never_reach
#if !defined(USE_NO_ASSUME) && (GCC_CHECK_VER(4, 5) || GNU_BUILTIN(__builtin_unreachable))
#define must_never_reach() __builtin_unreachable()
#else
#define must_never_reach()                                                                                             \
    do {                                                                                                               \
    } while (1)
#endif

// Assert that the condition is always true
#undef must_evaluate_true
#if !defined(USE_NO_ASSUME) && (GCC_CHECK_VER(4, 5) || GNU_BUILTIN(__builtin_unreachable))
#define must_evaluate_true(cond)                                                                                       \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            must_never_reach();                                                                                        \
        }                                                                                                              \
    } while (0)
#else
#define must_evaluate_true(cond)                                                                                       \
    do {                                                                                                               \
    } while (0)
#endif

/*
 * Type attributes & helpers
 */

// Portable zero-initializer that doesn't violate __designated_init__
#undef ZERO_INIT
#if defined(GNU_EXTS)
#undef ZERO_INIT_EMPTY_TOKEN
#define ZERO_INIT_EMPTY_TOKEN
#define ZERO_INIT {ZERO_INIT_EMPTY_TOKEN}
#else
#define ZERO_INIT {0}
#endif

// Allow aliasing for type with this attribute (Same as char type)
#undef safe_aliasing
#if GCC_CHECK_VER(3, 3) || GNU_ATTRIBUTE(__may_alias__)
#define safe_aliasing __attribute__((__may_alias__))
#else
#define safe_aliasing GNU_DUMMY_ATTRIBUTE
#endif

// Align type to specific offset & size divisible by power of two
// NOTE: Avoid using this on static data, fails to build on some platforms
#undef align_type
#if !defined(USE_NO_ALIGN_TYPE) && (GCC_CHECK_VER(3, 0) || GNU_ATTRIBUTE(__aligned__))
#define align_type(alignment) __attribute__((__aligned__(alignment)))
#elif !defined(USE_NO_ALIGN_TYPE) && defined(COMPILER_IS_MSVC)
#define align_type(alignment) __declspec(align(alignment))
#else
#define align_type(alignment) GNU_DUMMY_ATTRIBUTE
#endif

// Align type to cacheline to prevent false sharing
// NOTE: Avoid using this on static data, fails to build on some platforms
#undef align_cacheline
#define align_cacheline align_type(64)

// Randomize the structure layout (Requires Clang 15+ or GCC randstruct plugin)
#undef randomize_layout
#if !defined(USE_NO_RANDSTRUCT) && GNU_ATTRIBUTE(__randomize_layout__)
#define randomize_layout __attribute__((__randomize_layout__))
#else
#define randomize_layout GNU_DUMMY_ATTRIBUTE
#endif

// Structure randomization helpers: randomized_struct keyword, randomized fields start/end
#undef randomized_struct
#undef randomized_fields_start
#undef randomized_fields_end
#define randomized_struct struct randomize_layout
#if __STDC_VERSION__ >= 201112LL || (defined(GNU_EXTS) && !defined(__STRICT_ANSI__))
// clang-format off
#define randomized_fields_start randomized_struct {
#define randomized_fields_end   };
// clang-format on
#else
#define randomized_fields_start
#define randomized_fields_end
#endif

// Thread-local storage specifier (Optional, check presence via ifdef THREAD_LOCAL)
#undef THREAD_LOCAL
#if !defined(USE_NO_THREAD_LOCAL) /**/                                                                                 \
    && (GCC_CHECK_VER(3, 3) || CLANG_CHECK_VER(3, 0) || defined(__slimcc__) || defined(__cproc__))
// Use GNU __thread attribute on GCC 3.3+, Clang 3.0+, SlimCC, Cproc
#define THREAD_LOCAL __thread
#elif !defined(USE_NO_THREAD_LOCAL) && defined(COMPILER_IS_MSVC)
// Use MSVC __declspec(thread)
#define THREAD_LOCAL __declspec(thread)
#elif !defined(USE_NO_THREAD_LOCAL) && __STDC_VERSION__ >= 202000LL && !defined(__STDC_NO_THREADS__)
// Use C23 thread_local (GCC 14 reports 202000LL for C23 as of now)
#define THREAD_LOCAL thread_local
#elif !defined(USE_NO_THREAD_LOCAL) && __STDC_VERSION__ >= 201112LL && !defined(__STDC_NO_THREADS__)
// Use C11 _Thread_local (Deprecated since C23, but we don't need to include <threads.h> that way)
#define THREAD_LOCAL _Thread_local
#endif

/*
 * Warning attributes / suppressions
 */

// Mark unused arguments or variables to suppress warnings
#undef UNUSED
#define UNUSED(x) ((void)(x))

// Portably cast to a non-constant pointer without triggering -Wconst-qual. Use with care!
#undef NONCONST_CAST
#if defined(GNU_EXTS)
// clang-format off
#define NONCONST_CAST(type, x) ((type)((union { const void* _constptr; void* _ptr; }){ ._constptr = (const void*)(x), })._ptr)
// clang-format on
#else
#define NONCONST_CAST(type, x) ((type)(void*)(x))
#endif

// Warn if function return value is unused
#undef warn_unused_ret
#if GNU_ATTRIBUTE(__warn_unused_result__)
#define warn_unused_ret __attribute__((__warn_unused_result__))
#else
#define warn_unused_ret GNU_DUMMY_ATTRIBUTE
#endif

// Explicitly mark deallocator for an allocator function
#undef deallocate_with
#if GNU_ATTRIBUTE(__malloc__)
#define deallocate_with(deallocator) warn_unused_ret __attribute__((__malloc__, __malloc__(deallocator, 1)))
#else
#define deallocate_with(deallocator) warn_unused_ret
#endif

// Suppress AddressSanitizer for a function with false positives
#undef ASAN_SUPPRESS
#if defined(__SANITIZE_ADDRESS__) && GNU_ATTRIBUTE(__no_sanitize__)
#define ASAN_SUPPRESS __attribute__((__no_sanitize__("address")))
#else
#define ASAN_SUPPRESS GNU_DUMMY_ATTRIBUTE
#endif

// Suppress ThreadSanitizer for a function with false positives
#undef TSAN_SUPPRESS
#if defined(__SANITIZE_THREAD__) && GNU_ATTRIBUTE(__no_sanitize__)
#define TSAN_SUPPRESS __attribute__((__no_sanitize__("thread")))
#else
#define TSAN_SUPPRESS GNU_DUMMY_ATTRIBUTE
#endif

// Suppress MemorySanitizer for a function with false positives
#undef MSAN_SUPPRESS
#if defined(__SANITIZE_MEMORY__) && GNU_ATTRIBUTE(__no_sanitize__)
#define MSAN_SUPPRESS __attribute__((__no_sanitize__("memory")))
#else
#define MSAN_SUPPRESS GNU_DUMMY_ATTRIBUTE
#endif

/*
 * Global constructors / destructors
 */

// Call this function upon exit / library unload (GNU compilers only)
#undef GNU_DESTRUCTOR
#if GCC_CHECK_VER(3, 3) || GNU_ATTRIBUTE(__destructor__)
#define GNU_DESTRUCTOR __attribute__((__destructor__))
#else
#define GNU_DESTRUCTOR GNU_DUMMY_ATTRIBUTE
#endif

// Call this function upon startup / library load (GNU compilers only)
#undef GNU_CONSTRUCTOR
#if GCC_CHECK_VER(3, 3) || GNU_ATTRIBUTE(__constructor__)
#define GNU_CONSTRUCTOR __attribute__((__constructor__))
#else
#define GNU_CONSTRUCTOR GNU_DUMMY_ATTRIBUTE
#endif

/*
 * Conditional scoped statement helpers
 *
 * When 'cond' is true:
 * - Execute 'expr_pre' at entry
 * - Execute the statement body
 * - Execute 'expr_post' on scope exit, break or continue
 *
 * When using *_COND family of helpers,
 * an else-clause may be used for when 'cond' is false
 *
 * Named variants exist for instatiating multiple scopes from a single macro
 *
 * SCOPED_HELPER(spin_lock(&lock), spin_unlock(&lock)) {
 *     do_something_under_lock();
 *
 *     if (need_exit_under_lock()) {
 *         break;
 *     }
 *
 *     do_something_else_under_lock();
 * }
 *
 * POST_COND(spin_try_lock(&lock), spin_unlock(&lock)) {
 *     do_something_under_lock();
 * } else {
 *     do_something_when_locking_failed();
 * }
 */

#undef BREAKABLE_SCOPE
#define BREAKABLE_SCOPE(name)                                                                                          \
    for (int MACRO_IDENT_NAME(breakable_scope_iter, name) = 1; /**/                                                    \
         MACRO_IDENT_NAME(breakable_scope_iter, name);         /**/                                                    \
         MACRO_IDENT_NAME(breakable_scope_iter, name) = 0)     /**/

#undef SCOPED_COND_NAMED
#define SCOPED_COND_NAMED(cond, expr_pre, expr_post, name)                                                             \
    for (int MACRO_IDENT_NAME(scoped_cond_iter, name) = 1,                                                 /**/        \
         MACRO_IDENT_NAME(scoped_cond, name)          = !!(cond) && (expr_pre, 1);                         /**/        \
         MACRO_IDENT_NAME(scoped_cond_iter, name);                                                         /**/        \
         MACRO_IDENT_NAME(scoped_cond_iter, name) = MACRO_IDENT_NAME(scoped_cond, name) && (expr_post, 0)) /**/        \
        BREAKABLE_SCOPE (name)                                                                             /**/        \
            if (MACRO_IDENT_NAME(scoped_cond, name))

#undef POST_COND_NAMED
#define POST_COND_NAMED(cond, expr_post, name)                                                                         \
    for (int MACRO_IDENT_NAME(post_cond_iter, name) = 1, MACRO_IDENT_NAME(post_cond, name) = !!(cond); /**/            \
         MACRO_IDENT_NAME(post_cond_iter, name);                                                       /**/            \
         MACRO_IDENT_NAME(post_cond_iter, name) = MACRO_IDENT_NAME(post_cond, name) && (expr_post, 0)) /**/            \
        BREAKABLE_SCOPE (name)                                                                         /**/            \
            if (MACRO_IDENT_NAME(post_cond, name))

#undef SCOPED_STMT_NAMED
#define SCOPED_STMT_NAMED(cond, expr_pre, expr_post, name)                                                             \
    for (int MACRO_IDENT_NAME(scoped_stmt_iter, name) = !!(cond) && (expr_pre, 1); /**/                                \
         MACRO_IDENT_NAME(scoped_stmt_iter, name);                                 /**/                                \
         MACRO_IDENT_NAME(scoped_stmt_iter, name) = (expr_post, 0))                /**/                                \
        BREAKABLE_SCOPE (name)

#undef POST_STMT_NAMED
#define POST_STMT_NAMED(cond, expr_post, name)                                                                         \
    for (int MACRO_IDENT_NAME(post_stmt_iter, name) = !!(cond);   /**/                                                 \
         MACRO_IDENT_NAME(post_stmt_iter, name);                  /**/                                                 \
         MACRO_IDENT_NAME(post_stmt_iter, name) = (expr_post, 0)) /**/                                                 \
        BREAKABLE_SCOPE (name)

#undef SCOPED_COND
#define SCOPED_COND(cond, expr_pre, expr_post) SCOPED_COND_NAMED (cond, expr_pre, expr_post, )

#undef POST_COND
#define POST_COND(cond, expr_post) POST_COND_NAMED (cond, expr_post, )

#undef SCOPED_STMT
#define SCOPED_STMT(cond, expr_pre, expr_post) SCOPED_STMT_NAMED (cond, expr_pre, expr_post, )

#undef POST_STMT
#define POST_STMT(cond, expr_post) POST_STMT_NAMED (cond, expr_post, )

#undef SCOPED_HELPER
#define SCOPED_HELPER(expr_pre, expr_post) SCOPED_STMT (1, expr_pre, expr_post)

#endif
