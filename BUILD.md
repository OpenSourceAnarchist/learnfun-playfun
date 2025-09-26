# How to build

## Requirements:
Install the following packages:

### Ubuntu/Debian
```
sudo apt install autoconf automake build-essential libpng-dev libsdl1.2-dev libsdl-net1.2 libsdl-net1.2-dev libtool m4 make protobuf-compiler -y
```

### Fedora/CentOS
```
sudo yum install aclocal autoconf automake autoreconf gcc-c++ libpng-devel libtool libtool-devel m4 make perl-Thread-Queue protobuf-devel SDL-devel SDL_net-devel -y
```

## Compiling:
Run the following commands:
```
./autogen.sh
./configure
make
```

### High-Performance Build (Unified Flag)

By default (no flags) the project configures with `-O0` plus warnings for
maximum debuggability and determinism.

To explicitly request an optimized build use the single master flag:

```
./configure --all-opts
```

Aliases (equivalent):
```
./configure -all-opts
./configure --enable-all-opts
```

When enabled the build prepends these additive flags (compiler dependent):
* Core: `-Ofast -march=native -funroll-loops`
* LTO: `-flto` (GCC) or `-flto=full` (Clang) applied to compile and link
* Section GC helpers: `-ffunction-sections -fdata-sections` and linker `-Wl,--gc-sections`
* Loop transform attempts: minimal Graphite (`-fgraphite-identity -floop-nest-optimize`) on GCC or Polly (`-mllvm -polly ...`) on Clang if supported (silently skipped if not)
* Visibility tightening: `-fvisibility=hidden` if supported

No other micro-tuning flags are added; anything already implied by `-Ofast`
is not redundantly repeated. If a flag is not supported it is skipped.

Portability considerations:
* `-march=native` produces binaries optimized for the build host CPU only.
* `-Ofast` relaxes strict IEEE/ISO rules (fast-math, etc.). Omit `--all-opts`
  for standards-conforming or broadly redistributable builds.

Override behavior:
If you pass your own `CFLAGS`/`CXXFLAGS` containing an `-O` level they are
respected; the script will not force `-O0` in that case.

Typical workflows:
```
# Debug / instrumentation build
./configure && make -j

# Performance experiment build
./configure --all-opts && make -j
```

You can build both (in separate build directories) to compare performance.

