#!/usr/bin/env bash
# Build and run the native unit tests on the host (no device, no emulator).
# game.cpp is platform-independent; app/src/main/cpp/test/host/ stands in for
# <android/log.h>, and Google Test comes from the NDK's bundled sources (or a
# system copy under /usr/src/googletest).
#
#   scripts/host-tests.sh                 # run every test
#   scripts/host-tests.sh --gtest_filter='Settings.*'
#
# Set NDK to point at an NDK root if it is not under $ANDROID_HOME/ndk.
set -euo pipefail
cd "$(dirname "$0")/.."
cpp=app/src/main/cpp
out=build/host-tests
mkdir -p "$out"

gtest=""
if [ -n "${NDK:-}" ] && [ -d "$NDK/sources/third_party/googletest" ]; then
    gtest="$NDK/sources/third_party/googletest"
else
    sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
    for d in "$sdk"/ndk/*/sources/third_party/googletest; do
        [ -d "$d" ] && gtest="$d"
    done
fi
if [ -z "$gtest" ] && [ -d /usr/src/googletest/googletest ]; then
    gtest=/usr/src/googletest/googletest
fi
if [ -z "$gtest" ]; then
    echo "Google Test sources not found: install an NDK or libgtest-dev, or set NDK=<ndk root>" >&2
    exit 2
fi

cxx="${CXX:-c++}"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra \
    -I"$cpp" -I"$cpp/test/host" -I"$gtest/include" -I"$gtest" \
    "$cpp/test/game_test.cpp" "$cpp/test/audio_stub.cpp" "$cpp/game.cpp" \
    "$gtest/src/gtest-all.cc" "$gtest/src/gtest_main.cc" \
    -lpthread -o "$out/game_tests"

# The persistence tests write next to TMPDIR; keep that inside the build tree.
export TMPDIR="$(pwd)/$out"
exec "$out/game_tests" "$@"
