# Packaging for the reMarkable Paper Pro

**Everything in this directory is UNTESTED on hardware.** The environment that produced it has no reMarkable SDK, no device, and cannot reach developer.remarkable.com. It was written from [docs/COMPATIBILITY.md](../docs/COMPATIBILITY.md), which is itself desk research (VERIFIED / SNIPPET / UNVERIFIED tags apply there). Nothing here modifies a device until a person runs `scripts/install-device.sh` with `SLOWMAIL_I_HAVE_A_BACKUP=1`, and tonight's decision (DECISIONS.md D14) is that no device is touched.

Read first: COMPATIBILITY.md section 3 (known breakages) and section 4 (developer mode: factory reset, weakened secure boot), THREAT_MODEL.md section 7 (hard gates), and DECISIONS.md D24 (launch routes).

## Two launch routes

| | Route A: takeover (the MVP target for the Stage 1 gate) | Route B: AppLoad native QML (likely long-term) |
|---|---|---|
| Idea | Over SSH, stop `xochitl`, run the standalone Qt Quick binary on the E Ink panel, restart `xochitl` | QML frontend loaded inside `xochitl` by AppLoad, plus a separate backend process |
| Reuses the desktop UI unchanged | Yes | No: needs the UI split into QML frontend and a backend over a local socket (`core::TriageSession` is the intended boundary) |
| Files | `run-slowmail.sh`, optionally `slowmail.draft.json` | `appload/manifest.json` (DRAFT) |
| Needs a launcher | No (SSH is enough) | Yes: xovi + AppLoad >= 0.6.0 on OS 3.28 |
| Main risks | `remarkable-fail.service` reboot, passcode re-prompt after each restart, SIGKILL leaves `xochitl` stopped (COMPATIBILITY breakages 9, 10) | AppLoad 0.5.x crash-loops `xochitl` on OS 3.28; hashtab rebuild after each OS update; GPL-3.0 AppLoad QML module (breakages 1-3, licence note) |
| Evidence for the Paper Pro | Official guide route (SNIPPET only); takeover shown by a third-party browser on Paper Pro OS 3.28 (VERIFIED [3P]) | Native QML apps shown on Paper Pro Move (VERIFIED [3P]); external Qt Quick via `qtfb` reportedly does NOT work on Paper Pro |

## Files

| File | What it is | Status |
|---|---|---|
| `run-slowmail.sh` | POSIX sh wrapper for route A. Refuses to run (leaving the stock UI alone) if the binary is missing; masks `remarkable-fail.service` for the session; stops `xochitl`; runs `QT_QUICK_BACKEND=epaper slowmail -platform epaper`; restarts `xochitl` from an EXIT trap on every normal, signalled or failed exit. Cannot survive SIGKILL of the wrapper itself. Dry-run with a logging `systemctl` shim on a host: see TEST_PLAN section 3.1. | UNTESTED on a device. The `epaper` platform plugin lives on the device, not in the SDK (COMPATIBILITY section 1). Touch rotation or axis flags for the Paper Pro are unknown (`SLOWMAIL_QPA_ARGS`, env file). |
| `slowmail.draft.json` | Draft AppLoad **external** app manifest that launches `run-slowmail.sh`. See below. | **A GUESS.** UNTESTED. |
| `appload/manifest.json` | Draft AppLoad **native QML** manifest for route B. See below. | **DRAFT. A GUESS in its values.** UNTESTED. |

## About the manifests: format and what is a guess

COMPATIBILITY.md section 2 documents the AppLoad and remagic manifest formats from upstream READMEs (VERIFIED [upstream]). The field names below follow it exactly. **The values are guesses, and whether a takeover wrapper is a sensible thing for AppLoad to launch is not documented anywhere I could read.**

`slowmail.draft.json` is an `external.manifest.json` (fields from the AppLoad README): `name`, `application`, `workingDirectory`, `environment`, `args`, `qtfb`, `disablesWindowedMode`, `aspectRatio`; plus the remagic extras `id`, `version`, `description`.
- `qtfb` is `false` because the `qtfb` route is reported not to work for Qt Quick on the Paper Pro (COMPATIBILITY option B).
- Unverified, and the first thing to test: AppLoad runs inside `xochitl`, and the wrapper stops `xochitl`. If AppLoad starts the wrapper as a child of `xochitl`, stopping `xochitl` may kill the wrapper before it can restart the UI. A takeover app on Paper Pro OS 3.28 (rmweb, MIT) reportedly uses a "no-brick launcher" for this reason; its mechanism was not studied here. Until this is tested, launch route A over SSH only.
- No `icon.png` is provided (AppLoad apps use a 600x600 icon, COMPATIBILITY section 2).
- JSON cannot carry comments; this README is where the caveats live.

`appload/manifest.json` is a native QML manifest: `id` (must equal the QML `applicationID`), `name`, `loadsBackend`, `entry`, `canHaveMultipleFrontends`, `supportsScaling`, `aspectRatio`, `disablesWindowedMode`. The `width` field exists upstream but its meaning is not documented in what was read, so it is omitted. Value types and meanings of `entry`, `supportsScaling`, `loadsBackend` are guesses. A native app also needs `icon.png`, `resources.rcc` (compiled QML) and `backend/entry` (the backend binary), none of which exist yet because the UI has not been split. Manifest, icon and rcc are read once at `xochitl` start; only `backend/entry` re-executes per launch, so a new app needs an `xochitl` restart (about a minute, passcode again).

If COMPATIBILITY.md is later updated with a manifest format that differs from the above, follow COMPATIBILITY.md and fix these files.

## Install layout on the device (no systemd units, no autostart)

```
/home/root/.local/share/slowmail-app/        application (binaries, run-slowmail.sh, qml/ if shipped)
/home/root/.local/share/slowmail/            data: slowmail.sqlite3 (0600), -wal, -shm (SLOWMAIL_DATA_DIR overrides)
/home/root/.config/slowmail/env              KEY=VALUE, chmod 600: SLOWMAIL_CLIENT_ID, SLOWMAIL_TENANT_ID
/home/root/xovi/exthome/appload/slowmail/    launcher entry (only if a launcher is installed)
```

Everything lives under `/home/root`, which survives OS updates, unlike the root filesystem (COMPATIBILITY breakage 7). No systemd unit is installed and nothing starts at boot: upstream warns that editing `xochitl` systemd files to start xovi automatically on an encrypted device causes a bootloop (breakage 11). Slow Mail is only ever started by a person.

## Build, install, roll back

1. Install the SDK from reMarkable's page (not re-hosted here). Expected env script: `/opt/codex/ferrari/5.7.119/environment-setup-cortexa53-crypto-remarkable-linux` (cortexa53, not cortexa55; Paper Pro codename `ferrari`). Check the installer checksum (RESEARCH_LOG L16 records a sha256 for the 5.7.119 x86_64 installer from a third-party script; compare with what you download).
2. `scripts/build-device.sh` (`REMARKABLE_SDK_ENV` overrides the path; uses the toolchain file the SDK exports). Build against an SDK whose Qt is not newer than the device's.
3. Create `/home/root/.config/slowmail/env` on the device by hand.
4. `SLOWMAIL_I_HAVE_A_BACKUP=1 scripts/install-device.sh`
5. Run `/home/root/.local/share/slowmail-app/run-slowmail.sh` over SSH, and follow `tests/device/CHECKLIST.md`.
6. `scripts/rollback-device.sh` removes the app and launcher entry; `SLOWMAIL_REMOVE_DATA=1` also deletes the cache (the local wipe).

If the device misbehaves: reboot to the stock UI; if that fails use the official recovery tool (COMPATIBILITY section 4). Do not use the remagic one-line installer on OS 3.28 (it pins AppLoad 0.5.3, which crash-loops `xochitl`).

## Not decided / not done

- Slow Mail's own licence (DECISIONS D16) and the licence implications of AppLoad's GPL-3.0 QML module for route B (docs/LICENSES.md).
- Signed updates, a catalog entry, remagic packaging (zip + release + `catalog.json` sha256): Stage 6, not planned.
- OS 3.29.0.153 compatibility: unknown (COMPATIBILITY section 1).
