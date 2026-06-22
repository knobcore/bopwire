#ifndef LIBWALLYCORE_CONFIG_H
#define LIBWALLYCORE_CONFIG_H

/* Define if building universal (internal helper macro) */
/* #undef AC_APPLE_UNIVERSAL_BUILD */

/* Define to 1 if you have the <asm/page.h> header file. */
/* #undef HAVE_ASM_PAGE_H */

/* Define to 1 if you have the <byteswap.h> header file. */
#define HAVE_BYTESWAP_H 1

/* Define to 1 if you have the `explicit_bzero' function. */
#define HAVE_EXPLICIT_BZERO 1

/* Define to 1 if you have the `explicit_memset' function. */
/* #undef HAVE_EXPLICIT_MEMSET */

/* inline asm code can be used */
#define HAVE_INLINE_ASM 1

/* Define to 1 if you have the <mbedtls/sha256.h,> header file. */
/* #undef HAVE_MBEDTLS_SHA256_H_ */

/* Define to 1 if you have the <mbedtls/sha512.h> header file. */
/* #undef HAVE_MBEDTLS_SHA512_H */

/* Define to 1 if you have the `memset_s' function. */
/* #undef HAVE_MEMSET_S */

/* Define if we have mmap */
#define HAVE_MMAP 1

/* Define if we have posix_memalign */
#define HAVE_POSIX_MEMALIGN 1

/* Define to 1 if you have the <sys/mman.h> header file. */
#define HAVE_SYS_MMAN_H 1

/* Define if we have unaligned access */
/* #undef HAVE_UNALIGNED_ACCESS */

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Name of package */
#define PACKAGE "musicchain_web"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT ""

/* Define to the full name of this package. */
#define PACKAGE_NAME "musicchain_web"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "musicchain_web 1.5.4"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "musicchain_web"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "1.5.4"

/* Version number of package */
#define VERSION "1.5.4"

/* #undef WORDS_BIGENDIAN */

#if defined (_WIN32) && !defined(_SSIZE_T_DECLARED) && !defined(_ssize_t) && !defined(ssize_t)
#if defined(_WIN64)
typedef __int64 ssize_t;
#else
typedef long ssize_t;
#endif
#endif

#include "ccan_config.h"
#endif /* LIBWALLYCORE_CONFIG_H */
