#!/usr/bin/env bash
# Linux build of the working tree on the build server over SSH, for catching gcc/clang -Werror breaks
# (e.g. -Wswitch, -Wmissing-field-initializers) that MSVC does not report, before a push makes CI find them.
# Replaces tools/wsl-linux-build.sh on this project's Windows dev machine (WSL is not used there).
#
# Run from Git Bash:  bash tools/ssh-linux-build.sh [--clang] [--test] [--host H] [ninja targets...]
#
# Needs on the server: gcc/g++ 15, clang 21, cmake >= 3.28, ninja, ccache, mold (all apt packages).
# The server is reached through the `thnak-server` alias in ~/.ssh/config; override with --host or
# AE_BUILD_HOST.
#
# The tree is shipped as a tar stream of `git ls-files` (tracked + untracked-but-not-ignored, so
# uncommitted edits are built and build output never travels), unpacked into ~/ae/src on the server.
# Files deleted locally are deleted remotely because the src dir is replaced each run; timestamps of
# unchanged files are irrelevant since ccache keys on content (preprocessor fallback).
#
# Linux tests that need root or kernel features the server lacks fail here; CI's Linux leg is the
# authority. The Docker daemon IS reachable on the server, so the Docker-dependent tests can run.
set -euo pipefail

host="${AE_BUILD_HOST:-thnak-server}"
compiler=gcc
run_tests=0
targets=()
while [ $# -gt 0 ]; do
    case "$1" in
        --clang) compiler=clang ;;
        --test) run_tests=1 ;;
        --host) host="$2"; shift ;;
        *) targets+=("$1") ;;
    esac
    shift
done

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

# Sync. Extract to a sibling dir then swap, so a dropped connection never leaves a half tree.
git ls-files -z --cached --others --exclude-standard \
    | tar --null -T - -cf - 2>/dev/null \
    | ssh "$host" 'set -e; w="$HOME/ae"; mkdir -p "$w"; rm -rf "$w/src.new"; mkdir "$w/src.new"; tar -xf - -C "$w/src.new"; rm -rf "$w/src"; mv "$w/src.new" "$w/src"'

# Deliberately all cores, overriding CONVENTIONS.md's -j4: this server is a dedicated build box
# (project owner, 2026-10-19). Do not copy that to a shared or local machine.
# Targets are passed through the environment, not interpolated into the remote command line.
ssh "$host" env AE_COMPILER="$compiler" AE_TEST="$run_tests" AE_TARGETS="${targets[*]:-}" bash -s <<'REMOTE'
set -euo pipefail
work="$HOME/ae"
build="$work/build-$AE_COMPILER"
if [ "$AE_COMPILER" = clang ]; then cc=clang; cxx=clang++; else cc=gcc; cxx=g++; fi
ccache --set-config=max_size=20G >/dev/null
ccache --set-config=compression=true >/dev/null
if [ ! -f "$build/build.ninja" ]; then
    cmake -S "$work/src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
        -DAGENTENGINE_WITH_HTTPS=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_LINKER_TYPE=MOLD
fi
# shellcheck disable=SC2086  # targets are intentionally word-split
jobs="$(nproc)"
ninja -C "$build" -j"$jobs" -k 0 $AE_TARGETS
if [ "$AE_TEST" = 1 ]; then
    ctest --test-dir "$build" -j"$jobs" --timeout 180 --output-on-failure
fi
REMOTE
