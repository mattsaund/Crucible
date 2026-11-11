// SPDX-License-Identifier: MIT
//
// Mbed TLS's lock on Windows, where it brings none of its own (see config.h).
//
// An SRWLOCK, which is one pointer and needs nothing freed. Kept here as the
// pointer it is rather than by its own name, so that every file that includes
// an Mbed TLS header does not have to include windows.h first: httplib.h,
// for one, has its own order for the Windows headers. The functions that use
// it are in src/serve/tls.cpp, which checks that the two are the same size.
//
// C, because Mbed TLS is.
#ifndef CRUCIBLE_MBEDTLS_THREADING_ALT_H
#define CRUCIBLE_MBEDTLS_THREADING_ALT_H

typedef struct mbedtls_threading_mutex_t {
    void* lock;
} mbedtls_threading_mutex_t;

#endif  // CRUCIBLE_MBEDTLS_THREADING_ALT_H
