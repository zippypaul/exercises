#!/usr/bin/env bash
# Cross-compile Slow Mail for the reMarkable Paper Pro ("ferrari") with the official SDK.
#
# STATUS: UNTESTED. Written from docs/COMPATIBILITY.md (SDK 5.7.119 payload inspected there). It has
# NOT been run: the build environment that produced this script has no SDK and cannot reach
# developer.remarkable.com. Expect to fix it on first use and record the result in docs/TEST_LOG.md.
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/build-device.sh [-h|--help]

UNTESTED. Sources the reMarkable SDK environment script, configures with the SDK's CMake toolchain
file, builds without tests, and stages a deployable directory.

Environment:
  REMARKABLE_SDK_ENV   path of the SDK environment-setup script
                       default: /opt/codex/ferrari/5.7.119/environment-setup-cortexa53-crypto-remarkable-linux
                       (Paper Pro = cortexa53; the cortexa55 script is for Paper Pro Move / Paper Pure)
  CMAKE_TOOLCHAIN_FILE overrides the toolchain file the SDK script exports
                       (fallback: $OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake)
  BUILD_DIR            build directory (default: build-device)
  JOBS                 parallel jobs (default: number of CPUs)

Output: $BUILD_DIR/dist/slowmail-app/  (binaries + run-slowmail.sh), used by scripts/install-device.sh.

Notes:
  * Build against an SDK whose Qt is at or below the device's Qt (COMPATIBILITY.md). SDK 5.7.119 ships
    Qt 6.8.2; xochitl on OS 3.28 runs Qt 6.10.3.
  * The SDK has no Qt Widgets or Qt Sql; Slow Mail uses neither.
  * The SDK script refuses to run when LD_LIBRARY_PATH is set; this script unsets it.
USAGE
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  "") ;;
  *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
esac

echo "WARNING: scripts/build-device.sh is UNTESTED on real SDK or hardware." >&2

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk_env="${REMARKABLE_SDK_ENV:-/opt/codex/ferrari/5.7.119/environment-setup-cortexa53-crypto-remarkable-linux}"
build_dir="${BUILD_DIR:-build-device}"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
cd "$root"

if [[ ! -f "$sdk_env" ]]; then
  echo "SDK environment script not found: $sdk_env" >&2
  echo "Install the ferrari SDK (see docs/COMPATIBILITY.md) or set REMARKABLE_SDK_ENV." >&2
  exit 1
fi

unset LD_LIBRARY_PATH
# Yocto environment scripts reference unset variables; relax -u only while sourcing.
set +u
# shellcheck disable=SC1090
source "$sdk_env"
set -u

toolchain="${CMAKE_TOOLCHAIN_FILE:-}"
if [[ -z "$toolchain" && -n "${OECORE_NATIVE_SYSROOT:-}" ]]; then
  toolchain="$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake"
fi

cmake_args=(-S . -B "$build_dir" -G Ninja -DSLOWMAIL_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release)
if [[ -n "$toolchain" && -f "$toolchain" ]]; then
  cmake_args+=("-DCMAKE_TOOLCHAIN_FILE=$toolchain")
  echo "Using toolchain file: $toolchain"
else
  echo "No CMake toolchain file found (CMAKE_TOOLCHAIN_FILE='${CMAKE_TOOLCHAIN_FILE:-}'); relying on the SDK compiler variables only." >&2
fi

cmake "${cmake_args[@]}"
cmake --build "$build_dir" -j "$jobs"

dist="$build_dir/dist/slowmail-app"
rm -rf -- "${dist:?}"
mkdir -p "$dist"
staged=0
for bin in src/ui/slowmail src/cli/slowmail-cli; do
  if [[ -f "$build_dir/$bin" ]]; then
    cp "$build_dir/$bin" "$dist/"
    staged=$((staged + 1))
    command -v file >/dev/null 2>&1 && file "$dist/$(basename "$bin")" || true
  fi
done
if [[ "$staged" -eq 0 ]]; then
  echo "No binaries were produced (is Qt Quick available in the SDK? is the UI target defined?)." >&2
  exit 1
fi
cp packaging/run-slowmail.sh "$dist/"
chmod 0755 "$dist"/*
# If the UI loads QML from disk instead of embedding it, ship the QML next to the binary.
if compgen -G "src/ui/qml/*.qml" >/dev/null; then
  mkdir -p "$dist/qml"
  cp src/ui/qml/*.qml "$dist/qml/"
fi
echo "Staged $dist (UNTESTED on hardware). Next: SLOWMAIL_I_HAVE_A_BACKUP=1 scripts/install-device.sh"
