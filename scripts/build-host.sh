#!/usr/bin/env bash
# Configure, build and test Slow Mail on an Ubuntu 24.04 host (desktop simulator build).
# This is the build that has been exercised; see docs/TEST_LOG.md for what actually ran.
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/build-host.sh [--no-test] [--no-ui] [--clean] [-h|--help]

Configures with CMake + Ninja into $BUILD_DIR (default: build), builds, then runs ctest.

Options:
  --no-test   build only, skip ctest
  --no-ui     pass -DSLOWMAIL_BUILD_UI=OFF (core, storage, graph, CLI, tests only)
  --clean     remove $BUILD_DIR first
  -h, --help  show this text

Environment:
  BUILD_DIR   build directory (default: build)
  JOBS        parallel jobs (default: number of CPUs)

Ubuntu 24.04 packages (install once):
  sudo apt-get install cmake ninja-build g++ qt6-base-dev qt6-declarative-dev \
    qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
    qml6-module-qtquick-templates qml6-module-qtquick-window qml6-module-qtqml-workerscript \
    libgtest-dev nlohmann-json3-dev libcurl4-openssl-dev libssl-dev libsqlite3-dev
Python 3 is also needed for the contract tests (they spawn tools/fake_graph/server.py).
USAGE
}

run_tests=1
ui=ON
clean=0
for arg in "$@"; do
  case "$arg" in
    --no-test) run_tests=0 ;;
    --no-ui) ui=OFF ;;
    --clean) clean=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-build}"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
cd "$root"

for tool in cmake ninja g++; do
  command -v "$tool" >/dev/null 2>&1 || { echo "missing tool: $tool (see --help for packages)" >&2; exit 1; }
done

if [[ "$clean" -eq 1 ]]; then
  rm -rf -- "${root:?}/${build_dir:?}"
fi

cmake -S . -B "$build_dir" -G Ninja -DSLOWMAIL_BUILD_UI="$ui"
cmake --build "$build_dir" -j "$jobs"

if [[ "$run_tests" -eq 1 ]]; then
  ctest --test-dir "$build_dir" --output-on-failure -j "$jobs"
fi

echo "Build finished in $build_dir."
