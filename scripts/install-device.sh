#!/usr/bin/env bash
# Copy a staged Slow Mail build to a reMarkable Paper Pro over SSH.
#
# STATUS: UNTESTED. Never run on hardware. Writes only under /home/root; does not touch xochitl,
# systemd units, /usr or /etc, and does NOT enable developer mode (you must already have done that
# yourself, deliberately, after reading docs/COMPATIBILITY.md section 4: it factory-resets the device).
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: SLOWMAIL_I_HAVE_A_BACKUP=1 scripts/install-device.sh [-h|--help]

UNTESTED. Refuses to run unless SLOWMAIL_I_HAVE_A_BACKUP=1 is set, meaning: the device owner has said
go (docs/DECISIONS.md D14), the device content is backed up and verified, and this is NOT a device
connected to a corporate mailbox (docs/THREAT_MODEL.md section 7).

Steps:
  1. copy build-device/dist/slowmail-app to /home/root/.local/share/slowmail-app.new over scp,
     then swap directories (avoids "text file busy" on a running binary; keeps the old copy as
     slowmail-app.old)
  2. if the AppLoad directory /home/root/xovi/exthome/appload exists, install the DRAFT launcher
     manifest packaging/slowmail.draft.json as slowmail/external.manifest.json (a GUESS, see
     packaging/README.md). Skip with SLOWMAIL_SKIP_LAUNCHER=1.

Environment:
  SLOWMAIL_I_HAVE_A_BACKUP=1   required
  DEVICE_HOST                  ssh target (default: root@10.11.99.1, the USB address, COMPATIBILITY.md)
  DIST_DIR                     staged build (default: build-device/dist/slowmail-app)
  SLOWMAIL_SKIP_LAUNCHER=1     do not touch the AppLoad directory

Not done by this script: creating /home/root/.config/slowmail/env with SLOWMAIL_CLIENT_ID /
SLOWMAIL_TENANT_ID. Do that by hand (chmod 600). Nothing secret is copied.

Rollback: scripts/rollback-device.sh (also UNTESTED).
USAGE
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  "") ;;
  *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
esac

if [[ "${SLOWMAIL_I_HAVE_A_BACKUP:-}" != "1" ]]; then
  echo "Refusing to run: set SLOWMAIL_I_HAVE_A_BACKUP=1 after reading --help." >&2
  exit 1
fi
echo "WARNING: scripts/install-device.sh is UNTESTED on real hardware." >&2

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
host="${DEVICE_HOST:-root@10.11.99.1}"
dist="${DIST_DIR:-$root/build-device/dist/slowmail-app}"
app_dir="/home/root/.local/share/slowmail-app"
appload_dir="/home/root/xovi/exthome/appload"

[[ -d "$dist" ]] || { echo "staged build not found: $dist (run scripts/build-device.sh)" >&2; exit 1; }
[[ -x "$dist/slowmail" ]] || { echo "missing executable $dist/slowmail" >&2; exit 1; }
[[ -f "$dist/run-slowmail.sh" ]] || { echo "missing $dist/run-slowmail.sh" >&2; exit 1; }

ssh_opts=(-o BatchMode=no -o ConnectTimeout=10)

echo "Checking the device is reachable: $host"
ssh "${ssh_opts[@]}" "$host" 'uname -m; cat /etc/os-release 2>/dev/null | head -3'

echo "Copying build to $app_dir.new"
ssh "${ssh_opts[@]}" "$host" "rm -rf '$app_dir.new' && mkdir -p '/home/root/.local/share'"
scp "${ssh_opts[@]}" -r "$dist" "$host:$app_dir.new"
ssh "${ssh_opts[@]}" "$host" "chmod 0755 '$app_dir.new'/* && rm -rf '$app_dir.old' && { [ ! -d '$app_dir' ] || mv '$app_dir' '$app_dir.old'; } && mv '$app_dir.new' '$app_dir'"

if [[ "${SLOWMAIL_SKIP_LAUNCHER:-}" == "1" ]]; then
  echo "Skipping launcher registration (SLOWMAIL_SKIP_LAUNCHER=1)."
elif ssh "${ssh_opts[@]}" "$host" "[ -d '$appload_dir' ]"; then
  echo "AppLoad directory found; installing DRAFT manifest (a guess, UNTESTED)."
  ssh "${ssh_opts[@]}" "$host" "mkdir -p '$appload_dir/slowmail'"
  scp "${ssh_opts[@]}" "$root/packaging/slowmail.draft.json" "$host:$appload_dir/slowmail/external.manifest.json"
  echo "Open AppLoad and use Reload to pick it up (COMPATIBILITY.md section 2)."
else
  echo "No AppLoad directory at $appload_dir; not registering with a launcher."
  echo "Launch by hand over SSH:  $app_dir/run-slowmail.sh"
fi

echo "Installed. Previous copy (if any) is at $app_dir.old. Rollback: scripts/rollback-device.sh"
