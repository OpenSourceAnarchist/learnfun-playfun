AC_PREREQ([2.69])
AC_INIT([playfun], [1.0], [https://github.com/OpenSourceAnarchist/learnfun-playfun])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([-Wall foreign subdir-objects])

# (Alias normalization for -all-opts / --all-opts handled by wrapper script generated in autogen.sh)

AC_PROG_CC
AC_PROG_CXX
AM_PROG_AR
LT_INIT
AM_PROG_CC_C_O

# Require modern language levels (C23/C++23) to match project sources.
AX_CXX_COMPILE_STDCXX([23],[noext],[mandatory])
AC_LANG_PUSH([C])
AX_CHECK_COMPILE_FLAG([-std=c23],
    [CFLAGS="$CFLAGS -std=c23"],
    [AX_CHECK_COMPILE_FLAG([-std=gnu23],
         [CFLAGS="$CFLAGS -std=gnu23"],
         [AC_MSG_ERROR([A C23-compliant compiler is required.])])])
AC_LANG_POP([C])

# Base warnings
BASE_WARN="-Wall -Wno-register -Wno-deprecated -Wno-unused-result -Wno-write-strings"
CFLAGS="$CFLAGS $BASE_WARN -fpermissive"
CXXFLAGS="$CXXFLAGS $BASE_WARN"

# Detect compiler family early for optimization tailoring.
compiler_family=unknown
case "$CXX" in
    *clang*) compiler_family=clang ;;
    *g++*|*gcc*) compiler_family=gcc ;;
esac
AC_MSG_NOTICE([Compiler family: $compiler_family])

# Single master enable flag (canonical autoconf form --enable-all-opts)
AC_ARG_ENABLE([all-opts],
    [AS_HELP_STRING([--enable-all-opts],[Enable high-performance build: -Ofast plus additive flags (-march=native -flto -funroll-loops -ffunction-sections -fdata-sections; Graphite/Polly if supported). Alias: -all-opts / --all-opts])],
    [enable_all_opts=$enableval],
    [enable_all_opts=no])

graphite_enabled=no
polly_enabled=no

# Default: keep optimization minimal for debuggability unless user explicitly
# passed custom CFLAGS/CXXFLAGS externally. If not enabling all-opts AND user
# did not predefine optimization level, force -O0 to avoid accidental -O2.
if test "x$enable_all_opts" != "xyes"; then
    case " $CFLAGS " in
        *-O[0-9s]*|*-Ofast*|-O) ;; # user supplied
        *) CFLAGS="-O0 $CFLAGS" ;;
    esac
    case " $CXXFLAGS " in
        *-O[0-9s]*|*-Ofast*|-O) ;; 
        *) CXXFLAGS="-O0 $CXXFLAGS" ;;
    esac
else
    # Enabled: add -Ofast plus additive flags only.
    case "$compiler_family" in
        gcc)
            CFLAGS="-Ofast -march=native -flto -funroll-loops -ffunction-sections -fdata-sections $CFLAGS"
            CXXFLAGS="-Ofast -march=native -flto -funroll-loops -ffunction-sections -fdata-sections $CXXFLAGS"
            LDFLAGS="-flto -Wl,--gc-sections $LDFLAGS"
            ;;
        clang)
            CFLAGS="-Ofast -march=native -flto=full -funroll-loops -ffunction-sections -fdata-sections $CFLAGS"
            CXXFLAGS="-Ofast -march=native -flto=full -funroll-loops -ffunction-sections -fdata-sections $CXXFLAGS"
            LDFLAGS="-flto=full -Wl,--gc-sections $LDFLAGS"
            ;;
        *)
            CFLAGS="-Ofast $CFLAGS"
            CXXFLAGS="-Ofast $CXXFLAGS"
            ;;
    esac

    # Attempt Graphite (GCC only) minimal safe set.
    if test "$compiler_family" = "gcc"; then
        GRAPHITE_FLAGS="-fgraphite-identity -floop-nest-optimize"
        for gf in $GRAPHITE_FLAGS; do
            AX_CHECK_COMPILE_FLAG([$gf],
                [CFLAGS="$CFLAGS $gf"; CXXFLAGS="$CXXFLAGS $gf"; graphite_enabled=yes])
        done
    fi
    # Attempt Polly (Clang only)
    if test "$compiler_family" = "clang"; then
        POLLY_FLAGS="-mllvm -polly -mllvm -polly-vectorizer=polly"
        save_CXXFLAGS="$CXXFLAGS"; save_CFLAGS="$CFLAGS"
        CXXFLAGS="$CXXFLAGS $POLLY_FLAGS"; CFLAGS="$CFLAGS $POLLY_FLAGS"
        AC_MSG_CHECKING([Polly support])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],[])],
            [AC_MSG_RESULT([yes]); polly_enabled=yes],
            [AC_MSG_RESULT([no]); CXXFLAGS="$save_CXXFLAGS"; CFLAGS="$save_CFLAGS"])
    fi

    AX_CHECK_COMPILE_FLAG([-fvisibility=hidden],
        [CFLAGS="$CFLAGS -fvisibility=hidden"; CXXFLAGS="$CXXFLAGS -fvisibility=hidden"]) 
fi

# Dependencies
PKG_CHECK_MODULES([SDL], [sdl])
PKG_CHECK_MODULES([SDL_NET], [SDL_net])
PKG_CHECK_MODULES([ZLIB], [zlib])
PKG_CHECK_MODULES([LIBPNG], [libpng])
PKG_CHECK_MODULES([PROTOBUF], [protobuf])
AC_CHECK_PROG([PROTOC], [protoc], [protoc])
AS_IF([test "x${PROTOC}" = "x"],
    [AC_MSG_ERROR([ProtoBuf compiler protoc not found.])])

# Summary
AC_MSG_NOTICE([Configuration summary:])
AC_MSG_NOTICE([  all-opts          = $enable_all_opts])
AC_MSG_NOTICE([  graphite enabled  = $graphite_enabled])
AC_MSG_NOTICE([  polly enabled     = $polly_enabled])
AC_MSG_NOTICE([  CFLAGS            = $CFLAGS])
AC_MSG_NOTICE([  CXXFLAGS          = $CXXFLAGS])
AC_MSG_NOTICE([  LDFLAGS           = $LDFLAGS])

AC_CONFIG_FILES([Makefile cc-lib/Makefile tasbot/Makefile])
AC_OUTPUT
