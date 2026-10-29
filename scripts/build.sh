#!/bin/sh
# Portable build of QtParted: configure, build, test (optional) and install or package.
#
# usage: scripts/build.sh [-s SOURCE_DIR] [-b BUILD_DIR] [-p PREFIX] [-d DESTDIR]
#                         [-t] [-i] [-k deb|rpm] [-j JOBS]
#   -s  source directory            (default: the directory containing this script's parent)
#   -b  build directory             (default: SOURCE_DIR/build)
#   -p  installation prefix         (default: /usr/local)
#   -d  staging directory (DESTDIR) for -i, e.g. for packaging
#   -t  build and run the tests (tests that need root are skipped without it)
#   -i  install (needs root unless -d is given)
#   -k  make a package with CPack: deb or rpm (implies -p /usr unless -p is given)
#   -j  parallel jobs               (default: number of CPUs)
#
# Requirements: CMake >= 3.19, a C++20 compiler, Qt >= 6.4 (Core, Gui, Widgets,
# optional LinguistTools), libparted >= 3.1 with libparted-fs-resize, libblkid and
# pkg-config.  Ninja is used when available.
set -eu

SRC=$(cd "$(dirname "$0")/.." && pwd)
BUILD=""
PREFIX=""
DESTDIR_ARG=""
TESTS=OFF
INSTALL=no
PACKAGE=""
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

while getopts "s:b:p:d:tik:j:h" opt; do
    case "$opt" in
        s) SRC=$(cd "$OPTARG" && pwd) ;;
        b) BUILD=$OPTARG ;;
        p) PREFIX=$OPTARG ;;
        d) DESTDIR_ARG=$OPTARG ;;
        t) TESTS=ON ;;
        i) INSTALL=yes ;;
        k) PACKAGE=$OPTARG ;;
        j) JOBS=$OPTARG ;;
        h) sed -n '2,19p' "$0"; exit 0 ;;
        *) sed -n '2,19p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$BUILD" ] || BUILD="$SRC/build"
if [ -z "$PREFIX" ]; then
    if [ -n "$PACKAGE" ]; then PREFIX=/usr; else PREFIX=/usr/local; fi
fi
case "$PACKAGE" in
    ""|deb|rpm) ;;
    *) echo "build.sh: unknown package type '$PACKAGE' (use deb or rpm)" >&2; exit 2 ;;
esac

GENERATOR=""
if command -v ninja >/dev/null 2>&1; then GENERATOR="-G Ninja"; fi

echo "== QtParted: source $SRC, build $BUILD, prefix $PREFIX"
# shellcheck disable=SC2086
cmake -S "$SRC" -B "$BUILD" $GENERATOR -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DQTPARTED_BUILD_TESTS="$TESTS"
cmake --build "$BUILD" --parallel "$JOBS"

if [ "$TESTS" = ON ]; then
    ctest --test-dir "$BUILD" --output-on-failure
fi

if [ "$INSTALL" = yes ]; then
    if [ -n "$DESTDIR_ARG" ]; then
        DESTDIR="$DESTDIR_ARG" cmake --install "$BUILD"
    else
        cmake --install "$BUILD"
    fi
fi

if [ -n "$PACKAGE" ]; then
    (cd "$BUILD" && cpack -G "$(echo "$PACKAGE" | tr '[:lower:]' '[:upper:]')")
    ls -1 "$BUILD"/qtparted*."$PACKAGE"
fi
