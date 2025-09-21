# Version starts with 0 to facilitate actual upstream releases, in the future
# followed by the latest upstream svn revision number, followed by this
# autotools repackaging version.
#
# This number is bumped before the version is released, so to find the actual
# revision that creates that relase just look for the v$(version) tag.
AC_INIT([playfun], [1.0], [https://github.com/OpenSourceAnarchist/learnfun-playfun])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])
AC_PROG_CXX
AC_PROG_CC
AM_PROG_AR
LT_INIT
AM_PROG_CC_C_O

# We require C++23
AX_CXX_COMPILE_STDCXX([23],, [mandatory])
# We require C23. Check for the flag manually for portability.
AC_LANG_PUSH([C])
AX_CHECK_COMPILE_FLAG([-std=c23],
    [CFLAGS="$CFLAGS -std=c23"],
    [AX_CHECK_COMPILE_FLAG([-std=gnu23],
        [CFLAGS="$CFLAGS -std=gnu23"],
        [AC_MSG_ERROR([A C23-compliant compiler is required.])])
    ])
AC_LANG_POP([C])

# Add warning flags for all C and C++ builds
CFLAGS="$CFLAGS -Wall -Wno-register -Wno-deprecated -Wno-unused-result -Wno-write-strings -fpermissive"
CXXFLAGS="$CXXFLAGS -Wall -Wno-register -Wno-deprecated -Wno-unused-result -Wno-write-strings"

# Check for an aggressive optimization flag
AC_ARG_ENABLE([aggressive-opts],
    [AS_HELP_STRING([--enable-aggressive-opts], [Enable aggressive, performance-focused compiler optimizations (-Ofast, -march=native, LTO, etc.)])],
    [
        if test "x$enableval" = "xyes"; then
            # These flags favor maximum runtime performance and may impact portability and floating-point semantics.
            # Notes:
            #  -Ofast implies -ffast-math and may change IEEE 754 behavior; use only if acceptable.
            #  -march=native generates code for the local CPU; resulting binaries may not run on older/different CPUs.
            #  -flto requires matching binutils; add to both compile and link flags.
            cxx_aggressive_flags="-Ofast -march=native -flto=auto -falign-functions=32 -fomit-frame-pointer -fno-strict-aliasing"
            c_aggressive_flags="-Ofast -march=native -flto=auto -falign-functions=32 -fomit-frame-pointer -fno-strict-aliasing"
            ld_aggressive_flags="-Ofast -march=native -flto=auto"

            CXXFLAGS="$CXXFLAGS $cxx_aggressive_flags"
            CFLAGS="$CFLAGS $c_aggressive_flags"
            LDFLAGS="$LDFLAGS $ld_aggressive_flags"
        fi
    ]
)

PKG_CHECK_MODULES([SDL], [sdl])
PKG_CHECK_MODULES([SDL_NET], [SDL_net])
PKG_CHECK_MODULES([ZLIB], [zlib])
PKG_CHECK_MODULES([LIBPNG], [libpng])
PKG_CHECK_MODULES([PROTOBUF], [protobuf])
AC_CHECK_PROG([PROTOC], [protoc], [protoc])
AS_IF([test "x${PROTOC}" == "x"],
    [AC_MSG_ERROR([ProtoBuf compiler "protoc" not found.])])

AC_CONFIG_FILES([Makefile cc-lib/Makefile tasbot/Makefile])
AC_OUTPUT
