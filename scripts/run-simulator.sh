#!/usr/bin/env bash
# Run the desktop simulator (Qt UI) or the CLI against the Python fake Graph server.
# Nothing here talks to Microsoft. Data goes to a throw-away directory unless you set SLOWMAIL_DATA_DIR.
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/run-simulator.sh [ui|cli] [-- <arguments passed to the program>]

  ui   (default) start the fake server, then ./build/src/ui/slowmail
  cli  start the fake server, then ./build/src/cli/slowmail-cli <arguments>
       e.g.  scripts/run-simulator.sh cli -- signin
             scripts/run-simulator.sh cli -- refresh
             scripts/run-simulator.sh cli -- queue

The fake server is started on a free port (--port 0), with a generated inbox and device-code sign-in
that approves itself after a few polls. It is stopped when this script exits.

Environment (all optional):
  BUILD_DIR       build directory (default: build)
  GENERATE        number of generated messages (default: 50)
  AUTO_APPROVE    polls before the fake device code is approved (default: 2)
  FAKE_ARGS       extra arguments for tools/fake_graph/server.py, e.g. "--no-archive" or "--immutable-gap"
  SLOWMAIL_DATA_DIR   keep data here (default: a temporary directory, deleted on exit)
  SLOWMAIL_PRO=1  lift the four-hour inbound cooldown (testing only)
  SLOWMAIL_COOLDOWN_SECONDS  shorten the cooldown (testing only; read by the CLI)
  QT_QPA_PLATFORM=offscreen  run the UI without a display

The script sets SLOWMAIL_GRAPH_BASE_URL, SLOWMAIL_AUTHORITY and SLOWMAIL_CLIENT_ID=fake for the child process.
USAGE
}

mode=ui
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  ui|cli) mode="$1"; shift ;;
  --|"") ;;
  *) echo "unknown mode: $1" >&2; usage >&2; exit 2 ;;
esac
if [[ "${1:-}" == "--" ]]; then shift; fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-build}"
case "$build_dir" in /*) ;; *) build_dir="$root/$build_dir" ;; esac

if [[ "$mode" == "ui" ]]; then
  program="$build_dir/src/ui/slowmail"
else
  program="$build_dir/src/cli/slowmail-cli"
fi
[[ -x "$program" ]] || { echo "not built: $program (run scripts/build-host.sh first)" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required for the fake server" >&2; exit 1; }

tmp="$(mktemp -d)"
server_pid=""
cleanup() {
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf -- "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# shellcheck disable=SC2086  # FAKE_ARGS is intentionally word-split
python3 "$root/tools/fake_graph/server.py" --host 127.0.0.1 --port 0 \
  --generate "${GENERATE:-50}" --auto-approve-after "${AUTO_APPROVE:-2}" ${FAKE_ARGS:-} \
  >"$tmp/server.out" 2>"$tmp/server.err" &
server_pid=$!

port=""
for _ in $(seq 1 100); do
  if line="$(grep -m1 '^LISTENING ' "$tmp/server.out" 2>/dev/null)"; then
    port="${line#LISTENING }"
    break
  fi
  kill -0 "$server_pid" 2>/dev/null || { echo "fake server exited early:" >&2; cat "$tmp/server.err" >&2; exit 1; }
  sleep 0.1
done
[[ -n "$port" ]] || { echo "fake server did not report a port in 10 s" >&2; cat "$tmp/server.err" >&2; exit 1; }
echo "Fake Graph server on 127.0.0.1:$port (pid $server_pid)" >&2

export SLOWMAIL_GRAPH_BASE_URL="http://127.0.0.1:$port/v1.0"
export SLOWMAIL_AUTHORITY="http://127.0.0.1:$port"
export SLOWMAIL_CLIENT_ID="fake"
export SLOWMAIL_DATA_DIR="${SLOWMAIL_DATA_DIR:-$tmp/data}"

if [[ "$mode" == "ui" && -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}${QT_QPA_PLATFORM:-}" ]]; then
  echo "No DISPLAY or WAYLAND_DISPLAY set; the UI will probably fail. Try QT_QPA_PLATFORM=offscreen." >&2
fi

set +e
"$program" "$@"
rc=$?
set -e
exit "$rc"
