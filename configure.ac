AC_INIT([jio], [0.1], [vcaputo@pengaru.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_CONFIG_MACRO_DIRS([m4])
AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB
AM_SILENT_RULES([yes])

CFLAGS="$CFLAGS -Wall"

dnl pthreads for libiou's sake
AX_PTHREAD
LIBS="$PTHREAD_LIBS $LIBS"
CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
CC="$PTHREAD_CC"

dnl Check for liburing
PKG_CHECK_MODULES(URING, liburing)
CFLAGS="$CFLAGS $URING_CFLAGS"
LIBS="$LIBS $URING_LIBS"

dnl Check for libcrypto
PKG_CHECK_MODULES(CRYPTO, libcrypto)
CFLAGS="$CFLAGS $CRYPTO_CFLAGS"
LIBS="$LIBS $CRYPTO_LIBS"

dnl Check for libztsd
PKG_CHECK_MODULES(ZSTD, libzstd)
CFLAGS="$CFLAGS $ZSTD_CFLAGS"
LIBS="$LIBS $ZSTD_LIBS"

AC_CONFIG_SUBDIRS([
 libiou
])

AC_CONFIG_FILES([
 Makefile
 src/Makefile
 src/upstream/Makefile
])

AC_OUTPUT
