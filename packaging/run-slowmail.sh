#!/bin/sh
# Slow Mail takeover wrapper for the reMarkable Paper Pro (route A, docs/DECISIONS.md D24).
#
# STATUS: UNTESTED on hardware. Based on a third-party recipe verified on a reMarkable 2 (OS 3.28.0.172)
# and the official guide as reported by search snippets (docs/COMPATIBILITY.md section 1, "Run-time flags").
# Paper Pro behaviour is UNVERIFIED.
#
# What it does: checks the binary exists, stops the stock UI (xochitl), runs Slow Mail on the E Ink
# display, and ALWAYS starts xochitl again when Slow Mail exits, crashes, or this script gets
# INT/TERM/HUP. It cannot do that if it is killed with SIGKILL; then run `systemctl start xochitl`
# over SSH or reboot.
#
# This is a POSIX sh script on purpose: the device ships BusyBox ash and no bash or python. For that
# reason it uses `set -eu` but not `pipefail`.
#
# Run it over SSH:  /home/root/.local/share/slowmail-app/run-slowmail.sh
# Expect: xochitl restart takes about a minute and asks for the passcode again (COMPATIBILITY breakage 10).
#
# Environment (all optional):
#   SLOWMAIL_APP_DIR   default /home/root/.local/share/slowmail-app
#   SLOWMAIL_BIN       default $SLOWMAIL_APP_DIR/slowmail
#   SLOWMAIL_ENV_FILE  default /home/root/.config/slowmail/env  (KEY=VALUE lines; keep it chmod 600;
#                      put SLOWMAIL_CLIENT_ID and SLOWMAIL_TENANT_ID here, never in the repository)
#   SLOWMAIL_QPA_ARGS  extra platform arguments (default: -platform epaper)
#   SLOWMAIL_NO_MASK=1 do not mask remarkable-fail.service
set -eu

usage() {
  cat <<'USAGE'
Usage: run-slowmail.sh [-h|--help] [-- app arguments]
UNTESTED takeover launcher: stops xochitl, runs slowmail with QT_QUICK_BACKEND=epaper, restarts xochitl.
USAGE
}
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --) shift ;;
esac

APP_DIR="${SLOWMAIL_APP_DIR:-/home/root/.local/share/slowmail-app}"
BIN="${SLOWMAIL_BIN:-$APP_DIR/slowmail}"
ENV_FILE="${SLOWMAIL_ENV_FILE:-/home/root/.config/slowmail/env}"
QPA_ARGS="${SLOWMAIL_QPA_ARGS:--platform epaper}"

# Fail safe: do not touch the stock UI if there is nothing to run.
if [ ! -x "$BIN" ]; then
  echo "run-slowmail: binary not found or not executable: $BIN (stock UI left running)" >&2
  exit 127
fi

if [ -f "$ENV_FILE" ]; then
  set -a
  # shellcheck disable=SC1090
  . "$ENV_FILE"
  set +a
fi

child=""
restored=0
restore_ui() {
  if [ "$restored" -eq 1 ]; then
    return 0
  fi
  restored=1
  if [ -n "$child" ] && kill -0 "$child" 2>/dev/null; then
    kill -TERM "$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
  fi
  if [ "${SLOWMAIL_NO_MASK:-0}" != "1" ]; then
    systemctl unmask --runtime remarkable-fail.service 2>/dev/null || true
  fi
  systemctl start xochitl || echo "run-slowmail: FAILED to start xochitl; run 'systemctl start xochitl' or reboot" >&2
}
trap restore_ui EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [ "${SLOWMAIL_NO_MASK:-0}" != "1" ]; then
  # Third-party finding (rM2): stopping xochitl can trigger remarkable-fail.service, which reboots the tablet.
  systemctl mask --runtime remarkable-fail.service 2>/dev/null || true
fi
systemctl stop xochitl

export QT_QUICK_BACKEND=epaper
rc=0
# QPA_ARGS is intentionally word-split.
# shellcheck disable=SC2086
"$BIN" $QPA_ARGS "$@" &
child=$!
wait "$child" || rc=$?
child=""
exit "$rc"
