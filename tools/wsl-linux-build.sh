#!/usr/bin/env bash
# Local Linux build of the working tree, for catching gcc/clang -Werror breaks (e.g. -Wswitch,
# -Wmissing-field-initializers) that MSVC does not report, before a push makes CI find them.
#
# Run from Windows:  wsl -e bash tools/wsl-linux-build.sh [--clang] [--test] [ninja targets...]
#
# Speed comes from three choices (gcc 15, 876 steps, 2026-10-02):
#   - the tree is rsync'd onto WSL's own ext4 disk; compiling across /mnt/c (9p) is many times slower;
#   - ccache (preprocessor fallback, so a comment-only header edit is still a hit): a warm clean
#     rebuild is seconds, against minutes cold;
#   - mold for the ~350 test executables that relink after any library change (~15 s for a one-.cpp edit).
# ccache and mold need no root: if missing, the release binaries are unpacked into ~/.local.
#
# Linux tests that need a Docker/containerd daemon or root fail here; CI's Linux leg runs them.
set -euo pipefail

CCACHE_VER=4.14.1
MOLD_VER=2.42.1

compiler=gcc
run_tests=0
targets=()
for a in "$@"; do
    case "$a" in
        --clang) compiler=clang ;;
        --test) run_tests=1 ;;
        *) targets+=("$a") ;;
    esac
done

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$HOME/ae"
bin="$HOME/.local/bin"
opt="$HOME/.local/opt"
mkdir -p "$bin" "$opt" "$work"

if [ ! -x "$bin/ccache" ]; then
    curl -sfL "https://github.com/ccache/ccache/releases/download/v$CCACHE_VER/ccache-$CCACHE_VER-linux-x86_64-glibc.tar.xz" | tar xJ -C "$opt"
    ln -sf "$opt/ccache-$CCACHE_VER-linux-x86_64-glibc/ccache" "$bin/ccache"
    "$bin/ccache" --set-config=max_size=20G
    "$bin/ccache" --set-config=compression=true
fi
if [ ! -x "$bin/mold" ]; then
    curl -sfL "https://github.com/rui314/mold/releases/download/v$MOLD_VER/mold-$MOLD_VER-x86_64-linux.tar.gz" | tar xz -C "$opt"
    ln -sf "$opt/mold-$MOLD_VER-x86_64-linux/bin/mold" "$bin/mold"
fi
# gcc's -fuse-ld=mold looks for `ld.mold` on PATH.
ln -sf "$(readlink -f "$bin/mold")" "$bin/ld.mold"
export PATH="$bin:$PATH"

rsync -a --delete --exclude='/build*/' --exclude='/out/' --exclude='/.codegraph/' "$repo/" "$work/src/"

build="$work/build-$compiler"
if [ "$compiler" = clang ]; then cc=clang; cxx=clang++; else cc=gcc; cxx=g++; fi
if [ ! -f "$build/build.ninja" ]; then
    cmake -S "$work/src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
        -DAGENTENGINE_WITH_HTTPS=ON \
        -DCMAKE_C_COMPILER_LAUNCHER="$bin/ccache" -DCMAKE_CXX_COMPILER_LAUNCHER="$bin/ccache" \
        -DCMAKE_LINKER_TYPE=MOLD
fi

# CONVENTIONS.md: -j4 max for builds and tests, never -j$(nproc).
jobs=4
ninja -C "$build" -j"$jobs" -k 0 "${targets[@]}"

if [ "$run_tests" = 1 ]; then
    ctest --test-dir "$build" -j"$jobs" --timeout 180 --output-on-failure
fi
