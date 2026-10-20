#!/usr/bin/env bash
# Remove Slow Mail from a reMarkable Paper Pro (app directory and launcher entry).
#
# STATUS: UNTESTED. Removes only Slow Mail's own paths under /home/root. Does not touch xochitl, xovi,
# AppLoad, the OS, or developer mode. Your mail data is kept unless you ask for it to be removed.
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/rollback-device.sh [-y|--yes] [-h|--help]

UNTESTED. Over SSH, removes:
  /home/root/.local/share/slowmail-app, slowmail-app.new, slowmail-app.old
  /home/root/xovi/exthome/appload/slowmail          (the launcher entry, if present)
and, only when SLOWMAIL_REMOVE_DATA=1:
  /home/root/.local/share/slowmail                  (cache DB with tokens and mail; see THREAT_MODEL.md)

It then prints whether xochitl (the stock UI) is active. It does not start or stop anything.
If xochitl is not active, recover with:  ssh root@10.11.99.1 systemctl start xochitl   (or reboot).

Environment:
  DEVICE_HOST           ssh target (default: root@10.11.99.1)
  SLOWMAIL_REMOVE_DATA=1  also delete the data directory (this is the local wipe; queued commands are lost)
  -y, --yes             do not ask for confirmation
USAGE
}

assume_yes=0
for arg in "$@"; do
  case "$arg" in
    -y|--yes) assume_yes=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

echo "WARNING: scripts/rollback-device.sh is UNTESTED on real hardware." >&2
host="${DEVICE_HOST:-root@10.11.99.1}"
paths=(
  /home/root/.local/share/slowmail-app
  /home/root/.local/share/slowmail-app.new
  /home/root/.local/share/slowmail-app.old
  /home/root/xovi/exthome/appload/slowmail
)
if [[ "${SLOWMAIL_REMOVE_DATA:-}" == "1" ]]; then
  paths+=(/home/root/.local/share/slowmail)
fi

echo "On $host this will delete:"
printf '  %s\n' "${paths[@]}"
if [[ "$assume_yes" -ne 1 ]]; then
  read -r -p "Type 'yes' to continue: " answer
  [[ "$answer" == "yes" ]] || { echo "aborted"; exit 1; }
fi

# Paths are fixed literals above; refuse anything else defensively.
cmd="set -e;"
for p in "${paths[@]}"; do
  case "$p" in
    /home/root/.local/share/slowmail*|/home/root/xovi/exthome/appload/slowmail) cmd+=" rm -rf -- '$p';" ;;
    *) echo "refusing unexpected path: $p" >&2; exit 1 ;;
  esac
done
ssh -o ConnectTimeout=10 "$host" "$cmd"

echo "Removed. Stock UI state:"
ssh -o ConnectTimeout=10 "$host" 'systemctl is-active xochitl || true'
