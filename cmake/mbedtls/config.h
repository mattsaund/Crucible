// SPDX-License-Identifier: MIT
//
// Mbed TLS as Crucible builds it: its own defaults, and then this, which
// Mbed TLS reads at the end of them (MBEDTLS_USER_CONFIG_FILE, set in
// cmake/CrucibleDependencies.cmake). Every file that includes an Mbed TLS
// header reads it too, Crucible's own and cpp-httplib's among them, since
// some of what it turns on changes the size of Mbed TLS's structures.
//
// C, because Mbed TLS is: no C++ in here.
#ifndef CRUCIBLE_MBEDTLS_CONFIG_H
#define CRUCIBLE_MBEDTLS_CONFIG_H

// Locks. The API server answers each connection on a thread of its own, and
// every connection draws on what its listener's connections share: the random
// generator, and PSA's state, which TLS 1.3 keeps for the whole process.
// Without locks that is a race, and Mbed TLS says as much: a program in which
// more than one thread does TLS has to turn them on. pthreads where there are
// pthreads, and on Windows its own slim locks, which src/serve/tls.cpp hands
// to Mbed TLS before anything else is asked of it (see threading_alt.h).
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif

#endif  // CRUCIBLE_MBEDTLS_CONFIG_H
