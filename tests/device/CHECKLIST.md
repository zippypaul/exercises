# Device test checklist (reMarkable Paper Pro)

Status: UNTESTED. Nothing on this page has been run. Every box is empty.

Prerequisites (all required, otherwise stop):
- [ ] Device owner has given an explicit go decision for developer mode, factory reset and unsupported installation (DECISIONS D14).
- [ ] Device is a spare or personal device that is **not** connected to a corporate mailbox (THREAT_MODEL section 7).
- [ ] Verified backup of the device content, and the official recovery tool available on a host (COMPATIBILITY section 4).
- [ ] Synthetic tenant and mailbox only. No real mail.
- [ ] You have read COMPATIBILITY.md section 3 (known breakages) and will **not** use the remagic one-line installer on OS 3.28.

Record in [docs/TEST_LOG.md](../../docs/TEST_LOG.md): date, tester, device model (expect Paper Pro "ferrari"), `cat /etc/os-release`, `/etc/version`, `ls -l /lib/libQt6Core.so.6*`, SDK version used, launcher versions (xovi bundle, AppLoad), Slow Mail commit id.

## Stage 1 gate: native E Ink hello-world

Pass criteria (handover section 10): launch/exit 20 times; reboot and stock UI still works; portrait text legible; no crash loop; measured input and refresh latency.

### 1. Build and deploy (host)
- [ ] `source /opt/codex/ferrari/5.7.119/environment-setup-cortexa53-crypto-remarkable-linux` works (or the path recorded in `REMARKABLE_SDK_ENV`).
- [ ] `scripts/build-device.sh` completes. Record compiler version and any warnings. (Script is UNTESTED.)
- [ ] `file` on the produced binary says `ARM aarch64`. Run `ldd`-equivalent check (`aarch64-remarkable-linux-readelf -d`) and list needed libraries; note any not present on the device.
- [ ] `SLOWMAIL_I_HAVE_A_BACKUP=1 scripts/install-device.sh` copies the app to `/home/root/.local/share/slowmail-app` without error. (UNTESTED.)
- [ ] Binary is replaced via a temp name and `mv` if an older copy may be running (`scp` onto a running binary fails with ETXTBSY, COMPATIBILITY breakage 8).

### 2. First launch (takeover route, DECISIONS D24)
- [ ] Over SSH run `/home/root/.local/share/slowmail-app/run-slowmail.sh`.
- [ ] Observe: does `remarkable-fail.service` fire (device reboots)? Does it need `systemctl mask --runtime remarkable-fail.service`? Record. (COMPATIBILITY breakage 9; rM2 evidence only.)
- [ ] Does `QT_QUICK_BACKEND=epaper ... -platform epaper` start on the Paper Pro? Record the exact error if not. Record whether touch rotation or axis flags are needed (`QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS`).
- [ ] Inbox-shaped static screen appears (use the fake server data or the built-in demo mode; no real mailbox).
- [ ] Touch works: tap each action button, scroll by buttons (no kinetic flick).

### 3. Launch / exit 20 times
Table: cycle, time to first paint (s), time back to stock UI after exit (s), passcode prompt (Y/N), problems.

| # | First paint (s) | Back to stock UI (s) | Passcode re-prompt | Notes |
|---|---|---|---|---|
| 1 | | | | |
| 2 | | | | |
| 3 | | | | |
| 4 | | | | |
| 5 | | | | |
| 6 | | | | |
| 7 | | | | |
| 8 | | | | |
| 9 | | | | |
| 10 | | | | |
| 11 | | | | |
| 12 | | | | |
| 13 | | | | |
| 14 | | | | |
| 15 | | | | |
| 16 | | | | |
| 17 | | | | |
| 18 | | | | |
| 19 | | | | |
| 20 | | | | |

