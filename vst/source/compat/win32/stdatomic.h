/*
 * stdatomic.h mirror (Windows only) — fixes C11 atomics in C++ TUs.
 *
 * The shared C headers declare struct fields as `_Atomic int`, `atomic_int`,
 * `atomic_uint_fast64_t` and include <stdatomic.h> unconditionally. In C TUs
 * that is correct and clang's real header must be used (include_next). In C++
 * TUs on Windows, clang's C11 <stdatomic.h> defines function-like macros
 * (atomic_load, atomic_exchange, …) that poison the MS STL (<atomic>,
 * ppltasks.h → "no matching function for call to '__c11_atomic_exchange'").
 * clang-cl accepts `_Atomic T` in C++ as an extension with the same layout,
 * so the typedefs below are all the C++ side needs.
 */
#pragma once

#if defined(__cplusplus)

#include <stdint.h>

typedef _Atomic bool               atomic_bool; /* C++: _Bool n'existe pas */
typedef _Atomic char               atomic_char;
typedef _Atomic int                atomic_int;
typedef _Atomic unsigned int       atomic_uint;
typedef _Atomic long               atomic_long;
typedef _Atomic unsigned long      atomic_ulong;
typedef _Atomic long long          atomic_llong;
typedef _Atomic unsigned long long atomic_ullong;
typedef _Atomic size_t             atomic_size_t;
typedef _Atomic int_fast32_t       atomic_int_fast32_t;
typedef _Atomic uint_fast32_t      atomic_uint_fast32_t;
typedef _Atomic int_fast64_t       atomic_int_fast64_t;
typedef _Atomic uint_fast64_t      atomic_uint_fast64_t;

#else /* C: defer to clang's real C11 header */

#include_next <stdatomic.h>

#endif
