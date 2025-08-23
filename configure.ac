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
AM_PROG_AR
LT_INIT
AM_PROG_CC_C_O
# We require C++11
AX_CXX_COMPILE_STDCXX_11(,[mandatory])

# Check for an aggressive optimization flag
AC_ARG_ENABLE([aggressive-opts],
    [AS_HELP_STRING([--enable-aggressive-opts], [Enable aggressive, performance-focused compiler optimizations (-O3, -march=native, etc.)])],
    [
        if test "x$enableval" = "xyes"; then
            # These are the flags for maximum runtime performance, potentially at the cost of binary size and compile time.
            # -O3: The highest level of standard optimization. Enables auto-vectorization.
            # -march=native: Compile for the specific CPU architecture of the build machine, enabling all its instruction sets (e.g., AVX2, FMA).
            # -flto=auto: Enable Link-Time Optimization and parallelize it across all available cores.
            # -falign-functions=32: Align functions to a 32-byte boundary, which can improve instruction cache performance.
            # -funroll-loops: Perform loop unrolling.
            # -fomit-frame-pointer: Frees up a register for general purpose use.
            # -ffast-math: WARNING: Breaks strict IEEE 754 compliance for floating point math, but is much faster. Use with caution.
            # -Wno-write-strings: Suppress warnings about converting const string literals to char*, which this legacy codebase does extensively.
            # -fpermissive: Downgrade some strict conformance errors to warnings. Needed for legacy code that compares pointers to integers.
            # -Wno-unused-result: Suppress warnings about not checking the return value of functions like fread().
            # -Wno-register: Suppress warnings about the deprecated 'register' keyword.
            aggressive_flags="-Ofast -march=native -flto=auto -falign-functions=32 -fomit-frame-pointer -Wno-write-strings -fpermissive -Wno-unused-result -Wno-register"
            CXXFLAGS="$CXXFLAGS $aggressive_flags"
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