- [ ] 20 of 20 launches reached the UI. After each exit `systemctl is-active xochitl` was `active`.
- [ ] No crash loop: `systemctl show xochitl -p NRestarts` is 0 (or unchanged from before) after the run.
- [ ] One exit by closing from the UI, one by `kill -TERM`, one by `kill -9` of the app; stock UI came back each time.
- [ ] Deliberately `kill -9` the wrapper once: record that `xochitl` stays stopped and how you recovered (expected limitation).

### 4. Reboot test
- [ ] Reboot the device (hold power as documented) with Slow Mail installed. Stock UI comes up normally.
- [ ] Developer-mode notice still shown on boot (expected). Slow Mail data directory still present.
- [ ] Re-run one launch after the reboot.

### 5. Legibility (portrait, front light on and off)
- [ ] Body text readable at arm's length at the default size; record the font sizes used.
- [ ] Mid-greys used anywhere? Anything below about `#4A4A4A` on white is expected to wash out (COMPATIBILITY section 6); list problems.
- [ ] Tap targets: all action buttons hit reliably with a finger. Record target size in mm (about 6.5 mm works, 3.5 mm does not).
- [ ] Long subject (200+ chars), RTL text, combining marks and emoji do not hide any control (A14, D-UNICODE).
- [ ] No animation anywhere; no auto-scroll; no countdown ticking on screen.
- [ ] Ghosting after a page turn: record whether a full refresh is needed and how it is requested. Photo attached.

### 6. Latency measurement
Method: film the screen at 60 fps or use a phone slow-motion mode; count frames from touch contact to first visible change; or use the evdev timestamp from `evtest` against a frame in the film. Do 10 samples each and record median and worst.

| Action | Median (ms) | Worst (ms) | Method |
|---|---|---|---|
| Tap Do/Archive button to visible response | | | |
| Tap a queue row to message detail painted | | | |
| Page turn in a long message | | | |
| Cold start to first paint | | | |

- [ ] Results recorded; no target value is set in the handover, so state the number and whether the owner finds it acceptable.

## Device behaviour items (feed the assumptions)

- [ ] **D-WIFI.** With Wi-Fi off, open the app: cached mail readable, actions queue with a Pending indicator (A09). Turn Wi-Fi on: queued actions flush.
- [ ] **D-SUSPEND.** Leave the device idle until deep suspend (about 40 s on Move; Paper Pro unknown). Wake it. Does Wi-Fi return, and after how many seconds? Start a refresh and let the screen time out during it: does it stall, fail or resume? A sync must be user-initiated with the device awake (R-SLEEP-SYNC, ASM-27). Record the behaviour.
- [ ] **D-CLOCK.** Record `date`, `timedatectl`, `/etc/localtime`. Does the clock stay correct after a long sleep and after a boot with Wi-Fi off? (ASM-29)
- [ ] **D-TLS.** From the app (not `wget`), reach the Graph and Entra hosts (use the fake server over loopback first, then a synthetic tenant). Check `/etc/ssl/openssl.cnf.d/10-reduce-tls-ciphers.cnf` for a cipher restriction (ASM-18).
- [ ] **D-KEYSTORE.** Look for any keyring daemon, `keyctl`, `/dev/tpm*` (COMPATIBILITY open item 10). Record what you find (ASM-28).
- [ ] **D-PERMS.** `ls -l` the data directory and DB: directory 0700, database and `-wal`/`-shm` 0600.
- [ ] **D-UNICODE.** Load 1000 synthetic messages with long Unicode subjects: scrolling by buttons stays responsive; memory stays bounded (`free`, `top`) (A14).
- [ ] **D-WIPE.** Sign out with wipe: DB, `-wal`, `-shm` gone; no token strings remain in the data directory (`grep -ri refresh`).

## Rollback and recovery

- [ ] `scripts/rollback-device.sh` removes the app directory and the launcher entry. Confirm the stock UI is unaffected. (UNTESTED.)
- [ ] If the device crash-loops: reboot to stock; if that fails, use the official recovery tool (COMPATIBILITY section 4). Record steps and time taken.

## Result

- Date / tester:
- OS build / SDK / launcher versions:
- Pass / fail for the Stage 1 gate:
- Open problems:
