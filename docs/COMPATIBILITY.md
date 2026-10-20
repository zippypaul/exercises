# Slow Mail - reMarkable Paper Pro compatibility

Research date: 2026-10-20 (all sources retrieved that day). Nothing here was tested on a physical device.

**Status legend**
- **VERIFIED** - I read the primary text (vendor artifact, upstream repo file, or issue text). Tag in brackets: `[vendor]` reMarkable artifact, `[upstream]` project repo/issue, `[3P]` a third-party project's own docs (accurate for them, not independently reproduced).
- **SNIPPET** - search-engine result summary only; the page itself was unreachable (developer.remarkable.com, remarkable.guide and support.remarkable.com are DNS/proxy-blocked here).
- **UNVERIFIED** - memory or inference. Do not treat as fact.

Full per-source evidence: [RESEARCH_LOG.md](RESEARCH_LOG.md) (entry numbers `L#` below).

## 0. Corrections to the brief's assumptions

| Assumption in the brief | Finding | Status | Evidence |
|---|---|---|---|
| Env script is `environment-setup-cortexa55-remarkable-linux` | That name is for **Paper Pro Move (chiappa) and Paper Pure (tatsu)**. The original **Paper Pro (ferrari)** is `environment-setup-cortexa53-crypto-remarkable-linux` (Cortex-A53, i.MX8M Mini). | VERIFIED [vendor] | L2, L3, L1 |
| Paper Pro codename | `ferrari` (Move = `chiappa`, Pure = `tatsu`) | VERIFIED [vendor] | L1, L3 |
| Run with `QT_QUICK_BACKEND=epaper` + `-platform epaper` | Official guide reportedly says so (SNIPPET). Working on a rM2 / OS 3.28.0.172 shown by a third-party Makefile (VERIFIED [3P], different device). **Not confirmed on Paper Pro.** | SNIPPET / VERIFIED [3P] rM2 | L6, L17 |
| remagic supports Paper Pro on current OS | `main` pins AppLoad v0.5.3, which crash-loops xochitl on OS 3.28.x. Fix PR #3 still open. | VERIFIED [upstream] | L8, L9, L10 |
| remagic README: "Ferrari / Chiappa / Tatsu - i.MX8MM" | Inconsistent: kernel repo builds chiappa/tatsu with `-soc IMX9` (Cortex-A55). Only ferrari is i.MX8MM. | VERIFIED [vendor] | L1, L16 |
| Developer mode "does not void warranty" (remagic doc) | Official wording (snippet) says modifications "may also void warranty coverage for resulting defects". Conflict; treat official as authoritative. | SNIPPET vs VERIFIED [3P] | L6, L8 |

## 1. Compatibility matrix (Paper Pro / ferrari)

"Launcher" = xovi bundle + AppLoad. Qt column = Qt of the SDK you would build against / Qt on the device's xochitl.

| Device | OS version(s) | Official SDK | Qt (SDK / device) | Launcher versions | Status | Evidence |
|---|---|---|---|---|---|---|
| Paper Pro (ferrari) | 3.27.0.x - 3.27.3.0 | `ferrari` **5.7.119** (dir `3.27.0.97`), x86_64 and aarch64 hosts | **6.8.2** (VERIFIED [vendor]) / device Qt not read for 3.27 (UNVERIFIED; rmweb says SDK ABI-compatible) | xovi bundle `v19-23052026` + AppLoad `v0.5.2`/`v0.5.3`. Needs `xovi/rebuild_hashtable` (else xochitl crash-loop, device reboots to stock) | **Works with hashtab rebuild** per 3 user reports; not tested by us | L3, L10 (remagic #2, #5, #7), L15 |
| Paper Pro (ferrari) | 3.28.0.162 / 3.28.0.164 (3.28 beta/RC builds) | 5.7.119 still ABI-compatible per rmweb; official table lists 5.8.203 for 3.28.0.172 | 6.8.2 / **6.10.3** (device, VERIFIED [3P] on 3.28.0.164) | AppLoad **v0.5.3 crash-loops xochitl** (hook hash unresolved). AppLoad >= v0.6.0 (3.28 support PR #59 merged 2026-09-07) | **v0.5.3 BROKEN; v0.6.0 expected OK** (only user reports; one launch-time freeze with the Riddle takeover app, remagic #12) | L11, L10, L16 |
| Paper Pro (ferrari) | 3.28.0.172 (appears to be latest stable) | `ferrari` **5.8.203** per developer.remarkable.com/links (SNIPPET); installer URL not found | **6.10.x** per one 3P doc (VERIFIED [3P] claim); SDK itself not inspected | remagic's pinned AppLoad v0.5.3 fails. Manual fix reported: `rebuild_hashtable` + AppLoad v0.6.0 `appload.so` (xovi 0.3.3, ext v19) | **BROKEN via remagic one-liner; manual workaround reported working** (single comment) | L10 (remagic #11), L11 |
| Paper Pro (ferrari) | 3.29.0.153 | unknown | unknown | unknown | **UNKNOWN** - remagic #13 (2026-10-18) asks if tested; no answer | L10 |
| Paper Pro Move (chiappa) | 3.22 - 3.28 | `chiappa` 5.2.96 (3.22.0.65) ... 5.8.203; env `...cortexa55...` | 6.8.2 (device 5.7.126) | AppLoad 0.5.2 works on 3.27; 3.28 needs 0.6.0 | Out of scope for target; useful because many references are Move-based | L1, L15 |
| Older SDK (reference) | 3.14.x | `ferrari` 4.0.813, `/opt/codex/ferrari/4.0.813/environment-setup-cortexa53-crypto-remarkable-linux` | not stated | - | Superseded | L5 |

### Toolchain facts (ferrari, SDK 5.7.119 - installer and payload inspected directly)

| Item | Value | Status |
|---|---|---|
| Installer | `https://storage.googleapis.com/remarkable-codex-toolchain/3.27.0.97/ferrari/remarkable-production-image-5.7.119-ferrari-public-x86_64-toolchain.sh` (HTTP 200, 482,953,304 bytes, last-modified 2026-05-21). `...-aarch64-toolchain.sh` variant also exists (3P reference). | VERIFIED [vendor] |
| Host | x86_64 Linux (aarch64 host builds exist from 3.27.0.97). Needs `xz`. Refuses to run if `LD_LIBRARY_PATH` is set. | VERIFIED [vendor] |
| Install | `chmod +x` then run; default dir `DEFAULT_INSTALL_DIR="/opt/codex/ferrari/5.7.119"` (override with `-d <dir>`; `-d` not seen in the header I read - UNVERIFIED) | VERIFIED [vendor] / UNVERIFIED flag |
| **Env script** | `source /opt/codex/ferrari/5.7.119/environment-setup-cortexa53-crypto-remarkable-linux` | VERIFIED [vendor] |
| Target triple / CPU | `aarch64-remarkable-linux`; `-mcpu=cortex-a53+crc+crypto -mbranch-protection=standard`; sysroot `sysroots/cortexa53-crypto-remarkable-linux`; GCC 13.4.0, glibc 2.39 (Yocto scarthgap, "codex" distro) | VERIFIED [vendor] / [3P] for GCC |
| CMake | Env script exports `CMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake`; it sets `QT_HOST_PATH` to the native sysroot. Use `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$CMAKE_TOOLCHAIN_FILE` (some other SDK versions do not export the variable; fall back to the path above) | VERIFIED [vendor] / [3P] |
| Qt in SDK | **6.8.2**: Core, Gui, Qml, Quick, QuickControls2/Templates2 (Basic, Fusion, Imagine, Material, Universal), QuickLayouts, QuickShapes, Svg, Network, DBus, WebSockets, Xml, Concurrent, Test, LabsPlatform/Settings. **No Qt6Widgets, Sql, Multimedia, PrintSupport in the target sysroot.** | VERIFIED [vendor] |
| Qt plugins in SDK | platforms: `minimal`, `offscreen`, `vnc` only (no `epaper`, `linuxfb`, `xcb`, `wayland`). `scenegraph/libqsgepaper.so`. generic: evdev keyboard/mouse/tablet/touch. imageformats: jpeg, svg. tls: openssl backend. | VERIFIED [vendor] |
| Qt Quick only | Official guide: "only pure Qt Quick applications (no Qt Widgets)" | SNIPPET (+ SDK has no Widgets in target: VERIFIED) |
| Qt minimum in official guide | CMake sample uses `qt_standard_project_setup(REQUIRES 6.5)` | SNIPPET |
| Version rule | Build against an SDK **at or below** the device's OS: a Qt 6.10 build fails on a 6.8.2 device with `version 'Qt_6.10' not found` and the app never shows in AppLoad | VERIFIED [3P] |
| Device runtime | xochitl is a Qt 6.10.3 Quick app on OS 3.28.0.164; display via closed `/usr/lib/plugins/platforms/libepaper.so` ("epaper" QPA) -> `imx-drm`; other QPA plugins: offscreen, minimal, vnc. Panel 1620x2160 Gallery 3 colour; no `/dev/fb0`; no GPU driver (CPU rendering only); ~2 GB RAM | VERIFIED [3P] |

### Run-time flags for a standalone Qt Quick binary (takeover mode)

| Claim | Status | Evidence |
|---|---|---|
| Official guide: stop xochitl, run `QT_QUICK_BACKEND=epaper ./app -platform epaper`, restart xochitl | SNIPPET | L6 |
| rM2 on OS 3.28.0.172: `systemctl mask --runtime remarkable-fail.service; systemctl stop xochitl; QT_QUICK_BACKEND=epaper QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=rotate=180:invertx ./app -platform epaper; systemctl unmask --runtime remarkable-fail.service; systemctl start xochitl` | VERIFIED [3P] (rM2 only) | L17 |
| rM1/rM2 only: `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=rotate=180` (+`:invertx` on rM2). No Paper Pro value found. | VERIFIED [3P] (RmAnki) | L18 |
| Paper Pro: nothing in the SDK provides an `epaper` platform plugin, so the plugin must come from the device | VERIFIED [vendor]+[3P] | L3, L16 |

## 2. Distribution options for Slow Mail (what the evidence supports)

| Option | How | Evidence for Paper Pro | Verdict |
|---|---|---|---|
| A. AppLoad **native QML** app: QML frontend loaded into xochitl + separate backend binary | `manifest.json`, `icon.png` (600x600), `resources.rcc`, `backend/entry` in `/home/root/xovi/exthome/appload/<id>/` | Used by MLB scoreboard (MIT) on Move; frontend/backend split documented incl. wire protocol | Best-documented; frontend inherits xochitl's Qt (6.10.3 on 3.28) and OpenSSL policy |
| B. AppLoad **external** app (`external.manifest.json`, `qtfb: true`) | Any aarch64 binary draws to a qtfb framebuffer | Qt `linuxfb` route reported **not working** on Paper Pro (no `/dev/fb0`; shim accepts only RMPP/RMPPM/CHIAPPA/FERRARI models; plugin not shipped) | Not viable for Qt Quick (VERIFIED [3P]); fine for apps that speak qtfb natively (KOReader) |
| C. **Takeover**: stop xochitl, own process drives the panel (`-platform epaper`, or vendor waveform engine as Riddle/Quill do) | systemd stop/start | rmweb (MIT) does exactly this on Paper Pro OS 3.28 with a no-brick launcher; Riddle takeover froze on 3.28.0.162 (remagic #12) | Works but riskier; see Known breakages |
| D. Official route | Official SDK + guide, developer mode only (no AppLoad) | Guide unreadable here (SNIPPET) | Needs physical-device check |

### AppLoad / remagic manifest formats (VERIFIED [upstream])

External app (`external.manifest.json`, AppLoad README): `name` (required), `application` (required, path), `workingDirectory`, `environment` (string dict), `args` (list), `qtfb` (bool; AppLoad passes the key in env `QTFB_KEY`), `disablesWindowedMode` (bool), `aspectRatio` (`auto` | `original` | `move` | float).
remagic additionally reads `id`, `version`, `description` (AppLoad ignores unknown keys); optional `settings.schema.json` (kinds `text`, `secret`, `select`); `settings.env` is written chmod 600 and never packaged. Publish = zip + GitHub release `v<version>` + `catalog.json` entry `{id, version, url, sha256}` (human-reviewed PR; checksum enforced).
Native QML app (`manifest.json`): `id` (must equal QML `applicationID`), `name`, `loadsBackend`, `entry`, `canHaveMultipleFrontends`, `supportsScaling`, `aspectRatio`, `width`, `disablesWindowedMode`. Manifest/icon/rcc are read once at xochitl start; only `backend/entry` re-executes per launch.

### Launcher versions and licences

| Component | Latest seen (retrieved 2026-10-20) | Licence | Status |
|---|---|---|---|
| remagic CLI | `cli-v0.2.0` (07 Jul 2026); Store `v0.1.1` (06 Jul). `main` last commit 13 Jul 2026. | MIT | VERIFIED [upstream] |
| asivery/rm-appload | **v0.6.0** (19 Sep; year inferred 2026), v0.5.3 (21 May), v0.5.2 (08 May, "compatibility with 3.27"), v0.4.1 (04 Dec, year inferred 2025, "support for version 3.24") | GPL-3.0 | VERIFIED [upstream]; release years inferred |
| asivery/rm-xovi-extensions | `v19-23052026` (Latest, 23 May), `pre-v20-08092026` (pre-release, 08 Sep) | GPL-3.0 | VERIFIED [upstream] |
| asivery/xovi (loader) | 0.3.3 (from issue text; Releases page empty in capture) | LGPL-3.0 | VERIFIED [upstream] (version from issue) |
| xovi-tripletap | no release listed; triple-press power toggle | GPL-3.0 | VERIFIED [upstream] |
| qmldiff / `.qmd` | `.qmd` = QML diff files applied by xovi `qt-resource-rebuilder`; needs a per-OS **hashtab** | GPL-3.0 | VERIFIED [upstream] |
| vellum-cli | apk-based package manager (reManager GUI) | MIT | VERIFIED [upstream] |
| freeMarkable | GUI installer of xovi+AppLoad; Paper Pro "Fully Supported"; 3.28 unstated | MIT | VERIFIED [upstream] |
| Toltec / remarkable-hacks | Do not support OS 3.28 (3P statement) | n/a | VERIFIED [3P] |

Licence note (UNVERIFIED, legal inference): the AppLoad QML module (`net.asivery.AppLoad`) is GPL-3.0. A native-QML frontend that imports it may carry GPL obligations; an external process that only speaks qtfb or a socket probably does not. Get a legal read before choosing the licence for Slow Mail.

## 3. Known breakages

| # | Breakage | Affects | Workaround | Status | Evidence |
|---|---|---|---|---|---|
| 1 | AppLoad v0.5.x hooks do not resolve on OS 3.28 -> `Couldn't resolve the hashed identifier ... required by AppLoad hooks in main UI`; xochitl crash-loops until reboot to stock | Paper Pro 3.28.0.162/.164/.172; also rM2 3.28.0.162 | Use AppLoad >= v0.6.0 (PR #59, merged 2026-09-07); `rebuild_hashtable` after each OS update. Builds are per-OS: v0.6.0 reportedly drops <=3.27 | VERIFIED [upstream] (reports); fix efficacy SNIPPET-grade | L11 (#65, #59, #75), L10 (#11), L16 |
| 2 | `remagic setup` skips/fails non-interactive hashtab rebuild and prints "AppLoad works without it"; on 3.27.3.0 and 3.28 xovi then crash-loops xochitl and device reboots to stock; `remagic doctor` stays green | Paper Pro 3.27.x, 3.28 | Run `/home/root/xovi/rebuild_hashtable` over SSH (press Enter, enter device passcode on the tablet, wait for "Hashtab saved") then `xovi/start`. One report: rebuild froze tablet >10 min on 3.28.0.172 | VERIFIED [upstream] | L10 (#2, #5, #7, #11) |
| 3 | remagic `main` pins AppLoad v0.5.3, so one-line install is broken on 3.28; fix PR #3 open since 2026-07-08, commit `a99f90e` linked 2026-10-18 but not on `main` when checked | all 3.28 | Manual workaround in #1 | VERIFIED [upstream] | L9, L10 |
| 4 | remagic catalog sha256 mismatches (riddle 0.3.0, chromium 0.5.0) after assets were re-uploaded | `remagic install` | install from a local folder | VERIFIED [upstream] | L10 (#6-#10) |
| 5 | Riddle takeover freezes on 3.28.0.162: `riddle-takeover.service` stops xochitl, display never returns; needs quill fix (quill#5) | takeover-style apps on 3.28 | none confirmed | VERIFIED [upstream] (open) | L10 (#12) |
| 6 | AppLoad v0.6.0 race on app close can crash (open issue, reproduced on Paper Pure) | v0.6.0 | none | VERIFIED [upstream] | L11 (#82) |
| 7 | OS update wipes rootfs changes (systemd units, /usr, /etc); `/home/root` survives. Re-run `remagic setup` / persistence after each update | all | install only under `/home/root` | VERIFIED [upstream]/[3P] | L8, L16, L17 |
| 8 | `scp` over a running AppLoad backend fails (ETXTBSY); AppLoad leaves backends running after close | deploy | copy to temp name then `mv` | VERIFIED [3P] | L15 |
| 9 | `systemctl stop xochitl` can trigger `OnFailure=remarkable-fail.service`, which reboots the tablet (observed rM2 3.28.0.172) | takeover mode | `systemctl mask --runtime remarkable-fail.service` during session | VERIFIED [3P] (rM2; Paper Pro UNVERIFIED) | L17 |
| 10 | Every xochitl (re)start re-runs the cold-boot lock and asks for the passcode again; restart takes ~1 min and returns to lock screen | takeover mode, AppLoad frontend redeploys | batch redeploys; avoid restarts | VERIFIED [3P] (rM2, Move) | L15, L17 |
| 11 | xovi on an encrypted device must NOT be auto-started via edited xochitl systemd files: bootloop needing manual recovery. Use tethered start / triple-press power button | all | `xovi-tripletap` | VERIFIED [upstream] | L12, L8 |
| 12 | OS 3.28 beta builds (3.28.0.157/.162) and earlier 3.19/3.20 betas broke third-party tooling; developers advise against beta channels | all | stay on stable | VERIFIED [upstream] | L11, L21 |
| 13 | Downgrade: codexctl cannot downgrade a device at or above 3.11.2.5 below 3.11.2.5 | recovery | use official recovery tool | VERIFIED [upstream] | L24 |

## 4. Developer mode (Paper Pro)

| Fact | Status | Evidence |
|---|---|---|
| Enabling performs a **factory reset**; unsynced data is lost; a notice shows on every boot; cannot be skipped | VERIFIED [3P] quoting reMarkable's warning; SNIPPET official | L8, L6 |
| Path: Settings > General > Paper Tablet > Software > Advanced > Developer Mode > Enable; confirm with two power-button presses | VERIFIED [3P] | L8 |
| Weakens secure boot (breaks chain of trust; official: "security ... fundamentally weakened"); **disk encryption stays on**; bootloader stays signed | SNIPPET (official) + VERIFIED [3P] | L6, L8 |
| SSH: `ssh root@10.11.99.1` over USB; password under Settings > Help > Copyrights and licenses (GPLv3 Compliance/SSH section); USB-only unless "SSH over Wi-Fi" toggled (Settings > General > Software > Advanced; resets with factory/dev-mode reset) | VERIFIED [3P]; SNIPPET official | L8 |
| Reset also resets SSH password and removes keys | SNIPPET | L6 |
| Leaving dev mode: official recovery tool (`rm_recover reset` or `restore`; `discover-device` first; hold power ~30 s, release, short press) - both turn dev mode off | VERIFIED [3P] | L8 |
| Warranty: official says modifications may void warranty coverage for resulting defects; remagic says not voided | SNIPPET vs VERIFIED [3P]; conflict | L6, L8 |
| xochitl can be stopped/started with systemd (`systemctl stop/start/restart xochitl`) | VERIFIED [3P] | L15, L17, L16 |
| Encrypted-device risks: see breakages 9, 10, 11 | VERIFIED [3P]/[upstream] | - |

## 5. Platform behaviour for a third-party mail app

| Topic | Finding | Status | Evidence |
|---|---|---|---|
| Secure storage / keychain | **No evidence found** of a keyring, Secret Service, TPM API or Qt Keychain on the device OS. SDK 5.7.119 target sysroot contains no libsecret, keyutils or tpm2-tss. Existing apps keep secrets in plaintext (reTaskable `config.toml`, remagic `settings.env` chmod 600, an app API key in a plaintext manifest). `/home` (46 GB) sits on an encrypted device-mapper volume (`/dev/mapper/home-encrypted-disk`), so at-rest protection comes from device encryption only. | VERIFIED [vendor] listing / [3P]; absence = no evidence | L3, L16, L20 |
| Storage layout | rootfs 500 MB ~91% full; `/etc`, `/var/lib` overlays; persistent `/home`; OTA replaces rootfs | VERIFIED [3P] | L16 |
| Clock / timezone | Move OS 5.7.126: clock is **UTC** (`/etc/localtime -> Universal`), full tzdata present. NTP behaviour: no evidence found. Paper Pro: UNVERIFIED | VERIFIED [3P] (Move) | L15 |
| Suspend | Move: deep suspend ~40 s after last touch; no timers or network run while suspended; `/sys/power/wake_lock` keeps polling but drains battery. KOReader on Move: closing folio / power button while a takeover app runs can leave "Sleeping" modal stuck. Paper Pro: UNVERIFIED | VERIFIED [3P] (Move) / SNIPPET | L15, L21 |
| Wi-Fi | Wi-Fi and USB gadget go down in deep sleep; wake before SSH. Wi-Fi roam/reconnect latency after resume: no evidence found | VERIFIED [3P] (Move) | L15, L8 |
| TLS (matters for IMAP/SMTP) | `/etc/ssl/openssl.cnf.d/10-reduce-tls-ciphers.cnf` limits TLS 1.2 to ECDHE-ECDSA suites; hosts with RSA certs and no TLS 1.3 fail the handshake. A separate backend process can set `OPENSSL_CONF` (frontends inside xochitl inherit its policy). Observed on Move; Paper Pro UNVERIFIED | VERIFIED [3P] | L15 |
| Userland | BusyBox 1.36.1 (no curl, no python, no gcc); `wget` has its own TLS stack so it proves nothing about Qt TLS | VERIFIED [3P] | L15, L16 |
| Input | Paper Pro: `event2` = Elan marker (pen), `event3` = Elan touch (reversed vs rM2); resolve by name (`EVIOCGNAME`). Touch is Elan type-B multitouch, ABS_MT X 0..2064, Y 0..2832. Official guide: touch works out of the box; pen handling is "more involved" | VERIFIED [3P]; SNIPPET | L16, L6 |
| E Ink | See section 6 | - | - |

## 6. E Ink guidance for a Qt Quick app

| Rule | Basis | Status |
|---|---|---|
| No animations, gradients, large dark fills; every frame of a flick is a panel repaint, so avoid kinetic scrolling; page with buttons | MLB scoreboard PLATFORM.md; reTaskable ("discrete page turns") | VERIFIED [3P] |
| Black on white; mid-greys vanish (`#8A8A8A` unreadable, `#4A4A4A` ok); thin strokes wash out; use Bold not DemiBold; colour is muted | PLATFORM.md | VERIFIED [3P] |
| Tap targets in millimetres: ~3.5 mm unhittable, ~6.5 mm works; draw glyphs instead of font symbols (U+2715 is a blank box) | PLATFORM.md | VERIFIED [3P] |
| One present per page turn with a deep waveform, then a "settle" full-quality refresh; B&W fast mode; kill CSS animations | rmweb README | VERIFIED [3P] |
| Refresh mode switching and full-refresh requests from apps exist in AppLoad since v0.4.0 (release note truncated) | rm-appload releases | VERIFIED [upstream] (title only) |
| Scenegraph plugin `libqsgepaper.so` ships in the SDK and is selected by `QT_QUICK_BACKEND=epaper`; its partial-refresh API is not documented in anything I could read | SDK listing; snippets | VERIFIED listing / UNVERIFIED API |
| Design canvas: Paper Pro 1620x2160 (~229 dpi per old QPA constant 228); Move 954x1696. Do not hardcode | rmweb, PLATFORM.md, epaper-qpa header | VERIFIED [3P] |

## 7. Other apps and prior art (reuse)

| Project | What it does | Licence | Reusable? | Status |
|---|---|---|---|---|
| j6k4m8/epistolary | Python CLI: IMAP/SMTP -> PDF -> tablet via `rmapi`; handwritten reply -> OCR (Tesseract/OpenAI) -> SMTP. Runs on a PC, not on device. Last commit 2025-02-06; 62 stars; no attachments/inline images | Apache-2.0 | Ideas and IMAP code patterns only; not an on-device app | VERIFIED [upstream] |
| jhasubhash/rmbrowser (fork of exp78/rmweb v0.9.7) | WPE WebKit browser for Paper Pro, OS >= 3.28, AppLoad launch, takeover with no-brick launcher; CPU-only, no JS JIT | MIT | Reference for takeover launcher and E Ink present path; webmail in it is UNVERIFIED and likely slow | VERIFIED [upstream] |
| MaximeRivest chromium / riddle / paperterm / quill / store | App catalog on remagic; Chromium engine ~830 MB | MIT (remagic); each app repo not checked | Worked examples of AppLoad external apps and takeover | VERIFIED [upstream] (README) |
| snydersaurus/remarkable-mlb-scoreboard | AppLoad native QML + Rust/Qt backend; best written device notes | MIT | **Yes - reference architecture and docs** | VERIFIED [3P] |
| jdkruzr/reTaskable | CalDAV client, Rust backend + QML, AppLoad, Vellum/reManager install | GPL-3.0 | Pattern for sync client/settings; GPL | VERIFIED [upstream] |
| native IMAP/mail client for Paper Pro | **No evidence found** (searches: GitHub repo search "remarkable email imap" = 0 results; web search) | - | - | UNVERIFIED absence |
| other mail tools | `remailable` (j6k4m8, email->tablet PDF, Apache-2.0) and `PaperEmail` (sr.ht, rM2 server-side) - snippets only | - | - | SNIPPET |

## 8. Open items to verify on physical device

1. `cat /etc/os-release`, `/etc/version`, `ls -l /lib/libQt6Core.so.6*`: confirm OS build and device Qt (expect 6.10.3 on 3.28; 6.8.x on 3.27).
2. Which SDK to pin: confirm 5.8.203 installer URL/path from developer.remarkable.com/links (could not read) and its Qt version; decide between 5.7.119 (Qt 6.8.2, older = safe) and 5.8.203.
3. Does official `QT_QUICK_BACKEND=epaper ... -platform epaper` run on Paper Pro 3.28.x, and which `QT_QPA_EVDEV_*` / rotation values does Paper Pro need? Is the pen path usable from Qt events?
4. `systemctl stop xochitl` / start on Paper Pro: does `remarkable-fail.service` fire, is the passcode prompt repeated, how long is a restart?
5. AppLoad v0.6.0 + `rebuild_hashtable` on 3.28.0.172 (and the unknown 3.29.0.153): launches, stays stable (`NRestarts=0`), survives reboot via tripletap.
6. Native-QML AppLoad app built with SDK Qt 6.8.2 loads inside xochitl (Qt 6.10.3) - confirm no ABI errors; verify frontend/backend socket protocol on current AppLoad.
7. TLS: connect from both frontend and backend to the target IMAP/SMTP/Graph hosts; check `/etc/ssl/openssl.cnf.d` cipher restriction on Paper Pro; test `OPENSSL_CONF` override from backend.
8. Clock: `date`, `timedatectl`, `/etc/localtime`, NTP sync after resume, RTC drift; is time correct on first wake with Wi-Fi off?
9. Suspend/resume: time to deep suspend, Wi-Fi reconnect latency, wakelock effect on battery, behaviour with folio/Marker attached, background sync feasibility.
10. Secure storage: look for any keyring daemon, `keyctl`, TPM nodes (`/dev/tpm*`), and what `/home` encryption actually unlocks with (passcode?). Decide token storage policy.
11. E Ink: measure full-vs-partial refresh APIs available to a Qt app (`libqsgepaper` / AppLoad refresh-mode switching), ghosting on list updates, colour rendering, minimum legible grey.
12. Developer-mode warranty wording (official vs remagic) and OTA behaviour: does OTA remove dev mode, xovi, AppLoad apps in `/home/root`?
13. Licence review of AppLoad's GPL-3.0 QML module vs Slow Mail's chosen licence.
14. remagic PR #3 / commit `a99f90e`: has it merged? Re-check before relying on the one-line installer.
